#pragma once
#include <stdint.h>

// Shared CPU and Metal topology rules keep dynamic remapping identical.
#define NVN_GEOMETRY_TOPOLOGY \
/* Geometry compute kernels use the translator's Vulkan primitive input order. */ \
/* https://docs.vulkan.org/spec/latest/chapters/drawing.html#drawing-primitive-topologies */ \
static inline unsigned nvn_geometry_input_width(unsigned primitive) { \
  switch (primitive) { \
  case 0: return 1; \
  case 1: case 2: case 3: return 2; \
  case 4: case 5: case 6: return 3; \
  case 10: case 11: return 4; \
  case 12: case 13: return 6; \
  default: return 0; \
  } \
} \
 \
static inline uint32_t nvn_geometry_input_count(unsigned primitive, \
                                                    uint32_t count) { \
  switch (primitive) { \
  case 0: return count; \
  case 1: return count / 2; \
  case 2: return count >= 2 ? count : 0; \
  case 3: return count >= 2 ? count - 1 : 0; \
  case 4: return count / 3; \
  case 5: case 6: return count >= 3 ? count - 2 : 0; \
  case 10: return count / 4; \
  case 11: return count >= 4 ? count - 3 : 0; \
  case 12: return count / 6; \
  case 13: return count >= 6 ? (count - 4) / 2 : 0; \
  default: return 0; \
  } \
} \
 \
static inline void nvn_geometry_input_indices(unsigned primitive, \
                                                    uint32_t count, \
                                                    uint32_t index, \
                                                    NVN_GEOMETRY_THREAD uint32_t *output) { \
  unsigned width = nvn_geometry_input_width(primitive); \
  if (primitive == 2) { \
    output[0] = index; \
    output[1] = (index + 1) % count; \
  } else if (primitive == 3 || primitive == 11) { \
    for (unsigned slot = 0; slot < width; slot++) output[slot] = index + slot; \
  } else if (primitive == 5) { \
    output[0] = index; \
    output[1] = index + 1 + (index & 1); \
    output[2] = index + 2 - (index & 1); \
  } else if (primitive == 6) { \
    output[0] = index + 1; \
    output[1] = index + 2; \
    output[2] = 0; \
  } else if (primitive == 13) { \
    uint32_t primitives = nvn_geometry_input_count(primitive, count); \
    uint32_t base = 2 * index; \
    output[0] = base; \
    if (primitives == 1) { \
      output[1] = 1; output[2] = 2; output[3] = 5; \
      output[4] = 4; output[5] = 3; \
    } else if (!index) { \
      output[1] = 1; output[2] = 2; output[3] = 6; \
      output[4] = 4; output[5] = 3; \
    } else if (index & 1) { \
      output[1] = base + 3; output[2] = base + 4; \
      output[3] = base + (index + 1 == primitives ? 5 : 6); \
      output[4] = base + 2; output[5] = base - 2; \
    } else { \
      output[1] = base - 2; output[2] = base + 2; \
      output[3] = base + (index + 1 == primitives ? 5 : 6); \
      output[4] = base + 4; output[5] = base + 3; \
    } \
  } else { \
    for (unsigned slot = 0; slot < width; slot++) \
      output[slot] = index * width + slot; \
  } \
} \

#define NVN_GEOMETRY_THREAD
NVN_GEOMETRY_TOPOLOGY
#undef NVN_GEOMETRY_THREAD
