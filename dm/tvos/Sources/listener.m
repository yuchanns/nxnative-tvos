// Suspension can reclaim a listening socket without a usable error notification.
// Own its lifecycle explicitly, rather than keeping a blocking accept alive.
#import "listener.h"
#include <TargetConditionals.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#if TARGET_OS_TV
#import <UIKit/UIKit.h>
#endif

@interface DMTVListener : NSObject {
@public
    // All mutable state, descriptor closes and lifecycle events use the main queue.
    NSString *_name;
    int _port, _fd, _last_error;
    unsigned _limit, _failures;
    BOOL _enabled, _closing, _retry_pending;
    uint64_t _generation;
    NSTimeInterval _retry_delay;
    dispatch_source_t _source;
    dispatch_queue_t _workers;
    void (^_handler)(int);
    NSMutableSet<NSNumber *> *_clients;
    NSMutableArray *_observers;
}
- (instancetype)initWithName:(NSString *)name port:(int)port limit:(unsigned)limit
                      workers:(dispatch_queue_t)workers handler:(void (^)(int))handler;
- (void)setEnabled:(BOOL)enabled;
- (void)openIfNeeded;
- (void)acceptReady;
- (void)retryOperation:(const char *)operation error:(int)error;
@end

@implementation DMTVListener
- (instancetype)initWithName:(NSString *)name port:(int)port limit:(unsigned)limit
                      workers:(dispatch_queue_t)workers handler:(void (^)(int))handler {
    if ((self = [super init])) {
        _name = [name copy]; _port = port; _limit = limit; _fd = -1;
        _workers = workers; _handler = [handler copy]; _retry_delay = 0.25;
        _clients = [NSMutableSet set]; _observers = [NSMutableArray array];
    }
    return self;
}
- (void)closeListener {
    if (!_source) return;
    dispatch_source_t source = _source;
    _source = nil; _fd = -1; _closing = YES;
    // The cancellation handler is the only owner of the listening descriptor's
    // close; reopening waits for it, avoiding descriptor reuse and bind races.
    dispatch_source_cancel(source);
}
- (void)setEnabled:(BOOL)enabled {
    if (_enabled == enabled) return;
    _enabled = enabled; _generation++; _retry_pending = NO; _retry_delay = 0.25;
    if (enabled) [self openIfNeeded];
    else {
        [self closeListener];
        // Workers may be blocked in recv/send. Shutdown wakes them; only their
        // completion on this queue closes the descriptor, so it cannot be reused.
        for (NSNumber *client in _clients) shutdown(client.intValue, SHUT_RDWR);
        fprintf(stderr, "tvOS %s listener paused\n", _name.UTF8String);
    }
}
- (void)retryOperation:(const char *)operation error:(int)error {
    _failures++;
    if (_last_error != error || !(_failures & (_failures - 1)))
        fprintf(stderr, "tvOS %s listener %s failed: errno=%d; retry in %.2fs\n",
                _name.UTF8String, operation, error, _retry_delay);
    _last_error = error; _retry_pending = YES;
    uint64_t generation = ++_generation;
    NSTimeInterval delay = _retry_delay; _retry_delay = MIN(delay * 2, 5.0);
    [self closeListener];
    __weak DMTVListener *weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        DMTVListener *listener = weak;
        if (!listener || !listener->_enabled || listener->_generation != generation) return;
        listener->_retry_pending = NO;
        [listener openIfNeeded];
    });
}
- (void)openIfNeeded {
    if (!_enabled || _source || _closing || _retry_pending) return;
    int fd = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    if (fd < 0) { [self retryOperation:"socket" error:errno]; return; }
    struct sockaddr_in address = {.sin_len = sizeof address, .sin_family = AF_INET,
        .sin_port = htons(_port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) ||
        fcntl(fd, F_SETFL, O_NONBLOCK) ||
        bind(fd, (struct sockaddr *)&address, sizeof address) || listen(fd, MAX(_limit, 4))) {
        int error = errno; close(fd); [self retryOperation:"open" error:error]; return;
    }
    _fd = fd;
    _source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
    if (!_source) {
        _fd = -1; close(fd); [self retryOperation:"source" error:ENOMEM]; return;
    }
    __weak DMTVListener *weak = self;
    dispatch_source_set_event_handler(_source, ^{
        DMTVListener *listener = weak;
        if (listener && listener->_fd == fd) [listener acceptReady];
    });
    dispatch_source_set_cancel_handler(_source, ^{
        close(fd);
        DMTVListener *listener = weak;
        if (listener) { listener->_closing = NO; [listener openIfNeeded]; }
    });
    dispatch_resume(_source);
    fprintf(stderr, "tvOS %s listening on port %d\n", _name.UTF8String, _port);
}
- (void)acceptReady {
    // Bound work on the UI queue even under a burst of clients or interrupted accepts.
    for (unsigned count = 0; count < 16; count++) {
        int fd = accept(_fd, NULL, NULL);
        if (fd < 0) {
            int error = errno;
            if (error == EINTR) continue;
            if (error == EAGAIN || error == EWOULDBLOCK) return;
            [self retryOperation:"accept" error:error]; return;
        }
        if (_clients.count >= _limit || fcntl(fd, F_SETFL, 0)) { close(fd); continue; }
        _failures = 0; _last_error = 0; _retry_delay = 0.25;
        [_clients addObject:@(fd)];
        dispatch_async(_workers, ^{ @autoreleasepool {
            self->_handler(fd);
            dispatch_async(dispatch_get_main_queue(), ^{
                [self->_clients removeObject:@(fd)]; close(fd);
            });
        } });
    }
}
@end

void dm_tvos_listener_start(const char *name, int port, unsigned max_connections,
                            dispatch_queue_t workers, void (^handler)(int)) {
    NSString *label = @(name);
    dispatch_async(dispatch_get_main_queue(), ^{
        static NSMutableArray *listeners;
        if (!listeners) listeners = [NSMutableArray array];
        DMTVListener *listener = [[DMTVListener alloc] initWithName:label port:port limit:max_connections
                                                        workers:workers handler:handler];
        [listeners addObject:listener];
#if TARGET_OS_TV
        __weak DMTVListener *weak = listener;
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        [listener->_observers addObject:[center addObserverForName:UIApplicationDidEnterBackgroundNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
                (void)note; [weak setEnabled:NO];
            }]];
        [listener->_observers addObject:[center addObserverForName:UIApplicationWillEnterForegroundNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
                (void)note; [weak setEnabled:YES];
            }]];
        [listener setEnabled:UIApplication.sharedApplication.applicationState != UIApplicationStateBackground];
#else
        [listener setEnabled:YES];
#endif
    });
}
