#include <metal_stdlib>
using namespace metal;

template<typename T>
inline T spvFindUMSB(T x)
{
    return select(clz(T(0)) - (clz(x) + T(1)), T(-1), x == T(0));
}

inline uint4 spvSubgroupBallot(bool value)
{
    simd_vote vote = simd_ballot(value);
    return uint4(as_type<uint2>((simd_vote::vote_t)vote), 0, 0);
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
    bool gl_HelperInvocation = {};
    gl_HelperInvocation = simd_is_helper_thread();
    float derivative = dfdx(pos.x);
    if ((uint(pos.x) + uint(pos.y)) % 3 == 0)
    {
        discard_fragment();
    }
    bool predicate = uint(pos.x) % 2 == 0;
    if (predicate)
    {
        float helperBits = as_type<float>(gl_HelperInvocation ? (-1) : 0);
        uint counter = (*spvDescriptorSet0.fp_c0)._m0;
        bool helper = as_type<int>(helperBits) != 0;
        uint unused = counter + 1;
        if (!helper)
        {
            uint mask = spvSubgroupBallot(true).x;
            float lane = as_type<float>(gl_SubgroupInvocationID);
            int highest = int(spvFindUMSB(mask));
            int count = popcount(int(mask));
            bool elected = highest == as_type<int>(lane);
            if (elected)
            {
                uint old = atomic_fetch_add_explicit((device atomic_uint*)&(*spvDescriptorSet0.fp_s0)._m0[counter], uint(count), memory_order_relaxed);
                uint result = old;
            }
        }
    }
    return float4(float(predicate), derivative, 1.0, 1.0);
}
