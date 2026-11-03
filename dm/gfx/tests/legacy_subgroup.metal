#include <metal_stdlib>
using namespace metal;
#ifndef FIXTURE_THREADS
#define FIXTURE_THREADS 64
#endif

inline float spvSubgroupShuffle(float value, ushort lane)
{
    return simd_shuffle(value, lane);
}

inline uint4 spvSubgroupBallot(bool value)
{
    simd_vote vote = simd_ballot(value);
    return uint4(as_type<uint2>((simd_vote::vote_t)vote), 0, 0);
}

kernel void main0(device uint *output [[buffer(0)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])
{
    uint index = gl_WorkGroupID.x * FIXTURE_THREADS + gl_LocalInvocationIndex;
    uint lane = gl_SubgroupInvocationID;
    uint value = index ^ 0xfedcba98u;
    output[index * 6] = as_type<uint>(spvSubgroupShuffle(as_type<float>(value), 31 - lane));
    uint total = index;
    uint step = 16;
    do
    {
        total += as_type<uint>(spvSubgroupShuffle(as_type<float>(total), lane ^ step));
        step >>= 1;
    } while (step != 0);
    output[index * 6 + 1] = total;
    output[index * 6 + 2] = spvSubgroupBallot(index % 3 == 0).x;
    if (lane % 5 == 0)
    {
        return;
    }
    uint active = spvSubgroupBallot(true).x;
    output[index * 6 + 3] = active;
    uint highest = 31 - clz(active);
    output[index * 6 + 4] = as_type<uint>(spvSubgroupShuffle(as_type<float>(value), highest));
    output[index * 6 + 5] = spvSubgroupBallot((index & 2) != 0).x;
}
