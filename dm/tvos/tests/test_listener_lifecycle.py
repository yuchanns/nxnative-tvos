"""Exercise the real GCD/TCP listener on the CPU, with injected socket failures."""

import json
import os
from pathlib import Path
import subprocess
import tempfile

DM = Path(__file__).resolve().parents[2]
SOURCE = r'''
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

static _Atomic int next_accept_error, next_socket_error;
static _Atomic unsigned socket_calls;
static int faulty_accept(int fd, struct sockaddr *address, socklen_t *length) {
    int error = atomic_exchange(&next_accept_error, 0);
    if (error) { errno = error; return -1; }
    return accept(fd, address, length);
}
static int faulty_socket(int domain, int type, int protocol) {
    atomic_fetch_add(&socket_calls, 1);
    int error = atomic_exchange(&next_socket_error, 0);
    if (error) { errno = error; return -1; }
    return socket(domain, type, protocol);
}
#define accept faulty_accept
#define socket faulty_socket
#include @LISTENER_SOURCE@
#undef socket
#undef accept

static void on_main(dispatch_block_t block) { dispatch_sync(dispatch_get_main_queue(), block); }
static void until(BOOL (^condition)(void)) {
    for (unsigned i = 0; i < 600; i++) {
        __block BOOL ready;
        on_main(^{ ready = condition(); });
        if (ready) return;
        usleep(10000);
    }
    assert(!"listener state timed out");
}
static int reserve_port(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    assert(fd >= 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == 0);
    struct sockaddr_in address = {.sin_len = sizeof address, .sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(fd, (struct sockaddr *)&address, sizeof address) == 0);
    socklen_t size = sizeof address;
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    assert(listen(fd, 4) == 0); *port = ntohs(address.sin_port); return fd;
}
static int connect_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    assert(fd >= 0);
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    struct sockaddr_in address = {.sin_len = sizeof address, .sin_family = AF_INET,
        .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&address, sizeof address)) { close(fd); return -1; }
    return fd;
}
static void healthy(int port, DMTVListener *listener) {
    int fd = connect_port(port); assert(fd >= 0);
    assert(send(fd, "x", 1, 0) == 1);
    char byte = 0; assert(recv(fd, &byte, 1, 0) == 1 && byte == 'x'); close(fd);
    until(^BOOL{ return listener->_clients.count == 0; });
}
static void disabled(DMTVListener *listener, int port) {
    on_main(^{ [listener setEnabled:NO]; });
    until(^BOOL{ return !listener->_closing && !listener->_source && listener->_clients.count == 0; });
    if (port > 0) assert(connect_port(port) == -1);
}
static void run_tests(void) {
    int port; int reserved = reserve_port(&port); close(reserved);
    __block DMTVListener *listener;
    on_main(^{
        listener = [[DMTVListener alloc] initWithName:@"test" port:port limit:2
            workers:dispatch_get_global_queue(QOS_CLASS_UTILITY, 0) handler:^(int fd) {
                int yes = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
                char byte;
                if (recv(fd, &byte, 1, 0) == 1) send(fd, &byte, 1, 0);
            }];
        [listener setEnabled:YES];
    });
    healthy(port, listener);
    for (unsigned i = 0; i < 64; i++) {
        int client = connect_port(port); assert(client >= 0);
        until(^BOOL{ return listener->_clients.count == 1; });
        disabled(listener, port);
        char byte; assert(recv(client, &byte, 1, 0) == 0); close(client);
        int sentinel = open("/dev/null", O_RDONLY); assert(sentinel >= 0);
        on_main(^{
            [listener setEnabled:YES];
            [listener setEnabled:NO];
            [listener setEnabled:YES];
        });
        until(^BOOL{ return listener->_fd >= 0; });
        healthy(port, listener);
        assert(fcntl(sentinel, F_GETFD) >= 0); close(sentinel);
    }
    disabled(listener, port);
    on_main(^{
        listener->_limit = 4;
        listener->_workers = dispatch_queue_create("test.serial-requests", DISPATCH_QUEUE_SERIAL);
        [listener setEnabled:YES];
    });
    for (unsigned i = 0; i < 100; i++) {
        int fd = connect_port(port); assert(fd >= 0);
        char value = 0; assert(send(fd, "s", 1, 0) == 1);
        assert(recv(fd, &value, 1, 0) == 1 && value == 's'); close(fd);
        // Deliberately do not wait for the main-queue connection cleanup.
    }
    until(^BOOL{ return listener->_clients.count == 0; });
    disabled(listener, port);
    on_main(^{
        listener->_limit = 2;
        listener->_workers = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
        [listener setEnabled:YES];
    });
    int first = connect_port(port), second = connect_port(port);
    assert(first >= 0 && second >= 0);
    until(^BOOL{ return listener->_clients.count == 2; });
    int excess = connect_port(port); assert(excess >= 0);
    char byte; assert(recv(excess, &byte, 1, 0) == 0); close(excess);
    disabled(listener, port); close(first); close(second);
    on_main(^{ [listener setEnabled:YES]; });
    healthy(port, listener);
    int errors[] = {EINTR, EAGAIN, ECONNABORTED, EBADF, EINVAL, ENETDOWN};
    for (unsigned i = 0; i < sizeof errors / sizeof *errors; i++) {
        atomic_store(&next_accept_error, errors[i]);
        int pending = connect_port(port); assert(pending >= 0);
        until(^BOOL{ return atomic_load(&next_accept_error) == 0; });
        close(pending);
        until(^BOOL{ return listener->_fd >= 0 && !listener->_retry_pending && listener->_clients.count == 0; });
        healthy(port, listener);
    }
    disabled(listener, port);
    atomic_store(&next_socket_error, EMFILE);
    on_main(^{ [listener setEnabled:YES]; });
    until(^BOOL{ return listener->_fd >= 0; });
    healthy(port, listener);
    disabled(listener, port);
    int occupied = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    assert(occupied >= 0); setsockopt(occupied, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in address = {.sin_len = sizeof address, .sin_family = AF_INET,
        .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    assert(bind(occupied, (struct sockaddr *)&address, sizeof address) == 0);
    assert(listen(occupied, 4) == 0);
    unsigned before = atomic_load(&socket_calls);
    on_main(^{ [listener setEnabled:YES]; });
    usleep(900000);
    unsigned attempts = atomic_load(&socket_calls) - before;
    assert(attempts >= 2 && attempts <= 4);
    disabled(listener, -1);
    close(occupied);
    usleep(1200000);
    on_main(^{ assert(listener->_fd == -1 && !listener->_retry_pending); [listener setEnabled:YES]; });
    healthy(port, listener);
    disabled(listener, port);
    puts("listener lifecycle: 64 sleep/resume cycles, 100 serial requests, client shutdown/limit, accept/socket faults and bounded cancelled retries passed");
}
int main(void) {
    alarm(40);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ @autoreleasepool {
        run_tests(); exit(0);
    } });
    dispatch_main();
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="nxnative-listener-test-") as directory:
        root = Path(directory)
        source, binary = root / "listener.m", root / "listener"
        source.write_text(SOURCE.replace(
            "@LISTENER_SOURCE@", json.dumps(str(DM / "tvos/Sources/listener.m"))
        ))
        subprocess.run([
            "clang", "-std=gnu11", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-framework", "Foundation",
            "-I", str(DM / "tvos/Sources"), str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=45,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    main()
