#include "../nvn/nvn_geometry_indices.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "line %d: expected %llu, got %llu\n", __LINE__, (unsigned long long)w, (unsigned long long)g); failures++; } } while (0)

int main(void) {
    uint32_t raw[] = {65535, 3, 1, 3, 65535, 9, 10, 11, 65535};
    uint32_t unique[9], dense[9], count = 123, output[24];
    CHECK(1, nvn_geometry_dense_indices(raw, 9, -1, 65535, unique, dense, &count));
    CHECK(5, count);
    const uint32_t expected_unique[] = {1, 3, 9, 10, 11};
    const uint32_t expected_dense[] = {UINT32_MAX, 1, 0, 1, UINT32_MAX, 2, 3, 4, UINT32_MAX};
    CHECK(0, memcmp(expected_unique, unique, sizeof expected_unique));
    CHECK(0, memcmp(expected_dense, dense, sizeof expected_dense));
    CHECK(2, nvn_geometry_index_primitives(5, dense, 9, output));
    const uint32_t strips[] = {1, 0, 1, 2, 3, 4}; CHECK(0, memcmp(strips, output, sizeof strips));
    CHECK(2, nvn_geometry_input_count(5, 4));
    nvn_geometry_input_indices(5, 4, 1, output);
    const uint32_t winding[] = {1, 3, 2}; CHECK(0, memcmp(winding, output, sizeof winding));
    nvn_geometry_input_indices(6, 5, 2, output);
    const uint32_t fan[] = {3, 4, 0}; CHECK(0, memcmp(fan, output, sizeof fan));
    nvn_geometry_input_indices(13, 6, 0, output);
    const uint32_t adjacency[] = {0, 1, 2, 5, 4, 3}; CHECK(0, memcmp(adjacency, output, sizeof adjacency));
    nvn_geometry_input_indices(13, 8, 1, output);
    const uint32_t adjacency_last[] = {2, 5, 6, 7, 4, 0}; CHECK(0, memcmp(adjacency_last, output, sizeof adjacency_last));
    uint32_t all_restart[] = {65535, 65535};
    CHECK(1, nvn_geometry_dense_indices(all_restart, 2, 0, 65535, unique, dense, &count)); CHECK(0, count);
    CHECK(0, nvn_geometry_index_primitives(5, dense, 2, output));
    count = 123; uint32_t too_low[] = {1};
    CHECK(0, nvn_geometry_dense_indices(too_low, 1, -2, UINT64_MAX, unique, dense, &count)); CHECK(123, count);
    uint32_t too_high[] = {UINT32_MAX};
    CHECK(0, nvn_geometry_dense_indices(too_high, 1, 1, UINT64_MAX, unique, dense, &count)); CHECK(123, count);
    for (unsigned primitive = 0; primitive < 14; primitive++) {
        unsigned width = nvn_geometry_input_width(primitive); if (!width) continue;
        for (uint32_t vertices = 0; vertices < 64; vertices++) {
            uint32_t primitives = nvn_geometry_input_count(primitive, vertices);
            for (uint32_t index = 0; index < primitives; index++) {
                nvn_geometry_input_indices(primitive, vertices, index, output);
                for (unsigned slot = 0; slot < width; slot++) CHECK(1, output[slot] < vertices);
            }
        }
    }
    fprintf(stderr, "t_geometry_indices: %u checks, %u failed\n", checks, failures);
    return failures != 0;
}
