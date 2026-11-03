"""Exercise upload batching and retirement with CPU-only Metal object mocks."""

import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "nvn/nvn_mtl.m").read_text()


def function(name):
    match = re.search(r"^static [^\n]+\b" + name + r"\([^\n]*\) \{", SOURCE, re.M)
    if not match:
        raise AssertionError("Missing production function " + name)
    start = SOURCE.index("{", match.start())
    depth = 0
    for end in range(start, len(SOURCE)):
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        if not depth:
            return SOURCE[match.start():end + 1]
    raise AssertionError("Unterminated production function " + name)


PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures, created_compute, ended_compute, copies, commits, waits, removed;
#define CHECK(value) do { checks++; if (!(value)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)
#define MLOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define MDIE(...) @throw [NSException exceptionWithName:@"UploadBudget" reason:@"invalid size" userInfo:nil]
#define UP_TIME(field, statement) do { statement; } while (0)
@interface MockBuffer : NSObject
@property NSUInteger length, allocatedSize;
@property void *contents;
@end
@implementation MockBuffer
- (void)dealloc { free(_contents); }
@end
@interface MockDevice : NSObject
@property NSUInteger allocationPadding;
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options;
@end
@implementation MockDevice
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    CHECK(options == MTLResourceStorageModeShared);
    MockBuffer *buffer = [MockBuffer new];
    buffer.length = length; buffer.allocatedSize = length + self.allocationPadding;
    buffer.contents = calloc(1, length); CHECK(buffer.contents != NULL);
    return (id)buffer;
}
@end
@interface MockEncoder : NSObject
@property BOOL compute;
@end
@implementation MockEncoder @end
@interface MockCommandBuffer : NSObject
@property NSError *error;
@property double GPUStartTime, GPUEndTime;
- (id<MTLComputeCommandEncoder>)computeCommandEncoder;
@end
@implementation MockCommandBuffer
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
    MockEncoder *encoder = [MockEncoder new]; encoder.compute = YES;
    created_compute++; return (id)encoder;
}
@end
@interface NvCommitOptions : NSObject
@property(copy) void (^handler)(id<MTLCommandBuffer>);
- (void)addFeedbackHandler:(void (^)(id<MTLCommandBuffer>))handler;
@end
@implementation NvCommitOptions
- (void)addFeedbackHandler:(void (^)(id<MTLCommandBuffer>))handler { self.handler = handler; }
@end
static id<MTLCommandBuffer> g_cb, g_up_cb;
static id<MTLComputeCommandEncoder> g_up_ce;
static id<MTLCommandQueue> g_replay_q;
static id<MTLDevice> g_dev;
static _Atomic int g_up_alloc_busy[3], g_up_pending;
static unsigned g_up_seq, g_up_cur_slot;
static uint64_t g_up_slot_frame[3];
static _Atomic uint64_t g_cur_frame;
static id<MTLBuffer> g_up_slot_blk[3], g_up_block;
static size_t g_up_block_off, g_up_block_size, g_up_compressed_bytes;
static id<MTLBuffer> g_up_shared_blk[3];
static size_t g_up_shared_off;
static uint64_t g_up_shared_bytes, g_up_shared_allocations, g_up_shared_slices;
static NSMutableArray *g_up_texs, *g_up_keep, *jobs;
static NSMutableArray *copy_snapshots;
static _Atomic uint64_t g_up_staging_bytes, g_up_staging_peak, g_upload_gpu_ns;
static NSHashTable *g_up_lost;
static os_unfair_lock g_up_lost_lk = OS_UNFAIR_LOCK_INIT;
static _Atomic int g_up_lost_n;
static _Atomic uint64_t g_up_lost_redo;
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_ser_gen;
typedef struct { uint64_t flush_ns; } upload_timing;
static upload_timing *t_upload_timing;
typedef struct {
    __unsafe_unretained id<MTLBuffer> src;
    size_t off, row, image;
    MTLSize size;
    MTLOrigin origin;
    __unsafe_unretained id<MTLTexture> dst;
    NSUInteger slice, level;
} up_blit;
static up_blit *g_up_blits;
static int g_nup_blits, g_up_blitcap;
static size_t largest_batch;
static uint64_t bp_now(void) { return 0; }
static void up_owner_check(const char *where) { (void)where; }
static void up_diag(void) {}
static void up_wake(void) {}
static void res_add(id object) { CHECK(object != nil); }
static void res_remove(id object) {
    CHECK(object != nil);
    for (int i = 0; i < 3; i++) CHECK(object != g_up_shared_blk[i]);
    removed++;
}
static void res_commit(void) {}
static id<MTLCommandBuffer> command_buffer(id<MTLCommandQueue> queue) {
    (void)queue; return (id)[MockCommandBuffer new];
}
static id encoder_begin(id cb, id encoder) { CHECK(cb != nil); return encoder; }
static void enc_end(id encoder) { ended_compute += ((MockEncoder *)encoder).compute; }
static id<MTLBlitCommandEncoder> enc_b_begin(id cb) { CHECK(cb != nil); return (id)[MockEncoder new]; }
static void enc_b_copy_buffer_texture(id encoder, id src, size_t off, size_t row, size_t image,
                                      MTLSize size, id dst, NSUInteger slice, NSUInteger level, MTLOrigin origin) {
    CHECK(encoder && src && dst); copies++;
    MockBuffer *buffer = src;
    if (buffer.contents && image) {
        CHECK(off <= buffer.length && image <= buffer.length - off);
        NSData *snapshot = [NSData dataWithBytes:(uint8_t *)buffer.contents + off length:image];
        [copy_snapshots addObject:@[buffer, @(off), snapshot]];
    }
    (void)off; (void)row; (void)image; (void)size; (void)slice; (void)level; (void)origin;
}
static void replay_commit(id cb, NvCommitOptions *options, id event, uint64_t fence) {
    CHECK(cb != nil && options.handler != nil); commits++;
    if (g_up_compressed_bytes > largest_batch) largest_batch = g_up_compressed_bytes;
    CHECK(g_up_compressed_bytes <= UP_COMPRESSED_LIMIT);
    [jobs addObject:@[cb, options, [copy_snapshots copy]]];
    [copy_snapshots removeAllObjects]; (void)event; (void)fence;
}
static void complete_first(void) {
    CHECK(jobs.count != 0);
    NSArray *job = jobs[0]; [jobs removeObjectAtIndex:0];
    for (NSArray *copy in job[2]) {
        MockBuffer *buffer = copy[0]; NSData *expected = copy[2];
        CHECK(!memcmp((uint8_t *)buffer.contents + [copy[1] unsignedLongLongValue], expected.bytes, expected.length));
    }
    ((NvCommitOptions *)job[1]).handler(job[0]);
}
static void complete_all(void) { while (jobs.count) complete_first(); }
static void up_wait_zero(_Atomic int *busy) {
    while (atomic_load(busy)) { waits++; complete_first(); }
}
'''


TEST = r'''
static id<MTLBuffer> append_shared(size_t bytes, uint8_t value, size_t *offset) {
    id<MTLBuffer> buffer = up_shared_alloc(bytes, offset);
    memset((uint8_t *)buffer.contents + *offset, value, bytes);
    id texture = [NSObject new]; [g_up_texs addObject:texture];
    if (g_nup_blits == g_up_blitcap) {
        g_up_blitcap = g_up_blitcap ? g_up_blitcap * 2 : 16;
        g_up_blits = realloc(g_up_blits, g_up_blitcap * sizeof *g_up_blits);
    }
    g_up_blits[g_nup_blits++] = (up_blit){.src=buffer, .off=*offset, .image=bytes, .dst=texture};
    return buffer;
}
static void append_copy(size_t bytes) {
    up_compressed_reserve(bytes);
    MockBuffer *buffer = [MockBuffer new]; buffer.length = buffer.allocatedSize = bytes;
    id texture = [NSObject new];
    [g_up_keep addObject:buffer]; [g_up_texs addObject:texture];
    if (g_nup_blits == g_up_blitcap) {
        g_up_blitcap = g_up_blitcap ? g_up_blitcap * 2 : 16;
        g_up_blits = realloc(g_up_blits, g_up_blitcap * sizeof *g_up_blits);
    }
    g_up_blits[g_nup_blits++] = (up_blit){.src=(id)buffer, .dst=texture};
    up_compressed_chunk_end();
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "unset")) unsetenv("DM_NVN_COMPRESSED_BATCH");
    else setenv("DM_NVN_COMPRESSED_BATCH", argv[1], 1);
    int batch = strcmp(argv[1], "0") != 0;
    if (!strcmp(argv[2], "unset")) unsetenv("DM_NVN_SHARED_STAGING");
    else setenv("DM_NVN_SHARED_STAGING", argv[2], 1);
    int shared = strcmp(argv[2], "0") != 0;
    jobs = [NSMutableArray new]; copy_snapshots = [NSMutableArray new];
    g_cb = (id)[MockCommandBuffer new]; g_dev = (id)[MockDevice new];
    CHECK(up_staging_size(nil) == 0);
    MockBuffer *buffer = [MockBuffer new]; buffer.length = 256; buffer.allocatedSize = 16384;
    CHECK(up_staging_size((id)buffer) == 16384);
    buffer.allocatedSize = 0;
    CHECK(up_staging_size((id)buffer) == 256);

    append_copy(16384); up_compressed_texture_end();
    append_copy(32768); up_compressed_texture_end();
    CHECK(g_up_seq == (batch ? 1u : 2u));
    CHECK(!created_compute && !g_up_ce);
    up_flush(); CHECK(!g_up_cb && !g_up_compressed_bytes);
    CHECK(copies == 2 && commits == (batch ? 1u : 2u));
    complete_all(); CHECK(!g_up_staging_bytes && !g_up_pending);

    // A mixed batch retains both encoded and decoded storage, not just output.
    append_copy(3u << 20); append_copy(3u << 20);
    unsigned previous = commits;
    append_copy(4u << 20);
    CHECK(commits == previous + 1 && g_up_compressed_bytes == 4u << 20);
    append_copy(4u << 20);
    CHECK(!g_up_cb && !g_up_compressed_bytes && largest_batch == UP_COMPRESSED_LIMIT);
    complete_all();

    // Ring pressure completes old uploads before the same slot can be reused.
    unsigned old_waits = waits;
    for (int i = 0; i < 8; i++) { append_copy(6u << 20); up_flush(); }
    CHECK(waits > old_waits && jobs.count <= 3);
    complete_all();
    CHECK(!g_up_alloc_busy[0] && !g_up_alloc_busy[1] && !g_up_alloc_busy[2]);

    append_copy(16384);
    CHECK(!g_up_ce);
    CHECK(up_compute() != nil); CHECK(up_compute() != nil);
    CHECK(created_compute == 1);
    up_flush(); CHECK(ended_compute == 1); complete_all();

    // A submission boundary commits the last partial batch; out-of-submit
    // uploads also commit without a subsequent texture call.
    g_cb = nil; previous = commits;
    append_copy(16384); up_compressed_texture_end();
    CHECK(commits == previous + 1 && !g_up_cb); complete_all();
    g_cb = (id)[MockCommandBuffer new];

    __weak id weak_buffer, weak_texture;
    @autoreleasepool {
        append_copy(16384);
        weak_buffer = g_up_keep.lastObject; weak_texture = g_up_texs.lastObject;
        up_flush();
    }
    CHECK(weak_buffer && weak_texture);
    complete_all();
    CHECK(!weak_buffer && !weak_texture);

    // Error recovery retains no staging but marks every live target unwritten.
    append_copy(16384);
    id target = g_up_texs.lastObject;
    ((MockCommandBuffer *)g_up_cb).error = [NSError errorWithDomain:@"MockGPU" code:1 userInfo:nil];
    uint64_t generation = g_ser_gen;
    up_flush(); complete_all();
    CHECK(g_ser_gen == generation + 1 && g_up_lost_n == 1);
    CHECK(up_lost_take(target)); CHECK(!up_lost_take(target));
    CHECK(!g_up_lost_n && g_up_lost_redo == 1);

    previous = g_up_seq;
    @try { up_compressed_reserve((size_t)UP_COMPRESSED_LIMIT + 1); CHECK(0); }
    @catch (NSException *error) { CHECK([error.name isEqualToString:@"UploadBudget"]); }
    CHECK(!g_up_cb && !g_up_compressed_bytes && g_up_seq == previous);

    size_t off_a, off_b;
    id<MTLBuffer> a = append_shared(257, 0x11, &off_a);
    id<MTLBuffer> b = append_shared(31, 0x22, &off_b);
    CHECK(off_a == 0 && off_b == (shared ? 512u : 0u));
    CHECK((a == b) == shared);
    CHECK(((uint8_t *)a.contents)[off_a] == 0x11);
    CHECK(((uint8_t *)b.contents)[off_b] == 0x22);
    CHECK(g_up_compressed_bytes == (shared ? UP_COMPRESSED_CHUNK : 288u));
    CHECK(!g_up_ce); up_flush(); complete_all();

    // Full blocks force a new batch. Reusing slot zero must wait for its
    // previous reads; completion checks snapshots before new CPU writes.
    old_waits = waits;
    for (int i = 0; i < 8; i++) {
        append_shared(UP_COMPRESSED_CHUNK, (uint8_t)(0x40 + i), &off_a);
        CHECK(off_a == 0);
        if (!shared) up_flush();
    }
    CHECK(waits > old_waits);
    up_flush(); complete_all();
    CHECK(g_up_shared_allocations == (shared ? 3u : 0u));
    CHECK(g_up_shared_bytes == (shared ? 3u * UP_COMPRESSED_CHUNK : 0u));

    // A private BC pair and a shared arena cannot exceed the same batch's
    // allocation budget, in either arrival order.
    append_copy(6u << 20); previous = commits;
    append_shared(1024, 0x73, &off_a);
    CHECK(commits == previous + (shared ? 1u : 0u));
    CHECK(off_a == 0);
    if (shared) CHECK(g_up_compressed_bytes == UP_COMPRESSED_CHUNK);
    previous = commits; append_copy(6u << 20);
    CHECK(commits == previous + 1);
    up_flush(); complete_all();

    // Oversized rows use bounded transient storage and retain it until the
    // upload completes. Reusable slot blocks never enter that retirement set.
    __weak id weak_large;
    @autoreleasepool {
        weak_large = append_shared(5u << 20, 0x91, &off_a);
        CHECK(off_a == 0 && g_up_compressed_bytes == 5u << 20);
        up_flush();
    }
    CHECK(weak_large != nil); complete_all(); CHECK(weak_large == nil);
    for (int i = 0; i < 3; i++) CHECK((g_up_shared_blk[i] != nil) == shared);

    append_shared(512, 0xD1, &off_a);
    target = g_up_texs.lastObject;
    ((MockCommandBuffer *)g_up_cb).error = [NSError errorWithDomain:@"MockGPU" code:2 userInfo:nil];
    up_flush(); complete_all();
    CHECK(up_lost_take(target) && !up_lost_take(target));
    CHECK(!g_up_pending && !g_up_shared_off);

    // Real allocation size, including driver padding, is charged once.
    if (shared) {
        ((MockBuffer *)g_up_shared_blk[g_up_seq % 3]).allocatedSize += 16384;
        append_shared(256, 0xA1, &off_a);
        CHECK(g_up_compressed_bytes == UP_COMPRESSED_CHUNK + 16384u);
        append_shared(256, 0xA2, &off_b);
        CHECK(off_b == 256 && g_up_compressed_bytes == UP_COMPRESSED_CHUNK + 16384u);
        up_flush(); complete_all();
    }
    previous = g_up_seq;
    @try { up_shared_alloc(0, &off_a); CHECK(0); }
    @catch (NSException *error) { CHECK([error.name isEqualToString:@"UploadBudget"]); }
    @try { up_shared_alloc((size_t)UP_COMPRESSED_LIMIT + 1, &off_a); CHECK(0); }
    @catch (NSException *error) { CHECK([error.name isEqualToString:@"UploadBudget"]); }
    CHECK(!g_up_cb && g_up_seq == previous);
    CHECK(removed > 0 && !g_up_staging_bytes && !g_up_pending);
    free(g_up_blits);
    printf("batch=%s shared=%s: %u checks, %u failures; no Metal device created\n", argv[1], argv[2], checks, failures);
    return failures != 0;
} }
'''


constants = re.search(r"^enum \{ UP_COMPRESSED_LIMIT[^\n]+", SOURCE, re.M).group()
# The mocks implement only CPU object lifetime and ordering. No device factory,
# shader compiler, command queue or GPU submission is linked or called.
prefix = PREFIX.replace("static unsigned checks", constants + "\nstatic unsigned checks", 1)
functions = "\n\n".join(function(name) for name in (
    "up_lost_take", "up_flush", "up_begin", "up_compute", "up_staging_size",
    "up_compressed_reserve", "up_compressed_chunk_end", "up_compressed_texture_end",
    "up_shared_enabled", "up_shared_alloc",
))
with tempfile.TemporaryDirectory(prefix="nvn-upload-batch-") as directory:
    root = Path(directory)
    source = root / "batch.m"
    binary = root / "batch"
    source.write_text(prefix + "\n" + functions + "\n" + TEST)
    subprocess.run([
        "clang", "-std=gnu11", "-fobjc-arc", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-framework", "Foundation", str(source), "-o", str(binary),
    ], check=True)
    environment = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
    for mode in ("0", "1", "unset"):
        for shared in ("0", "1", "unset"):
            subprocess.run([str(binary), mode, shared], env=environment, check=True)
