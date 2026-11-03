// CPU expectations cover cross-quad exchange, repeated votes, reductions, and
// partially terminated SIMD groups. Run the same fixture native and lowered.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2 && argc != 4) { fprintf(stderr, "usage: t_legacy_subgroup shader.metallib [width height]\n"); return 2; }
        unsigned width = argc == 4 ? (unsigned)atoi(argv[2]) : 64, height = argc == 4 ? (unsigned)atoi(argv[3]) : 1;
        unsigned threads = 4 * width * height;
        if (!width || !height || width * height > 512) return 2;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"main0"];
        id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!pipeline) { fprintf(stderr, "pipeline: %s\n", error.localizedDescription.UTF8String); return 1; }
        if (pipeline.threadExecutionWidth != 32) {
            fprintf(stderr, "expected width 32, got %lu\n", (unsigned long)pipeline.threadExecutionWidth); return 1;
        }
        enum { FIELDS = 6 };
        const uint32_t sentinel = 0xa5a5a5a5;
        id<MTLBuffer> buffer = [device newBufferWithLength:threads * FIELDS * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        uint32_t *data = buffer.contents;
        for (unsigned i = 0; i < threads * FIELDS; i++) data[i] = sentinel;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:buffer offset:0 atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, height, 1)];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "command: %s\n", command.error.localizedDescription.UTF8String); return 1;
        }
        unsigned failed = 0;
        for (unsigned i = 0; i < threads; i++) {
            unsigned base = i & ~31u, lane = i & 31;
            uint32_t vote = 0, active = 0, filtered = 0, sum = 0;
            for (unsigned j = 0; j < 32; j++) {
                sum += base + j;
                if ((base + j) % 3 == 0) vote |= 1u << j;
                if (j % 5) {
                    active |= 1u << j;
                    if ((base + j) & 2) filtered |= 1u << j;
                }
            }
            uint32_t expected[FIELDS] = {(base + 31 - lane) ^ 0xfedcba98u, sum, vote,
                lane % 5 ? active : sentinel,
                lane % 5 ? (base + 31) ^ 0xfedcba98u : sentinel,
                lane % 5 ? filtered : sentinel};
            for (unsigned field = 0; field < FIELDS; field++) {
                if (data[i * FIELDS + field] == expected[field]) continue;
                if (failed++ < 12) fprintf(stderr, "thread %u field %u: %#x != %#x\n", i, field, data[i * FIELDS + field], expected[field]);
            }
        }
        printf("subgroup GPU: %u checks, %u failures, %lu bytes threadgroup storage\n",
               threads * FIELDS, failed, (unsigned long)pipeline.staticThreadgroupMemoryLength);
        return failed != 0;
    }
}
