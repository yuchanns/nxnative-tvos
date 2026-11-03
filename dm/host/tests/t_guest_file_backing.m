#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../guest_file_backing.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr, "%s:%d: %s (errno %d)\n", __FILE__, __LINE__, #x, errno); } } while (0)
int main(void) { @autoreleasepool {
    size_t page = getpagesize(), size = page * 16;
    uint8_t *base = mmap(NULL, size + page * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(base != MAP_FAILED); uint32_t *heap = (uint32_t *)(base + page);
    CHECK(dm_guest_file_backing_map(heap, size, "/dev/null/invalid") == -1);
    CHECK(!mprotect(heap, size, PROT_READ | PROT_WRITE)); heap[0] = 42; CHECK(heap[0] == 42);
    CHECK(!mprotect(heap, size, PROT_NONE));
    CHECK(dm_guest_file_backing_map((uint8_t *)heap + 1, size, "/tmp") == -1 && errno == EINVAL);
    int fd = dm_guest_file_backing_map(heap, size, NSTemporaryDirectory().fileSystemRepresentation); CHECK(fd >= 0);
    if (fd < 0) return 1;
    struct stat st; CHECK(!fstat(fd, &st)); CHECK(st.st_nlink == 0 && (size_t)st.st_size == size);
    CHECK(!mprotect(heap, size, PROT_READ | PROT_WRITE));
    for (size_t i = 0; i < size / 4; i++) { CHECK(heap[i] == 0); heap[i] = (uint32_t)i ^ 0xa531; }
    CHECK(!msync(heap, size, MS_SYNC));
    uint32_t *disk = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0); CHECK(disk != MAP_FAILED);
    for (size_t i = 0; i < size / 4; i++) CHECK(disk[i] == ((uint32_t)i ^ 0xa531));
    @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); CHECK(device != nil);
    id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:heap length:size options:MTLResourceStorageModeShared deallocator:nil]; CHECK(buffer != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void mutate(device uint*data[[buffer(0)]],uint i[[thread_position_in_grid]]){data[i]^=0xcafe0000u;}" options:nil error:&error];
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"mutate"] error:&error]; CHECK(pipeline != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    for (unsigned pass = 1; pass <= 4; pass++) {
        id<MTLCommandBuffer> command = [queue commandBuffer]; id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline]; [encoder setBuffer:buffer offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(size / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted]; CHECK(!command.error);
        CHECK(!msync(heap, size, MS_SYNC));
        CHECK(!madvise(heap, size, MADV_DONTNEED));
        for (size_t i = 0; i < size / 4; i++) {
            uint32_t expected = (uint32_t)i ^ 0xa531 ^ (pass % 2 ? 0xcafe0000u : 0);
            CHECK(heap[i] == expected && disk[i] == expected);
        }
    }
    }
    CHECK(!munmap(disk, size)); CHECK(!close(fd)); CHECK(!munmap(base, size + page * 2));
    printf("Guest file backing checks=%d failed=%d\n", checks, failed); return failed != 0;
} }
