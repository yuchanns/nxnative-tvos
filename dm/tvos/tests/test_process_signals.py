"""Check startup signal policy against real disconnected CPU-only streams."""

import os
from pathlib import Path
import subprocess
import tempfile

DM = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include "process_signals.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void *broken_streams(void *unused) {
    (void)unused;
    int descriptors[2]; char byte = 'x', received;
    assert(pipe(descriptors) == 0);
    close(descriptors[0]); errno = 0;
    assert(write(descriptors[1], &byte, 1) == -1 && errno == EPIPE);
    close(descriptors[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    assert(send(descriptors[0], &byte, 1, 0) == 1);
    assert(read(descriptors[1], &received, 1) == 1 && received == byte);
    close(descriptors[1]); errno = 0;
    assert(send(descriptors[0], &byte, 1, 0) == -1 && errno == EPIPE);
    close(descriptors[0]);
    return NULL;
}
int main(void) {
    for (int protected = 0; protected < 2; protected++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            assert(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
            struct sigaction before, after;
            assert(sigaction(SIGTERM, NULL, &before) == 0);
            if (protected) assert(dm_process_signals_init() == 0);
            assert(sigaction(SIGTERM, NULL, &after) == 0);
            assert(before.sa_handler == after.sa_handler && before.sa_flags == after.sa_flags);
            broken_streams(NULL);
            pthread_t thread;
            assert(pthread_create(&thread, NULL, broken_streams, NULL) == 0);
            assert(pthread_join(thread, NULL) == 0);
            _exit(0);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        if (protected) assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        else assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE);
    }
    puts("process signals: disconnected pipes and sockets return EPIPE without terminating main or worker threads");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="nxnative-signal-test-") as directory:
        root = Path(directory)
        source, binary = root / "signals.c", root / "signals"
        source.write_text(SOURCE)
        subprocess.run([
            "clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-I", str(DM / "ios/Sources"),
            str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    main()
