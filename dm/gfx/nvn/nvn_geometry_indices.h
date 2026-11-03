#pragma once
#include <stdint.h>
#include <stdlib.h>

#include "nvn_geometry_topology.h"

static inline int nvn_geometry_compare_index(const void *left,
                                                 const void *right) {
  uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
  return (a > b) - (a < b);
}

static inline uint32_t nvn_geometry_lower_index(const uint32_t *values,
                                                   uint32_t count,
                                                   uint32_t value) {
  uint32_t begin = 0, end = count;
  while (begin < end) {
    uint32_t middle = begin + (end - begin) / 2;
    if (values[middle] < value) begin = middle + 1;
    else end = middle;
  }
  return begin;
}

static inline int nvn_geometry_dense_indices(const uint32_t *raw,
                                                 uint32_t count,
                                                 int32_t base_vertex,
                                                 uint64_t restart,
                                                 uint32_t *unique,
                                                 uint32_t *dense,
                                                 uint32_t *unique_count) {
  uint32_t used = 0;
  for (uint32_t index = 0; index < count; index++) {
    if (raw[index] == restart) continue;
    int64_t adjusted = (int64_t)raw[index] + base_vertex;
    if (adjusted < 0 || adjusted > UINT32_MAX) return 0;
    unique[used++] = raw[index];
  }
  if (used) qsort(unique, used, sizeof *unique, nvn_geometry_compare_index);
  uint32_t compact = 0;
  for (uint32_t index = 0; index < used; index++)
    if (!compact || unique[index] != unique[compact - 1])
      unique[compact++] = unique[index];
  for (uint32_t index = 0; index < count; index++)
    dense[index] = raw[index] == restart ? UINT32_MAX
        : nvn_geometry_lower_index(unique, compact, raw[index]);
  *unique_count = compact;
  return 1;
}

static inline uint32_t nvn_geometry_index_primitives(unsigned primitive,
                                                        const uint32_t *dense,
                                                        uint32_t count,
                                                        uint32_t *output) {
  unsigned width = nvn_geometry_input_width(primitive);
  uint32_t total = 0, begin = 0;
  while (begin < count) {
    if (dense[begin] == UINT32_MAX) { begin++; continue; }
    uint32_t end = begin;
    while (end < count && dense[end] != UINT32_MAX) end++;
    uint32_t primitives = nvn_geometry_input_count(primitive, end - begin);
    if (output)
      for (uint32_t index = 0; index < primitives; index++) {
        uint32_t positions[6];
        nvn_geometry_input_indices(primitive, end - begin, index, positions);
        for (unsigned slot = 0; slot < width; slot++)
          output[((uint64_t)total + index) * width + slot] = dense[begin + positions[slot]];
      }
    total += primitives;
    begin = end;
  }
  return total;
}
