#pragma once

enum {
  NVN_GEOMETRY_SCAN_THREADS = 256,
  NVN_GEOMETRY_BAD_INDEX = 0x10000,
  NVN_GEOMETRY_BAD_EMISSION = 0x20000,
  NVN_GEOMETRY_BAD_CAPACITY = 0x40000
};

#define DM_GEOMETRY_MSL(...) #__VA_ARGS__
static const char k_geometry_assemble_msl[] =
    "#include <metal_stdlib>\n"
    DM_GEOMETRY_MSL(
using namespace metal;
struct Params {
  uint groups, maximum, stride, width;
  uint capacity, count, reserved0, reserved1;
};
enum { BadIndex = 0x10000u, BadEmission = 0x20000u, BadCapacity = 0x40000u };
inline void fail(device atomic_uint *status, uint flag) {
  atomic_fetch_or_explicit(status, flag, memory_order_relaxed);
}
inline uint count_group(const device uint *raw, device atomic_uint *status,
                        constant Params &p, uint group) {
  uint n = 0u, emitted = 0u, used = 0u, flags = 0u;
  ulong begin = ulong(group) * p.maximum;
  for (uint j = 0u; j < p.stride; ++j) {
    uint vertex_index = raw[ulong(group) * p.stride + j];
    if (vertex_index == 0xffffffffu) { n = 0u; continue; }
    if (ulong(vertex_index) < begin || ulong(vertex_index) - begin >= p.maximum)
      flags |= BadIndex;
    if (++emitted > p.maximum) flags |= BadEmission;
    if (p.width == 1u || n + 1u >= p.width) {
      if (ulong(used) + p.width > p.capacity) flags |= BadCapacity;
      else used += p.width;
    }
    ++n;
  }
  if (flags) fail(status, flags);
  return flags ? 0u : used;
}
kernel void dm_gs_count(const device uint *raw [[buffer(0)]],
                        device uint *counts [[buffer(1)]],
                        device atomic_uint *status [[buffer(5)]],
                        constant Params &p [[buffer(6)]],
                        uint group [[thread_position_in_grid]]) {
  if (group < p.groups) counts[group] = count_group(raw, status, p, group);
}
kernel void dm_gs_scan(const device uint *input [[buffer(0)]],
                       device uint *offsets [[buffer(1)]],
                       device uint *sums [[buffer(2)]],
                       device atomic_uint *status [[buffer(5)]],
                       constant Params &p [[buffer(6)]],
                       uint lane [[thread_index_in_threadgroup]],
                       uint block [[threadgroup_position_in_grid]]) {
  threadgroup ulong values[256];
  uint index = block * 256u + lane;
  uint value = index < p.count ? input[index] : 0u;
  values[lane] = value;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  for (uint step = 1u; step < 256u; step <<= 1u) {
    ulong add = lane >= step ? values[lane - step] : 0ul;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    values[lane] += add;
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  ulong prefix = values[lane] - value;
  if (prefix > p.capacity || values[lane] > p.capacity)
    fail(status, BadCapacity);
  if (index < p.count) offsets[index] = prefix <= p.capacity ? uint(prefix) : 0u;
  if (lane == 255u) sums[block] = values[lane] <= p.capacity ? uint(values[lane]) : 0u;
}
kernel void dm_gs_add(device uint *offsets [[buffer(0)]],
                      const device uint *parents [[buffer(1)]],
                      device atomic_uint *status [[buffer(5)]],
                      constant Params &p [[buffer(6)]],
                      uint index [[thread_position_in_grid]]) {
  if (index >= p.count) return;
  ulong value = ulong(offsets[index]) + parents[index / 256u];
  if (value > p.capacity) { fail(status, BadCapacity); offsets[index] = 0u; }
  else offsets[index] = uint(value);
}
inline uint emit_group(const device uint *raw, device uint *output,
                       device atomic_uint *status, constant Params &p,
                       uint group, uint used) {
  uint a = 0u, b = 0u, n = 0u, emitted = 0u;
  ulong begin = ulong(group) * p.maximum;
  for (uint j = 0u; j < p.stride; ++j) {
    uint vertex_index = raw[ulong(group) * p.stride + j];
    if (vertex_index == 0xffffffffu) { n = 0u; continue; }
    if (ulong(vertex_index) < begin || ulong(vertex_index) - begin >= p.maximum) {
      fail(status, BadIndex); return used;
    }
    if (++emitted > p.maximum) { fail(status, BadEmission); return used; }
    if (p.width == 1u || n + 1u >= p.width) {
      if (ulong(used) + p.width > p.capacity) { fail(status, BadCapacity); return used; }
      if (p.width == 1u) output[used++] = vertex_index;
      else if (p.width == 2u) {
        output[used++] = b; output[used++] = vertex_index;
      } else {
        output[used++] = n & 1u ? b : a;
        output[used++] = n & 1u ? a : b;
        output[used++] = vertex_index;
      }
    }
    a = b; b = vertex_index; ++n;
  }
  return used;
}
kernel void dm_gs_emit(const device uint *raw [[buffer(0)]],
                       const device uint *offsets [[buffer(1)]],
                       device uint *output [[buffer(2)]],
                       device atomic_uint *status [[buffer(5)]],
                       constant Params &p [[buffer(6)]],
                       uint group [[thread_position_in_grid]]) {
  if (group < p.groups && !atomic_load_explicit(status, memory_order_relaxed))
    emit_group(raw, output, status, p, group, offsets[group]);
}
kernel void dm_gs_finalize(const device uint *counts [[buffer(0)]],
                           const device uint *offsets [[buffer(1)]],
                           device uint *arguments [[buffer(4)]],
                           device atomic_uint *status [[buffer(5)]],
                           constant Params &p [[buffer(6)]]) {
  ulong count = p.groups ? ulong(offsets[p.groups - 1u]) + counts[p.groups - 1u] : 0ul;
  if (count > p.capacity) fail(status, BadCapacity);
  arguments[0] = atomic_load_explicit(status, memory_order_relaxed) ? 0u : uint(count);
  arguments[1] = 1u;
  arguments[2] = arguments[3] = arguments[4] = 0u;
}
kernel void dm_gs_small(const device uint *raw [[buffer(0)]],
                        device uint *output [[buffer(2)]],
                        device uint *arguments [[buffer(4)]],
                        device atomic_uint *status [[buffer(5)]],
                        constant Params &p [[buffer(6)]]) {
  ulong count = 0ul;
  for (uint group = 0u; group < p.groups; ++group)
    count += count_group(raw, status, p, group);
  if (count > p.capacity) fail(status, BadCapacity);
  if (!atomic_load_explicit(status, memory_order_relaxed)) {
    uint used = 0u;
    for (uint group = 0u; group < p.groups; ++group)
      used = emit_group(raw, output, status, p, group, used);
  }
  arguments[0] = atomic_load_explicit(status, memory_order_relaxed) ? 0u : uint(count);
  arguments[1] = 1u;
  arguments[2] = arguments[3] = arguments[4] = 0u;
}
);
#undef DM_GEOMETRY_MSL
