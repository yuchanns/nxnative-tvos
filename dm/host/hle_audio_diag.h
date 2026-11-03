#ifndef DM_AUDIO_DIAG_H
#define DM_AUDIO_DIAG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int id, channels, state, backend, host_time_valid;
    uint64_t callback_ns, callbacks, queued_bytes, oldest_enqueue_ns;
    int64_t callback_lead_ns;
} dm_audio_diagnostic;

// Samples describe the queue before its last real output callback consumed it.
// They do not include the output route's downstream hardware latency.
size_t dm_audio_snapshot(dm_audio_diagnostic *output, size_t capacity);

#endif
