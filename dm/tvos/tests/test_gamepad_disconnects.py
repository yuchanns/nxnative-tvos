"""Exercise the real controller HTTP server using CPU-only disconnected streams."""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile

DM = Path(__file__).resolve().parents[2]
SOURCE = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>
#include "process_signals.h"

static _Atomic unsigned send_failures;
static _Atomic int disconnect_before_send = -1;
static ssize_t observed_send(int fd, const void *bytes, size_t size, int flags) {
    int peer = atomic_exchange(&disconnect_before_send, -1);
    if (peer >= 0) {
        struct linger value = {.l_onoff = 1, .l_linger = 0};
        assert(setsockopt(peer, SOL_SOCKET, SO_LINGER, &value, sizeof value) == 0);
        close(peer);
        // Wait for the actual peer reset before the first reply, removing the TCP race.
        char byte;
        ssize_t received = recv(fd, &byte, 1, MSG_PEEK);
        assert(received == 0 || (received < 0 && errno == ECONNRESET));
    }
    ssize_t result = send(fd, bytes, size, flags);
    if (result < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN))
        atomic_fetch_add(&send_failures, 1);
    return result;
}
#define send observed_send
#include @GAMEPAD_SOURCE@
#undef send

void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    (void)buttons; (void)lx; (void)ly; (void)rx; (void)ry;
}
void dm_pad_clear(void) {}
uint64_t dm_input_samples(void) { return 0; }
NSDictionary *dm_tvos_cloud_save_status(void) { return @{}; }
static unsigned display_options = DM_TV_DISPLAY_FPS | DM_TV_DISPLAY_CLOUD_SAVE;
static unsigned display_writes;
unsigned dm_tvos_display_options(void) { return display_options; }
void dm_tvos_display_set_options(unsigned options) { display_options = options; display_writes++; }

static NSDictionary *test_assets;
static NSString *client_id = @"00000000000000000000000000000001";
static void *run_server(void *argument) {
    int fd = *(int *)argument;
    @autoreleasepool { serve(fd, @"Bearer test-only", test_assets); }
    close(fd);
    return NULL;
}
static void send_request(int fd, NSString *request) {
    NSData *data = [request dataUsingEncoding:NSUTF8StringEncoding];
    assert(send_bytes(fd, data.bytes, data.length));
}
static NSString *state_request(void) {
    NSString *body = [NSString stringWithFormat:
        @"{\"client\":\"%@\",\"seq\":1,\"buttons\":1,\"lx\":0,\"ly\":0,\"rx\":0,\"ry\":0}", client_id];
    return [NSString stringWithFormat:
        @"POST /state HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer test-only\r\nContent-Length: %lu\r\n\r\n%@",
        (unsigned long)[body lengthOfBytesUsingEncoding:NSUTF8StringEncoding], body];
}
static void tcp_pair(int pair[2]);
static void close_before_reply(NSString *request, int state) {
    int pair[2]; tcp_pair(pair);
    dm_tvos_gamepad_active(1); g_owner = client_id; g_sequence = 0;
    send_request(pair[1], request);
    atomic_store(&disconnect_before_send, pair[1]);
    unsigned before = atomic_load(&send_failures);
    serve(pair[0], @"Bearer test-only", test_assets);
    assert(atomic_load(&disconnect_before_send) == -1);
    assert(atomic_load(&send_failures) > before);
    if (state) assert(g_sequence == 1 && g_buttons == 1);
    close(pair[0]);
    dm_tvos_gamepad_active(0);
    assert(g_buttons == 0 && g_owner == nil);
}
static void tcp_pair(int pair[2]) {
    int listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    struct sockaddr_in address = {.sin_len = sizeof address, .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof address) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t size = sizeof address;
    assert(getsockname(listener, (struct sockaddr *)&address, &size) == 0);
    pair[1] = socket(AF_INET, SOCK_STREAM, 0); assert(pair[1] >= 0);
    assert(connect(pair[1], (struct sockaddr *)&address, size) == 0);
    pair[0] = accept(listener, NULL, NULL); assert(pair[0] >= 0);
    close(listener);
}
static void close_during_reply(int reset) {
    int pair[2]; tcp_pair(pair);
    int capacity = 4096;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof capacity) == 0);
    pthread_t worker;
    unsigned before = atomic_load(&send_failures);
    assert(pthread_create(&worker, NULL, run_server, &pair[0]) == 0);
    send_request(pair[1], @"GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n");
    char buffer[4096];
    assert(recv(pair[1], buffer, sizeof buffer, 0) > 0);
    if (reset) {
        struct linger value = {.l_onoff = 1, .l_linger = 0};
        assert(setsockopt(pair[1], SOL_SOCKET, SO_LINGER, &value, sizeof value) == 0);
    }
    close(pair[1]);
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load(&send_failures) > before);
}
static void healthy_request(void) {
    int pair[2]; tcp_pair(pair);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, run_server, &pair[0]) == 0);
    send_request(pair[1], @"GET /small HTTP/1.1\r\nHost: localhost\r\n\r\n");
    NSMutableData *response = [NSMutableData data];
    char buffer[1024]; ssize_t size;
    while ((size = recv(pair[1], buffer, sizeof buffer, 0)) > 0)
        [response appendBytes:buffer length:(NSUInteger)size];
    assert(size == 0);
    NSString *text = [[NSString alloc] initWithData:response encoding:NSUTF8StringEncoding];
    assert([text hasPrefix:@"HTTP/1.1 200"] && [text hasSuffix:@"healthy"]);
    close(pair[1]); assert(pthread_join(worker, NULL) == 0);
}
static int submit_display_position(NSString *client, NSNumber *sequence, id fps, id cloud, id position) {
    NSMutableDictionary *request = [@{@"client": client, @"seq": sequence, @"fps": fps, @"cloud_save": cloud} mutableCopy];
    if (position) request[@"position"] = position;
    return input_request(@"/display", [NSJSONSerialization dataWithJSONObject:request options:0 error:NULL]);
}
static int submit_display(NSString *client, NSNumber *sequence, id fps, id cloud) {
    return submit_display_position(client, sequence, fps, cloud, nil);
}
static void display_requests(void) {
    dm_tvos_gamepad_active(1); g_owner = client_id; g_sequence = 80; g_display_sequence = 0;
    g_buttons = 1; g_axes[2] = 1234;
    NSString *other = @"00000000000000000000000000000002";
    assert(submit_display(other, @1, @YES, @NO) == 409 && display_writes == 0);
    assert(submit_display(client_id, @1, @1, @NO) == 400 && display_writes == 0);
    assert(submit_display(client_id, @0, @YES, @NO) == 400 && display_writes == 0);
    assert(submit_display(client_id, @2, @NO, @YES) == 204);
    assert(display_options == DM_TV_DISPLAY_CLOUD_SAVE && display_writes == 1);
    assert(g_sequence == 80 && g_buttons == 1 && g_axes[2] == 1234);
    assert(submit_display(client_id, @1, @YES, @NO) == 204 && display_writes == 1);
    assert(submit_display(client_id, @2, @YES, @NO) == 204 && display_writes == 1);
    NSDictionary *state = [NSJSONSerialization JSONObjectWithData:status_data() options:0 error:NULL];
    assert(![state[@"display"][@"fps"] boolValue] && [state[@"display"][@"cloud_save"] boolValue]);
    assert([state[@"display"][@"seq"] intValue] == 2);
    assert([state[@"display"][@"position"] isEqual:@"top-left"]);
    for (id invalid in @[@"center", @1, NSNull.null])
        assert(submit_display_position(client_id, @3, @YES, @YES, invalid) == 400 && display_writes == 1);
    NSArray *positions = @[@"top-left", @"top-right", @"bottom-left", @"bottom-right"];
    for (unsigned i = 0; i < 4; i++) {
        assert(submit_display_position(client_id, @(3 + i), @YES, @YES, positions[i]) == 204);
        assert(display_options == (3u | (i << DM_TV_DISPLAY_POSITION_SHIFT)) && display_writes == 2 + i);
        state = [NSJSONSerialization JSONObjectWithData:status_data() options:0 error:NULL];
        assert([state[@"display"][@"position"] isEqual:positions[i]]);
    }
    assert(submit_display_position(client_id, @5, @NO, @NO, @"top-left") == 204 && display_options == 15 && display_writes == 5);
    // An older open controller may omit position while changing visibility.
    assert(submit_display(client_id, @7, @NO, @YES) == 204 && display_options == 14 && display_writes == 6);
    dm_tvos_gamepad_active(0);
    assert(submit_display(client_id, @8, @YES, @NO) == 409 && display_writes == 6);
    g_owner = client_id;
    assert(submit_display(client_id, @8, @YES, @NO) == 423 && display_writes == 6);
    dm_tvos_gamepad_active(0);
}
int main(int argc, char **argv) {
    assert(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
    alarm(30);
    if (argc > 1 && !strcmp(argv[1], "unprotected")) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        close(pair[1]); send(pair[0], "x", 1, 0);
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "global-policy")) assert(dm_process_signals_init() == 0);
    @autoreleasepool {
        test_assets = @{@"/large": @[[NSMutableData dataWithLength:8 << 20], @"application/octet-stream"],
                        @"/small": @[[@"healthy" dataUsingEncoding:NSUTF8StringEncoding], @"text/plain"]};
        for (int i = 0; i < 16; i++) {
            close_before_reply(@"GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n", 0);
            close_before_reply(state_request(), 1);
            close_during_reply(0);
            close_during_reply(1);
        }
        healthy_request();
        display_requests();
        printf("display controls: ownership, validation, ordering and input isolation passed\n");
        printf("controller disconnects: 64 failures handled, healthy request survives (%s)\n",
               argc > 1 ? argv[1] : "socket-policy-only");
    }
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DM / "tvos/Sources/gamepad.m")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nxnative-gamepad-test-") as directory:
        root = Path(directory)
        source, binary = root / "disconnects.m", root / "disconnects"
        source.write_text(SOURCE.replace("@GAMEPAD_SOURCE@", json.dumps(str(args.source.resolve()))))
        subprocess.run([
            "clang", "-std=gnu11", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-framework", "Foundation",
            "-I", str(DM / "host"), "-I", str(DM / "ios/Sources"),
            "-I", str(DM / "tvos/Sources"), str(source),
            str(DM / "tvos/Sources/listener.m"), "-o", str(binary),
        ], check=True)
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        result = subprocess.run([str(binary), "unprotected"], env=environment, timeout=40)
        assert result.returncode == -signal.SIGPIPE, result.returncode
        for mode in [[], ["global-policy"]]:
            subprocess.run([str(binary), *mode], check=True, env=environment, timeout=40)


if __name__ == "__main__":
    main()
