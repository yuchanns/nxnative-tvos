"""Exercise production texture snapshots with CPU-only Metal doubles."""

import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include "include/nvn.h"
#undef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 1
#define MLOG(...) ((void)0)
#define MDIE(...) abort()
typedef struct { struct { unsigned flags; } d; int has_layout; uint64_t serial; } tex_t;
static _Atomic uint64_t g_cur_frame;
static _Atomic int g_up_pending;
static uint64_t g_fence = 7, g_done = 7, g_ser_gen;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int submitting = 1, wait_calls, commits;
static size_t headroom = 128u << 20;
static int fail_write, fail_read;
static ssize_t test_pwrite(int fd, const void *data, size_t size, off_t offset) {
    if (fail_write) { errno = ENOSPC; return -1; }
    return pwrite(fd, data, size, offset);
}
#define pwrite test_pwrite
static ssize_t test_pread(int fd, void *data, size_t size, off_t offset) {
    if (fail_read) { errno = EIO; return -1; }
    return pread(fd, data, size, offset);
}
#define pread test_pread
static int on_sub_thread(void) { return submitting; }
static size_t os_proc_available_memory(void) { return headroom; }
static unsigned pressure_hooks;
static dispatch_source_t test_pressure_source(dispatch_source_type_t type, uintptr_t handle,
        unsigned long mask, dispatch_queue_t queue) {
    assert(type == DISPATCH_SOURCE_TYPE_MEMORYPRESSURE && !handle && queue);
    assert(mask == (DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL));
    pressure_hooks++;
    // Inject notifications below instead of depending on the host's pressure.
    return nil;
}
#define dispatch_source_create test_pressure_source

static size_t live_buffer_bytes;
@interface MockBuffer : NSObject <MTLBuffer>
@property(nonatomic, strong) NSMutableData *data;
@property(nonatomic) BOOL registered;
@end
@implementation MockBuffer
- (void *)contents { return self.data.mutableBytes; }
- (NSUInteger)length { return self.data.length; }
- (NSUInteger)allocatedSize { return (self.data.length + 16383u) & ~(NSUInteger)16383u; }
- (void)dealloc { assert(live_buffer_bytes >= self.allocatedSize); live_buffer_bytes -= self.allocatedSize; }
@end
@interface MockTexture : NSObject <MTLTexture>
@property(nonatomic) NSUInteger width, height, depth, mipmapLevelCount, arrayLength, sampleCount, allocatedSize;
@property(nonatomic) MTLPixelFormat pixelFormat;
@property(nonatomic) MTLTextureType textureType;
@property(nonatomic) MTLStorageMode storageMode;
@property(nonatomic, strong) id<MTLTexture> parentTexture;
@property(nonatomic, strong) id<MTLBuffer> buffer;
@property(nonatomic, strong) id<MTLHeap> heap;
@property(nonatomic, assign) IOSurfaceRef iosurface;
@property(nonatomic) BOOL isSparse, registered;
@property(nonatomic, getter=isFramebufferOnly) BOOL framebufferOnly;
@property(nonatomic, strong) NSMutableDictionary<NSString *, NSMutableData *> *planes;
@property(nonatomic) unsigned empty_count;
@end
@implementation MockTexture
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)value {
    if (value == MTLPurgeableStateEmpty) {
        self.empty_count++;
        for (NSMutableData *data in self.planes.allValues) memset(data.mutableBytes, 0, data.length);
    }
    return MTLPurgeableStateNonVolatile;
}
@end

@interface MockCommand : NSObject <MTLCommandBuffer, MTLBlitCommandEncoder>
@property(nonatomic) MTLCommandBufferStatus status;
@property(nonatomic, strong) NSMutableArray *operations;
@property(nonatomic, strong) NSMutableArray *handlers;
- (void)finishGPU;
- (void)finishHandlers;
- (void)finish;
@end
static void transfer(MockTexture *texture, MockBuffer *buffer, NSUInteger slice, NSUInteger level,
        MTLOrigin origin, MTLSize size, NSUInteger stride, MTLBlitOption options, int restore) {
    unsigned block = texture.pixelFormat == MTLPixelFormatASTC_4x4_LDR ? 4 : 1;
    unsigned bytes = block == 4 ? 16 : options == MTLBlitOptionStencilFromDepthStencil ? 1 : 4;
    NSUInteger width = MAX(texture.width >> level, 1u);
    NSUInteger row = ((width + block - 1) / block) * bytes;
    BOOL volume = texture.textureType == MTLTextureType3D;
    NSString *key = [NSString stringWithFormat:@"%lu:%lu:%lu", volume ? origin.z : slice, level, options];
    NSMutableData *data = texture.planes[key]; assert(data);
    assert(origin.x == 0 && size.depth == 1);
    assert(volume ? slice == 0 && origin.z < MAX(texture.depth >> level, 1u) : origin.z == 0);
    assert(size.width == width && origin.y % block == 0);
    for (NSUInteger r = 0; r < (size.height + block - 1) / block; r++) {
        uint8_t *tex = (uint8_t *)data.mutableBytes + (origin.y / block + r) * row;
        uint8_t *buf = (uint8_t *)buffer.contents + r * stride;
        assert((tex + row) <= (uint8_t *)data.mutableBytes + data.length);
        assert((buf + row) <= (uint8_t *)buffer.contents + buffer.data.length);
        if (restore) memcpy(tex, buf, row); else memcpy(buf, tex, row);
    }
}
@implementation MockCommand
- (instancetype)init { if ((self = [super init])) { _operations = [NSMutableArray array]; _handlers = [NSMutableArray array]; } return self; }
- (id<MTLBlitCommandEncoder>)blitCommandEncoder { return self; }
- (void)endEncoding {}
- (void)commit { self.status = MTLCommandBufferStatusCommitted; commits++; }
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
    assert(self.status < MTLCommandBufferStatusCommitted);
    [self.handlers addObject:[handler copy]];
}
- (void)finishGPU {
    for (void (^operation)(void) in self.operations) operation();
    [self.operations removeAllObjects]; self.status = MTLCommandBufferStatusCompleted;
}
- (void)finishHandlers {
    assert(self.status >= MTLCommandBufferStatusCompleted);
    for (MTLCommandBufferHandler handler in self.handlers) handler(self);
    [self.handlers removeAllObjects];
}
- (void)finish { if (self.status < MTLCommandBufferStatusCompleted) [self finishGPU]; [self finishHandlers]; }
- (void)waitUntilCompleted { wait_calls++; [self finish]; }
- (void)copyFromTexture:(id<MTLTexture>)texture sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)buffer destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)stride destinationBytesPerImage:(NSUInteger)image options:(MTLBlitOption)options {
    assert(!offset && (texture.textureType == MTLTextureType3D ? image == stride * size.height : image == 0));
    [self.operations addObject:^{ transfer((MockTexture *)texture, (MockBuffer *)buffer, slice, level, origin, size, stride, options, 0); }];
}
- (void)copyFromBuffer:(id<MTLBuffer>)buffer sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)stride sourceBytesPerImage:(NSUInteger)image sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)texture destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin options:(MTLBlitOption)options {
    assert(!offset && (texture.textureType == MTLTextureType3D ? image == stride * size.height : image == 0));
    [self.operations addObject:^{ transfer((MockTexture *)texture, (MockBuffer *)buffer, slice, level, origin, size, stride, options, 1); }];
}
@end
@interface MockDevice : NSObject <MTLDevice> @end
@implementation MockDevice
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    (void)options; MockBuffer *buffer = [MockBuffer new]; buffer.data = [NSMutableData dataWithLength:length];
    live_buffer_bytes += buffer.allocatedSize; return buffer;
}
@end
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q, g_replay_q;
static NSHashTable *resident_objects;
static void res_add(id resource) {
    if (!resident_objects) resident_objects = [NSHashTable weakObjectsHashTable];
    if ([resident_objects containsObject:resource]) return;
    [resident_objects addObject:resource]; [resource setRegistered:YES];
}
static void res_remove(id resource) {
    assert([resource registered]);
    [resident_objects removeObject:resource]; [resource setRegistered:NO];
}
static void res_commit(void) {}
static void gpu_tag(id buffer) { (void)buffer; }
static void log_gpu_encoders(id buffer) { (void)buffer; }
static id<MTLCommandBuffer> command_buffer(id<MTLCommandQueue> queue) { (void)queue; return [MockCommand new]; }
static void replay_commit(id<MTLCommandBuffer> cb, id options, id event, uint64_t value) { (void)options; (void)event; (void)value; [cb commit]; }
'''

TESTS = r'''
static unsigned checks;
static MockTexture *texture(MTLPixelFormat format, unsigned size) {
    MockTexture *t = [MockTexture new];
    t.width = size; t.height = size + 3; t.mipmapLevelCount = 3; t.arrayLength = 2;
    t.sampleCount = 1; t.storageMode = MTLStorageModePrivate; t.textureType = MTLTextureType2DArray;
    t.pixelFormat = format; t.allocatedSize = 8u << 20; t.registered = YES;
    t.planes = [NSMutableDictionary dictionary];
    unsigned block = format == MTLPixelFormatASTC_4x4_LDR ? 4 : 1;
    unsigned count = format == MTLPixelFormatDepth32Float_Stencil8 ? 2 : 1;
    for (unsigned p = 0; p < count; p++) for (unsigned l = 0; l < 3; l++) for (unsigned a = 0; a < 2; a++) {
        unsigned option = count == 2 ? 1u << p : 0;
        unsigned bytes = block == 4 ? 16 : p ? 1 : 4;
        unsigned w = MAX(size >> l, 1u), h = MAX((size + 3) >> l, 1u);
        NSMutableData *data = [NSMutableData dataWithLength:((w + block - 1) / block) * ((h + block - 1) / block) * bytes];
        uint8_t *raw = data.mutableBytes;
        for (NSUInteger i = 0; i < data.length; i++) raw[i] = (uint8_t)(i * 29 + p * 79 + l * 53 + a * 17);
        t.planes[[NSString stringWithFormat:@"%u:%u:%u", a, l, option]] = data;
    }
    return t;
}
static NSDictionary *contents(MockTexture *t) {
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    for (NSString *key in t.planes) result[key] = [t.planes[key] copy];
    return result;
}
static MockTexture *shaped_texture(MTLTextureType type) {
    MockTexture *t = texture(MTLPixelFormatRGBA8Unorm, 17);
    t.textureType = type;
    t.height = type == MTLTextureType1D || type == MTLTextureType1DArray ? 1 : 17;
    t.arrayLength = type == MTLTextureTypeCubeArray || type == MTLTextureType1DArray ? 2 : 1;
    t.depth = type == MTLTextureType3D ? 5 : 1;
    [t.planes removeAllObjects];
    for (NSUInteger level = 0; level < t.mipmapLevelCount; level++) {
        NSUInteger count = type == MTLTextureType3D ? MAX(t.depth >> level, 1u) :
            t.arrayLength * (type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray ? 6 : 1);
        for (NSUInteger slice = 0; slice < count; slice++) {
            NSUInteger size = MAX(t.width >> level, 1u) * MAX(t.height >> level, 1u) * 4;
            NSMutableData *data = [NSMutableData dataWithLength:size]; uint8_t *bytes = data.mutableBytes;
            for (NSUInteger i = 0; i < size; i++) bytes[i] = (uint8_t)(i * 47 + slice * 13 + level * 29);
            t.planes[[NSString stringWithFormat:@"%lu:%lu:0", slice, level]] = data;
        }
    }
    return t;
}
static NvRtCache *track(MockTexture *t, unsigned flags) {
    tex_t source = {.d.flags = flags, .serial = ++checks}; rt_cache_track(t, &source);
    return [g_rt_cache objectForKey:t];
}
static void advance(void) { g_cur_frame++; rt_cache_trim(); }
static void clear(void) {
    while (g_rt_pending_n) {
        NvRtCache *entry = g_rt_pending[0];
        MockCommand *copy = (MockCommand *)entry->copy;
        rt_cache_cancel(entry);
        [copy finish];
    }
    g_rt_cache = nil; g_cur_frame += 200; g_fence = 7; g_done = 7;
    assert(!atomic_load(&g_rt_staging_bytes));
    headroom = 128u << 20; g_rt_reclaiming = 0;
    rt_cache_memory_pressure(DISPATCH_MEMORYPRESSURE_NORMAL);
}
static void finish_snapshot(NvRtCache *entry) {
    int waited = wait_calls;
    for (unsigned i = 0; !entry->spilled; i++) {
        assert(i < 200 && entry->copy);
        [(MockCommand *)entry->copy finish]; advance();
    }
    assert(wait_calls == waited);
}
int main(void) { @autoreleasepool {
    g_dev = [MockDevice new]; g_cur_frame = 200;
    MTLTextureType shapes[] = {MTLTextureType1D, MTLTextureType1DArray, MTLTextureTypeCube, MTLTextureTypeCubeArray, MTLTextureType3D};
    for (unsigned i = 0; i < sizeof shapes / sizeof *shapes; i++) { @autoreleasepool {
        clear(); MockTexture *t = shaped_texture(shapes[i]); NSDictionary *expected = contents(t);
        NvRtCache *entry = track(t, 0); assert(entry); g_done = 8; g_cur_frame += 200;
        advance(); finish_snapshot(entry); rt_cache_use(t); assert([expected isEqual:contents(t)]);
    } }
    uint64_t initial_spills = g_rt_spills;
    MTLPixelFormat formats[] = {MTLPixelFormatRGBA8Unorm, MTLPixelFormatASTC_4x4_LDR, MTLPixelFormatDepth32Float_Stencil8};
    for (unsigned f = 0; f < 3; f++) { @autoreleasepool {
        clear(); MockTexture *t = texture(formats[f], 1024); NSDictionary *before = contents(t);
        NvRtCache *entry = track(t, f == 2 ? NVN_TEXTURE_FLAGS_IMAGE : 0); assert(entry);
        // No compressible flag is needed. Neither age nor LRU can retire live GPU use.
        g_cur_frame += 200; advance(); assert(!g_rt_pending_n && entry->fd < 0);
        g_done = 8; int waited = wait_calls; advance(); assert(entry->pending && entry->copy);
        for (unsigned i = 0; !entry->spilled; i++) {
            assert(i < 200); id<MTLCommandBuffer> pending = entry->copy; int submitted = commits;
            advance(); assert(commits == submitted && wait_calls == waited);
            [(MockCommand *)pending finish]; advance();
        }
        assert(!t.registered && t.empty_count == 1 && !g_rt_pending_n);
        assert(entry->file_bytes > 0 && wait_calls == waited);
        rt_cache_use(t); assert(!entry->spilled && entry->fd == -1 && t.registered);
        assert([before isEqual:contents(t)]); assert(wait_calls > waited);
    } }
    clear(); MockTexture *a = texture(MTLPixelFormatRGBA8Unorm, 1024);
    MockTexture *b = texture(MTLPixelFormatRGBA8Unorm, 1024);
    NvRtCache *older = track(a, 0); g_cur_frame += 1; NvRtCache *newer = track(b, 0);
    g_done = 8; g_cur_frame += 200; advance(); assert(older->pending);
    MockCommand *pending = (MockCommand *)older->copy; MockBuffer *staging = (MockBuffer *)older->staging;
    assert(staging.registered); rt_cache_use(a); assert(!g_rt_pending_n && older->fd == -1 && staging.registered);
    [pending finish]; assert(!staging.registered && !a.empty_count);
    advance(); assert(newer->pending);
    [(MockCommand *)newer->copy finish];
    // A failed destination write must leave every original byte resident.
    NSDictionary *before = contents(b); fail_write = 1; advance(); fail_write = 0;
    assert(newer->excluded && !newer->spilled && !b.empty_count && b.registered);
    assert([before isEqual:contents(b)]);
    // Cancellation cannot register a handler after commit, or release storage
    // still owned by a pending completion callback (including GPU errors).
    MTLCommandBufferStatus states[] = {MTLCommandBufferStatusCommitted,
        MTLCommandBufferStatusScheduled, MTLCommandBufferStatusCompleted, MTLCommandBufferStatusError};
    for (unsigned i = 0; i < 4; i++) { @autoreleasepool {
        clear(); MockTexture *t = texture(MTLPixelFormatRGBA8Unorm, 512);
        NSDictionary *original = contents(t); NvRtCache *entry = track(t, 0);
        g_done = 8; g_cur_frame += 200; advance();
        MockCommand *copy = (MockCommand *)entry->copy;
        MockBuffer *buffer = (MockBuffer *)entry->staging;
        assert(copy && buffer.registered);
        if (states[i] == MTLCommandBufferStatusCompleted) [copy finishGPU];
        else copy.status = states[i];
        if (states[i] == MTLCommandBufferStatusError) [copy.operations removeAllObjects];
        rt_cache_use(t);
        assert(!entry->copy && !entry->staging && entry->fd == -1 && buffer.registered);
        [copy finish];
        assert(!buffer.registered && t.registered && !t.empty_count);
        assert([original isEqual:contents(t)]);
    } }
    // GPU completion and callback delivery are distinct. The next chunk can
    // begin before the old callback, which must release only its own buffer.
    clear(); MockTexture *delayed = texture(MTLPixelFormatRGBA8Unorm, 512);
    NvRtCache *delayed_entry = track(delayed, 0); g_done = 8; g_cur_frame += 200; advance();
    MockCommand *old_copy = (MockCommand *)delayed_entry->copy;
    MockBuffer *old_buffer = (MockBuffer *)delayed_entry->staging;
    [old_copy finishGPU]; advance();
    MockCommand *next_copy = (MockCommand *)delayed_entry->copy;
    MockBuffer *next_buffer = (MockBuffer *)delayed_entry->staging;
    assert(next_copy != old_copy && next_buffer != old_buffer);
    assert(old_buffer.registered && next_buffer.registered);
    [old_copy finishHandlers]; assert(!old_buffer.registered && next_buffer.registered);
    rt_cache_use(delayed); [next_copy finish]; assert(!next_buffer.registered);
    // An entry may also be reused while its canceled copy is still in flight.
    g_done = 9; g_cur_frame += 200; advance();
    old_copy = (MockCommand *)delayed_entry->copy; old_buffer = (MockBuffer *)delayed_entry->staging;
    rt_cache_use(delayed); g_cur_frame += 200; advance();
    next_copy = (MockCommand *)delayed_entry->copy; next_buffer = (MockBuffer *)delayed_entry->staging;
    assert(old_copy && next_copy && old_copy != next_copy);
    [old_copy finish]; assert(!old_buffer.registered && next_buffer.registered);
    rt_cache_use(delayed); [next_copy finish]; assert(!next_buffer.registered);
    clear(); MockTexture *view = texture(MTLPixelFormatRGBA8Unorm, 512); view.parentTexture = a;
    assert(!track(view, 0)); assert(!track(texture(MTLPixelFormatRGBA8Unorm, 512), NVN_TEXTURE_FLAGS_DISPLAY));
    clear(); NvRtCache *external = track(a, 0); submitting = 0; rt_cache_use(a); submitting = 1;
    assert(external->excluded);
    // Sparse file growth is not capped at 2 GiB, and freed ranges remain reusable.
    uint64_t first, second, merged;
    assert(rt_cache_file_allocate(4096, &first) >= 0); assert(rt_cache_file_allocate(4096, &second) >= 0);
    rt_cache_file_release(first, 4096); rt_cache_file_release(second, 4096);
    assert(rt_cache_file_allocate(8192, &merged) >= 0 && merged == MIN(first, second));
    rt_cache_file_release(merged, 8192);
    assert(rt_cache_file_allocate(3ull << 30, &first) >= 0);
    uint64_t grown_end = g_rt_file_end;
    rt_cache_file_release(first, 3ull << 30);
    assert(rt_cache_file_allocate(3ull << 30, &second) >= 0 && second <= first);
    assert(g_rt_file_end == grown_end);
    // Invalid sizes and an unrepresentable file end must not change the spool.
    uint64_t saved_end = g_rt_file_end, unchanged = UINT64_MAX;
    assert(rt_cache_file_allocate(0, &unchanged) < 0);
    assert(rt_cache_file_allocate(UINT64_MAX, &unchanged) < 0);
    assert(rt_cache_file_allocate(INT64_MAX, &unchanged) < 0);
    assert(g_rt_file_end == saved_end && unchanged == UINT64_MAX);
    g_rt_file_end = (uint64_t)INT64_MAX & ~4095ull;
    assert(rt_cache_file_allocate(3ull << 30, &unchanged) < 0);
    assert(unchanged == UINT64_MAX);
    g_rt_file_end = saved_end;
    rt_cache_file_release(second, 3ull << 30);
    assert([rt_cache_status()[@"spills"] unsignedLongLongValue] == initial_spills + 3);
    // Thousands of small textures must not be excluded from reclamation or
    // serialized one entire mip chain at a time. Bound both queue and storage.
    clear(); NSMutableArray<MockTexture *> *small = [NSMutableArray array];
    NSMutableArray<NSDictionary *> *originals = [NSMutableArray array];
    for (unsigned i = 0; i < 48; i++) {
        MockTexture *t = texture(MTLPixelFormatRGBA8Unorm, 64); t.allocatedSize = 64u << 10;
        [small addObject:t]; [originals addObject:contents(t)]; assert(track(t, 0)); g_cur_frame++;
    }
    g_done = 8; g_cur_frame += 200; int batch_waits = wait_calls; advance();
    assert(g_rt_pending_n == RT_CACHE_PENDING_LIMIT);
    for (unsigned tick = 0;; tick++) {
        unsigned spilled = 0;
        for (MockTexture *t in small) spilled += ((NvRtCache *)[g_rt_cache objectForKey:t])->spilled;
        if (spilled == small.count) break;
        assert(tick < 32 && g_rt_pending_n <= RT_CACHE_PENDING_LIMIT);
        assert(atomic_load(&g_rt_staging_bytes) <= NVN_SNAPSHOT_CHUNK);
        for (unsigned i = 0; i < g_rt_pending_n; i++) [(MockCommand *)g_rt_pending[i]->copy finish];
        advance();
    }
    assert(!g_rt_pending_n && !atomic_load(&g_rt_staging_bytes) && wait_calls == batch_waits);
    // The production weak residency table can autorelease a removed buffer.
    // A burst of restores must free each synchronous staging allocation before
    // the next texture, without depending on the submission's outer pool.
    size_t before_restore_bytes = live_buffer_bytes;
    NvRtCache *failed_restore = [g_rt_cache objectForKey:small[0]];
    fail_read = 1; assert(!rt_cache_restore(failed_restore, small[0])); fail_read = 0;
    assert(live_buffer_bytes == before_restore_bytes && failed_restore->spilled);
    for (unsigned i = 0; i < small.count; i++) {
        rt_cache_use(small[i]); assert([originals[i] isEqual:contents(small[i])]);
        if (live_buffer_bytes != before_restore_bytes)
            fprintf(stderr, "restore %u retained %zu staging bytes\n", i + 1, live_buffer_bytes - before_restore_bytes);
        assert(live_buffer_bytes == before_restore_bytes);
    }
    // GPU completion alone does not release a copy still owned by its callback.
    clear(); MockTexture *large = texture(MTLPixelFormatRGBA8Unorm, 1024);
    NvRtCache *large_entry = track(large, 0); g_done = 8; g_cur_frame += 200; advance();
    MockCommand *held = (MockCommand *)large_entry->copy;
    assert(atomic_load(&g_rt_staging_bytes) == NVN_SNAPSHOT_CHUNK);
    [held finishGPU]; advance();
    assert(!large_entry->copy && large_entry->pending && atomic_load(&g_rt_staging_bytes) == NVN_SNAPSHOT_CHUNK);
    [held finishHandlers]; assert(!atomic_load(&g_rt_staging_bytes));
    advance(); assert(large_entry->copy); finish_snapshot(large_entry);
    // Begin earlier than the old 640 MiB policy, and continue between watermarks.
    clear(); MockTexture *cold = texture(MTLPixelFormatRGBA8Unorm, 512);
    MockTexture *next = texture(MTLPixelFormatRGBA8Unorm, 512);
    NvRtCache *first_entry = track(cold, 0); g_cur_frame++;
    NvRtCache *next_entry = track(next, 0); g_done = 8; g_cur_frame += 200;
    headroom = 900u << 20; advance(); assert(!g_rt_pending_n && !g_rt_reclaiming);
    headroom = 895u << 20; advance(); assert(first_entry->pending && g_rt_reclaiming);
    headroom = 960u << 20; finish_snapshot(first_entry);
    advance(); assert(next_entry->pending && g_rt_reclaiming);
    // Recovery stops new work, but finishes an already submitted snapshot.
    headroom = 1024u << 20; finish_snapshot(next_entry); assert(!g_rt_reclaiming);
    MockTexture *unused = texture(MTLPixelFormatRGBA8Unorm, 512);
    NvRtCache *unused_entry = track(unused, 0); g_cur_frame += 200;
    advance(); assert(!g_rt_pending_n && !unused_entry->spilled);
    // Healthy headroom does not retain old scene storage indefinitely.
    g_cur_frame += RT_CACHE_IDLE_FRAMES; advance(); assert(unused_entry->pending && !g_rt_reclaiming);
    NSDictionary *idle_contents = contents(unused);
    finish_snapshot(unused_entry); rt_cache_use(unused); assert([idle_contents isEqual:contents(unused)]);
    // Both pressure levels must override ample per-process headroom.
    unsigned long levels[] = {DISPATCH_MEMORYPRESSURE_WARN, DISPATCH_MEMORYPRESSURE_CRITICAL};
    for (unsigned i = 0; i < 2; i++) { @autoreleasepool {
        clear(); MockTexture *t = texture(MTLPixelFormatRGBA8Unorm, 512);
        NvRtCache *entry = track(t, 0); g_cur_frame += 200; headroom = 1536u << 20;
        uint64_t events = atomic_load(&g_rt_pressure_events);
        rt_cache_memory_pressure(levels[i]);
        advance(); assert(!g_rt_pending_n); // Pressure cannot bypass GPU completion.
        g_done = 8; advance(); assert(entry->pending);
        assert([rt_cache_status()[@"pressure_events"] unsignedLongLongValue] == events + 1);
        assert([rt_cache_status()[@"pressure_flags"] unsignedLongValue] == levels[i]);
        rt_cache_memory_pressure(DISPATCH_MEMORYPRESSURE_NORMAL);
        finish_snapshot(entry); assert(!g_rt_reclaiming);
        rt_cache_use(t); g_cur_frame += 200; advance(); assert(!g_rt_pending_n);
    } }
    assert(pressure_hooks == 1);
    printf("texture snapshots: exact restore, small texture batches, staging lifetime/budget, cancellation, failure, pressure and headroom hysteresis checks passed\n");
} }
'''


def main():
    source = PREFIX + (ROOT / "nvn/nvn_rt_cache.inc").read_text() + TESTS
    with tempfile.TemporaryDirectory(prefix="nvn-texture-cache-cpu-") as directory:
        root = Path(directory)
        (root / "test.m").write_text(source)
        binary = root / "test"
        subprocess.run([
            "clang", "-x", "objective-c", "-fobjc-arc", "-std=gnu11",
            "-fsanitize=address,undefined", "-Wno-protocol", "-Wno-incomplete-implementation",
            "-Wno-objc-protocol-property-synthesis", "-I", str(ROOT / "nvn"),
            str(root / "test.m"), "-framework", "Foundation", "-framework", "Metal", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    main()
