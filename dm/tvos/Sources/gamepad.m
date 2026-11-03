// A local browser controller uses its own input channel and a short state lease.
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "dm_input.h"
#include "gamepad.h"
#include "display.h"
#import "cloud_save.h"
#import "listener.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static NSString *g_owner;
static uint64_t g_sequence, g_display_sequence, g_last_ns;
static uint32_t g_buttons;
static int32_t g_axes[4];
static int g_active;
static _Atomic unsigned g_page_requests;
static dispatch_source_t g_watchdog;
static NSString *const display_positions[] = {@"top-left", @"top-right", @"bottom-left", @"bottom-right"};

static uint64_t monotonic_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static void release_input(void) { // Called under g_mu, including by the lease timer.
    g_buttons = 0; memset(g_axes, 0, sizeof g_axes); g_last_ns = 0;
    dm_pad_clear();
}
void dm_tvos_gamepad_active(int active) {
    pthread_mutex_lock(&g_mu);
    g_active = active;
    if (!active) { g_owner = nil; release_input(); }
    pthread_mutex_unlock(&g_mu);
}
static int integer(id value, int64_t low, int64_t high, int64_t *out) {
    if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return 0;
    double n = [value doubleValue];
    if (!isfinite(n) || n < low || n > high || n != floor(n)) return 0;
    *out = (int64_t)n; return 1;
}
static int client_valid(id client) {
    if (![client isKindOfClass:NSString.class] || [client length] != 32) return 0;
    return [client rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location == NSNotFound;
}
static int input_request(NSString *route, NSData *body) {
    id data = [NSJSONSerialization JSONObjectWithData:body options:0 error:NULL];
    if (![data isKindOfClass:NSDictionary.class] || !client_valid(data[@"client"])) return 400;
    int connect = [route isEqualToString:@"/connect"], release = [route isEqualToString:@"/release"];
    int display = [route isEqualToString:@"/display"];
    if (!connect && !release && !display && ![route isEqualToString:@"/state"]) return 404;
    int64_t sequence = 0, buttons = 0, axes[4] = {0};
    int position = -1;
    if (display) {
        if (!integer(data[@"seq"], 1, 9007199254740991ll, &sequence)) return 400;
        for (NSString *key in @[@"fps", @"cloud_save"]) {
            if (![data[key] isKindOfClass:NSNumber.class] ||
                CFGetTypeID((__bridge CFTypeRef)data[key]) != CFBooleanGetTypeID()) return 400;
        }
        if (data[@"position"]) {
            if (![data[@"position"] isKindOfClass:NSString.class]) return 400;
            for (unsigned i = 0; i < 4; i++) if ([data[@"position"] isEqual:display_positions[i]]) position = (int)i;
            if (position < 0) return 400;
        }
    } else if (!connect && !release) {
        if (!integer(data[@"seq"], 1, 9007199254740991ll, &sequence) || !integer(data[@"buttons"], 0, 65535, &buttons)) return 400;
        NSArray *keys = @[@"lx", @"ly", @"rx", @"ry"];
        for (int i = 0; i < 4; i++) if (!integer(data[keys[i]], -DM_STICK_MAX, DM_STICK_MAX, &axes[i])) return 400;
    }
    pthread_mutex_lock(&g_mu);
    int code = 204;
    if (connect) {
        if (!g_active) code = 423;
        else { release_input(); g_owner = [data[@"client"] copy]; g_sequence = g_display_sequence = 0; }
    } else if (![g_owner isEqualToString:data[@"client"]]) code = 409;
    else if (release) { g_owner = nil; release_input(); }
    else if (!g_active) code = 423;
    else if (display) {
        if ((uint64_t)sequence > g_display_sequence) {
            unsigned options = ([data[@"fps"] boolValue] ? DM_TV_DISPLAY_FPS : 0) |
                ([data[@"cloud_save"] boolValue] ? DM_TV_DISPLAY_CLOUD_SAVE : 0);
            options |= position >= 0 ? (unsigned)position << DM_TV_DISPLAY_POSITION_SHIFT :
                dm_tvos_display_options() & DM_TV_DISPLAY_POSITION_MASK;
            dm_tvos_display_set_options(options);
            g_display_sequence = (uint64_t)sequence;
        }
    }
    else if ((uint64_t)sequence > g_sequence) {
        // Complete snapshots plus a session ID reject delayed packets from an old tab.
        g_sequence = (uint64_t)sequence; g_last_ns = monotonic_ns(); g_buttons = (uint32_t)buttons;
        for (int i = 0; i < 4; i++) g_axes[i] = (int32_t)axes[i];
        dm_pad_set(g_buttons, g_axes[0], g_axes[1], g_axes[2], g_axes[3]);
    }
    pthread_mutex_unlock(&g_mu);
    return code;
}
static NSData *status_data(void) {
    pthread_mutex_lock(&g_mu);
    unsigned display = dm_tvos_display_options();
    NSMutableDictionary *state = [@{@"active": @(g_active), @"connected": @(g_owner != nil && g_last_ns != 0),
        @"buttons": @(g_buttons), @"axes": @[@(g_axes[0]), @(g_axes[1]), @(g_axes[2]), @(g_axes[3])],
        @"display": @{@"fps": @((display & DM_TV_DISPLAY_FPS) != 0),
                      @"cloud_save": @((display & DM_TV_DISPLAY_CLOUD_SAVE) != 0),
                      @"position": display_positions[(display & DM_TV_DISPLAY_POSITION_MASK) >> DM_TV_DISPLAY_POSITION_SHIFT],
                      @"seq": @(g_display_sequence)},
        @"sequence": @(g_sequence), @"input_samples": @(dm_input_samples())} mutableCopy];
    pthread_mutex_unlock(&g_mu);
    state[@"cloud_save"] = dm_tvos_cloud_save_status();
    return [NSJSONSerialization dataWithJSONObject:state options:0 error:NULL];
}
static int send_bytes(int fd, const void *data, size_t size) {
    const uint8_t *p = data;
    while (size) {
        ssize_t n = send(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n; size -= (size_t)n;
    }
    return 1;
}
static int respond(int fd, int code, NSData *body, NSString *type, int keep, int head_only) {
    NSString *header = [NSString stringWithFormat:
        @"HTTP/1.1 %d Result\r\nContent-Length: %lu\r\nContent-Type: %@\r\nCache-Control: no-store\r\n"
         "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
         "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; media-src data:; manifest-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\n"
         "Connection: %@\r\n\r\n", code, (unsigned long)body.length, type, keep ? @"keep-alive" : @"close"];
    NSData *head = [header dataUsingEncoding:NSUTF8StringEncoding];
    return send_bytes(fd, head.bytes, head.length) && (head_only || send_bytes(fd, body.bytes, body.length));
}
static void serve(int fd, NSString *authorization, NSDictionary *assets) {
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes)) return;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    char bytes[8193]; size_t used = 0;
    for (;;) { @autoreleasepool {
        size_t end = 0;
        while (!end) {
            bytes[used] = 0;
            char *boundary = strstr(bytes, "\r\n\r\n");
            if (boundary) { end = (size_t)(boundary - bytes) + 4; break; }
            if (used == sizeof bytes - 1) return;
            ssize_t n = recv(fd, bytes + used, sizeof bytes - 1 - used, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            used += (size_t)n;
        }
        NSString *head = [[NSString alloc] initWithBytes:bytes length:end encoding:NSASCIIStringEncoding];
        NSArray *lines = [head componentsSeparatedByString:@"\r\n"];
        NSArray *request = [lines.firstObject componentsSeparatedByString:@" "];
        if (request.count != 3 || ![request[2] isEqualToString:@"HTTP/1.1"]) return;
        NSMutableDictionary *headers = [NSMutableDictionary dictionary];
        for (NSUInteger i = 1; i < lines.count; i++) {
            NSString *line = lines[i]; if (!line.length) continue;
            NSRange colon = [line rangeOfString:@":"]; if (colon.location == NSNotFound) return;
            NSString *key = [[line substringToIndex:colon.location] lowercaseString];
            if (headers[key]) return;
            headers[key] = [[line substringFromIndex:colon.location + 1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        }
        size_t length = 0;
        if (headers[@"transfer-encoding"]) return;
        if (headers[@"content-length"]) {
            const char *value = [headers[@"content-length"] UTF8String]; char *tail = NULL;
            unsigned long long n = strtoull(value, &tail, 10);
            if (!*value || *tail || n > 512) return;
            length = (size_t)n;
        }
        if (end + length > sizeof bytes - 1) return;
        while (used < end + length) {
            ssize_t n = recv(fd, bytes + used, sizeof bytes - 1 - used, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            used += (size_t)n;
        }
        NSString *method = request[0];
        NSString *route = [[request[1] componentsSeparatedByString:@"?"] firstObject], *type = @"application/json";
        int head_only = [method isEqualToString:@"HEAD"];
        int code = 404; NSData *body = [NSData data];
        NSArray *asset = assets[route];
        if (([method isEqualToString:@"GET"] || head_only) && asset) {
            code = 200; body = asset[0]; type = asset[1];
            // Record navigation only; never log pairing fragments, headers or input traffic.
            if ([type hasPrefix:@"text/html"] && atomic_fetch_add(&g_page_requests, 1) < 32)
                fprintf(stderr, "phone controller: page request %s (%lu bytes)\n", method.UTF8String, (unsigned long)body.length);
        } else if (![headers[@"authorization"] isEqualToString:authorization]) code = 401;
        else if (headers[@"origin"] && ![headers[@"origin"] isEqualToString:[@"http://" stringByAppendingString:headers[@"host"] ?: @""]]) code = 403;
        else if ([method isEqualToString:@"POST"]) code = input_request(route, [NSData dataWithBytes:bytes + end length:length]);
        else if ([method isEqualToString:@"GET"] && [route isEqualToString:@"/status"]) { code = 200; body = status_data(); }
        int keep = code == 204 || (code == 200 && !asset);
        if ([[headers[@"connection"] lowercaseString] isEqualToString:@"close"]) keep = 0;
        if (!respond(fd, code, body, type, keep, head_only) || !keep) return;
        size_t consumed = end + length; used -= consumed; memmove(bytes, bytes + consumed, used);
    } }
}
void dm_tvos_gamepad_start(void) {
    const char *port_value = getenv("DM_TV_GAMEPAD_PORT"), *token = getenv("DM_TV_GAMEPAD_TOKEN");
    int port = port_value ? atoi(port_value) : 0;
    if (port < 1 || port > 65535 || !token || strlen(token) < 32) return;
    NSData *page = [NSData dataWithContentsOfFile:[NSBundle.mainBundle pathForResource:@"gamepad" ofType:@"html"]];
    if (!page) { fprintf(stderr, "[RED] phone controller page is missing\n"); return; }
    NSMutableDictionary *resources = [NSMutableDictionary dictionaryWithObject:@[page, @"text/html; charset=utf-8"] forKey:@"/"];
    resources[@"/gamepad.html"] = resources[@"/"];
    for (NSArray *resource in @[@[@"controller-theme", @"css", @"text/css; charset=utf-8"],
                                 @[@"controller-skin", @"jpg", @"image/jpeg"],
                                 @[@"controller-icon", @"png", @"image/png"],
                                 @[@"controller", @"webmanifest", @"application/manifest+json"]]) {
        NSData *data = [NSData dataWithContentsOfFile:[NSBundle.mainBundle pathForResource:resource[0] ofType:resource[1]]];
        if (data) resources[[NSString stringWithFormat:@"/%@.%@", resource[0], resource[1]]] = @[data, resource[2]];
    }
    NSDictionary *assets = [resources copy];
    NSString *authorization = [NSString stringWithFormat:@"Bearer %s", token];
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    g_watchdog = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(g_watchdog, DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_watchdog, ^{
        pthread_mutex_lock(&g_mu);
        if (g_last_ns && monotonic_ns() - g_last_ns > 350000000ull) release_input();
        pthread_mutex_unlock(&g_mu);
    });
    dispatch_resume(g_watchdog);
    dm_tvos_listener_start("phone controller", port, 8, queue, ^(int fd) {
        serve(fd, authorization, assets);
    });
}
