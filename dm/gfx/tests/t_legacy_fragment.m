// Compare helper-aware subgroup counters with actual covered fragment pixels.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2 && argc != 3) return 2;
        int quad = argc == 3;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice(); NSError *error = nil;
        id<MTLLibrary> fragment = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        id<MTLLibrary> vertex = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 vertex_main(uint id [[vertex_id]]) { const float2 p[3] = {float2(-.91,-.87),float2(.83,-.69),float2(-.17,.93)}; return float4(p[id],0,1); }"
            options:nil error:&error];
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [vertex newFunctionWithName:@"vertex_main"];
        descriptor.fragmentFunction = [fragment newFunctionWithName:@"main0"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) { fprintf(stderr, "pipeline: %s\n", error.localizedDescription.UTF8String); return 1; }
        id<MTLArgumentEncoder> arguments = [descriptor.fragmentFunction newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> ab = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
        id<MTLBuffer> params = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLBuffer> counts = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        [arguments setArgumentBuffer:ab offset:0];
        [arguments setBuffer:params offset:0 atIndex:0]; [arguments setBuffer:counts offset:0 atIndex:1];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        unsigned failed = 0, checks = 0;
        for (unsigned size = 11; size <= 47; size += 12) for (unsigned index = 0; index < 4; index++) {
            memset(counts.contents, 0, counts.length); *(uint32_t *)params.contents = index;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:size height:size mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
            id<MTLTexture> target = [device newTextureWithDescriptor:td];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:pipeline]; [encoder setFragmentBuffer:ab offset:0 atIndex:0];
            [encoder useResource:params usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
            [encoder useResource:counts usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageFragment];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [encoder endEncoding];
            [command commit]; [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "GPU: %s\n", command.error.localizedDescription.UTF8String); return 1; }
            float *pixels = calloc(size * size * 4, sizeof(float));
            [target getBytes:pixels bytesPerRow:size * 16 fromRegion:MTLRegionMake2D(0, 0, size, size) mipmapLevel:0];
            unsigned covered = 0;
            for (unsigned y = 0; y < size; y++) for (unsigned x = 0; x < size; x++) {
                float *pixel = pixels + (y * size + x) * 4;
                if (pixel[2] == 0) continue;
                checks++;
                if ((x + y) % 3 == 0 || pixel[0] != (x % 2 == 0) || pixel[1] != (quad ? (float)(x ^ 1u) + 0.5f : 1.0f)) failed++;
                covered += pixel[0] == 1;
            }
            for (unsigned n = 0; n < 4; n++) {
                unsigned expected = n == index ? covered : 0, actual = ((uint32_t *)counts.contents)[n];
                checks++;
                if (actual != expected) { failed++; fprintf(stderr, "%ux%u counter %u: %u != %u\n", size, size, n, actual, expected); }
            }
            if (!covered) failed++;
            free(pixels);
        }
        printf("fragment GPU: %u checks, %u failures\n", checks, failed);
        return failed != 0;
    }
}
