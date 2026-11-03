#include <metal_stdlib>
using namespace metal;

inline float spvSubgroupShuffle(float value, ushort lane)
{
    return simd_shuffle(value, lane);
}

int exchange(thread const int& value, thread const int& mask, thread uint& gl_SubgroupInvocationID)
{
    float laneBits = as_type<float>(gl_SubgroupInvocationID);
    int target = as_type<int>(laneBits) ^ mask;
    float result = spvSubgroupShuffle(as_type<float>(value), uint(target));
    return as_type<int>(result);
}

struct Params { uint _m0; };
struct Counters { uint _m0[4]; };
struct spvDescriptorSetBuffer0
{
    constant Params *fp_c0 [[id(0)]];
    device Counters *fp_s0 [[id(1)]];
};

fragment float4 main0(float4 pos [[position]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])
{
    int value = as_type<int>(pos.x);
    int mask = 1;
    int exchanged = exchange(value, mask, gl_SubgroupInvocationID);
    float result = as_type<float>(exchanged) + dfdx(pos.x) - 1.0;
    if ((uint(pos.x) + uint(pos.y)) % 3 == 0)
    {
        discard_fragment();
    }
    bool predicate = uint(pos.x) % 2 == 0;
    if (predicate)
    {
        uint counter = (*spvDescriptorSet0.fp_c0)._m0;
        atomic_fetch_add_explicit((device atomic_uint*)&(*spvDescriptorSet0.fp_s0)._m0[counter], 1u, memory_order_relaxed);
    }
    return float4(float(predicate), result, 1.0, 1.0);
}
