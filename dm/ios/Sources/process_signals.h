#ifndef DM_PROCESS_SIGNALS_H
#define DM_PROCESS_SIGNALS_H

#include <signal.h>
#include <stddef.h>

static inline int dm_process_signals_init(void) {
    // Disconnected streams are I/O errors, not a request to terminate the app.
    // Install before worker threads so framework-owned pipes are covered too.
    struct sigaction action = {.sa_handler = SIG_IGN};
    sigemptyset(&action.sa_mask);
    return sigaction(SIGPIPE, &action, NULL);
}

#endif
