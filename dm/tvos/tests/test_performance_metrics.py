"""Check HUD rates and bounded history without starting UIKit or a GPU."""

from pathlib import Path
import subprocess
import tempfile

SOURCES = Path(__file__).resolve().parents[1] / "Sources"
SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include "performance_metrics.h"

int main(void) {
    dm_performance_counter counter = {0};
    dm_performance_rates rates = dm_performance_update(&counter, 1, 100, 10, 4);
    assert(isnan(rates.fps) && isnan(rates.cpu_percent));
    rates = dm_performance_update(&counter, 2, 130, 11.5, 4);
    assert(rates.fps == 30 && rates.cpu_percent == 37.5);
    rates = dm_performance_update(&counter, 3, 160, 20, 4);
    assert(rates.fps == 30 && rates.cpu_percent == 100);
    rates = dm_performance_update(&counter, 30, 200, 21, 4);
    assert(isnan(rates.fps) && isnan(rates.cpu_percent));
    rates = dm_performance_update(&counter, 31, 230, 22, 4);
    assert(rates.fps == 30 && rates.cpu_percent == 25);
    rates = dm_performance_update(&counter, 32, 1, 1, 4);
    assert(isnan(rates.fps) && isnan(rates.cpu_percent));
    rates = dm_performance_update(&counter, 33, 31, NAN, 4);
    assert(rates.fps == 30 && isnan(rates.cpu_percent));
    rates = dm_performance_update(&counter, 34, 61, 2, 4);
    assert(rates.fps == 30 && isnan(rates.cpu_percent));
    rates = dm_performance_update(&counter, 35, 91, 3, 0);
    assert(rates.fps == 30 && isnan(rates.cpu_percent));
    rates = dm_performance_update(&counter, 35, 91, 3, 4);
    assert(isnan(rates.fps) && isnan(rates.cpu_percent));
    counter.valid = 0;
    rates = dm_performance_update(&counter, 36, 121, 4, 4);
    assert(isnan(rates.fps) && isnan(rates.cpu_percent));

    dm_performance_history history = {0};
    for (unsigned i = 0; i < 10000; i++) dm_performance_push(&history, i, (uint64_t)i << 20);
    assert(history.count == 64);
    assert(dm_performance_at(&history, 0).time == 9936);
    assert(dm_performance_at(&history, 63).time == 9999);
    unsigned recent = 0;
    for (unsigned i = 0; i < history.count; i++) {
        dm_performance_point point = dm_performance_at(&history, i);
        assert(point.bytes == (uint64_t)point.time << 20);
        if (point.time >= 9999 - 60) recent++;
    }
    assert(recent == 61);
    dm_performance_push(&history, NAN, 0); assert(history.count == 64);
    dm_performance_push(&history, 5, 42);
    assert(history.count == 1 && dm_performance_at(&history, 0).bytes == 42);
    puts("performance metrics: CPU normalization, pause/reset/error handling and bounded chronological history passed");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="nxnative-performance-") as directory:
        root = Path(directory)
        source, binary = root / "metrics.c", root / "metrics"
        source.write_text(SOURCE)
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-I", str(SOURCES), str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    main()
