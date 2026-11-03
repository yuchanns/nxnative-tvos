#pragma once
#include <math.h>
#include <stdint.h>

enum { DM_PERFORMANCE_HISTORY_CAPACITY = 64 };
typedef struct {
    double time, cpu_seconds;
    uint64_t frames;
    int valid;
} dm_performance_counter;
typedef struct { double fps, cpu_percent; } dm_performance_rates;
typedef struct { double time; uint64_t bytes; } dm_performance_point;
typedef struct {
    dm_performance_point points[DM_PERFORMANCE_HISTORY_CAPACITY];
    unsigned next, count;
} dm_performance_history;

// Whole-device CPU capacity is 100%, independent of the number of cores.
// A suspended app must establish a fresh baseline instead of averaging the gap.
static inline dm_performance_rates dm_performance_update(dm_performance_counter *previous,
        double time, uint64_t frames, double cpu_seconds, unsigned cores) {
    dm_performance_rates rates = {NAN, NAN};
    double elapsed = time - previous->time;
    if (previous->valid && elapsed > 0 && elapsed <= 2.5) {
        if (frames >= previous->frames) rates.fps = (frames - previous->frames) / elapsed;
        if (cores && isfinite(cpu_seconds) && isfinite(previous->cpu_seconds) && cpu_seconds >= previous->cpu_seconds)
            rates.cpu_percent = fmin(100, (cpu_seconds - previous->cpu_seconds) * 100 / (elapsed * cores));
    }
    *previous = (dm_performance_counter){time, cpu_seconds, frames, isfinite(time)};
    return rates;
}

static inline void dm_performance_push(dm_performance_history *history, double time, uint64_t bytes) {
    if (!isfinite(time)) return;
    if (history->count) {
        unsigned last = (history->next + DM_PERFORMANCE_HISTORY_CAPACITY - 1) % DM_PERFORMANCE_HISTORY_CAPACITY;
        if (time <= history->points[last].time) history->next = history->count = 0;
    }
    history->points[history->next] = (dm_performance_point){time, bytes};
    history->next = (history->next + 1) % DM_PERFORMANCE_HISTORY_CAPACITY;
    if (history->count < DM_PERFORMANCE_HISTORY_CAPACITY) history->count++;
}

static inline dm_performance_point dm_performance_at(const dm_performance_history *history, unsigned index) {
    unsigned first = (history->next + DM_PERFORMANCE_HISTORY_CAPACITY - history->count) % DM_PERFORMANCE_HISTORY_CAPACITY;
    return history->points[(first + index) % DM_PERFORMANCE_HISTORY_CAPACITY];
}
