"""Check real staging-cache retirement using CPU-only Metal doubles."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "nvn/nvn_mtl.m").read_text()
ARENAS = SOURCE[SOURCE.index("@interface NvArena"):SOURCE.index("// ---------- Draw state")]
ZERO = SOURCE[SOURCE.index("static id<MTLBuffer> g_zero_buffer;"):SOURCE.index("static uint64_t g_tex_zeroed;")]
start = SOURCE.index("static void up_cache_trim(void) {")
UPLOAD = SOURCE[start:SOURCE.index("\n}", start) + 2]
PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <assert.h>
#define MDIE(...) abort()
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER, g_deferred_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence, g_done, g_arena_made;
static _Atomic uint64_t g_cur_frame;
static _Atomic int g_up_pending;
static NSHashTable *registered;
@interface MockBuffer : NSObject <MTLBuffer>
@property(strong) NSMutableData *data;
@end
@implementation MockBuffer
- (NSUInteger)length { return self.data.length; }
- (NSUInteger)allocatedSize { return self.data.length; }
- (void *)contents { return self.data.mutableBytes; }
@end
@interface MockDevice : NSObject
@property NSUInteger maxBufferLength;
@end
@implementation MockDevice
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)options {
    (void)options; MockBuffer *b = [MockBuffer new]; b.data = [NSMutableData dataWithLength:n]; return b;
}
@end
static MockDevice *g_dev;
static uint64_t buffer_gpu_address(id<MTLBuffer> b) { return (uintptr_t)b.contents; }
static void res_add(id resource) { [registered addObject:resource]; }
static void res_remove(id resource) { assert([registered containsObject:resource]); [registered removeObject:resource]; }
@interface NvDeferredRelease : NSObject { @public id object; uint64_t fence; } @end
@implementation NvDeferredRelease @end
static NSMutableArray<NvDeferredRelease *> *g_deferred;
static void defer_obj_release(id object) { NvDeferredRelease *d = [NvDeferredRelease new]; d->object = object; d->fence = g_fence + 1; if (!g_deferred) g_deferred = [NSMutableArray new]; [g_deferred addObject:d]; }
static id<MTLCommandBuffer> g_up_cb;
static _Atomic int g_up_alloc_busy[3];
static id<MTLBuffer> g_up_slot_blk[3], g_up_shared_blk[3];
static uint64_t g_up_slot_frame[3], g_up_shared_bytes;
'''
TESTS = r'''
static void use_arena(void) {
    @autoreleasepool {
        arena_begin(); id<MTLBuffer> buffer; arena_alloc(ARENA_SIZE, &buffer);
        arena_commit(++g_fence); g_done = g_fence;
    }
}
int main(void) { @autoreleasepool {
    registered = [NSHashTable weakObjectsHashTable]; g_dev = [MockDevice new]; g_dev.maxBufferLength = 256 << 20;
    g_cur_frame = 1;
    __weak id<MTLBuffer> retired;
    @autoreleasepool {
        arena_begin(); id<MTLBuffer> buffer;
        for (unsigned i = 0; i < 10; i++) { arena_alloc(ARENA_SIZE, &buffer); if (i == 9) retired = buffer; }
        arena_commit(++g_fence);
    }
    g_cur_frame += 200; arena_begin(); assert(g_arenas.count == 10); // In-flight work cannot be evicted.
    arena_commit(g_fence); g_done = g_fence;
    @autoreleasepool { arena_begin(); assert(!g_arenas.count); arena_commit(g_fence); }
    assert(!retired);
    use_arena(); assert(g_arenas.count == 1);
    // A transient burst must shrink back to one active block without steadily
    // allocating new blocks during normal, repeated submissions.
    for (unsigned i = 0; i < 240; i++) { g_cur_frame++; use_arena(); }
    assert(g_arenas.count == 1 && g_arena_made == 11);
    @autoreleasepool {
        arena_begin(); id<MTLBuffer> large; arena_alloc(ARENA_SIZE + 256, &large); arena_commit(++g_fence);
        assert(g_deferred.count == 1 && g_deferred[0]->object == large && g_deferred[0]->fence == g_fence);
    }
    g_cur_frame += 300;
    for (unsigned i = 0; i < 3; i++) {
        g_up_slot_blk[i] = [g_dev newBufferWithLength:4096 options:0]; res_add(g_up_slot_blk[i]);
        g_up_shared_blk[i] = [g_dev newBufferWithLength:8192 options:0]; res_add(g_up_shared_blk[i]);
        g_up_shared_bytes += 8192; g_up_slot_frame[i] = g_cur_frame;
    }
    g_cur_frame += 120; g_up_alloc_busy[1] = 1; up_cache_trim();
    assert(!g_up_slot_blk[0] && g_up_slot_blk[1] && !g_up_slot_blk[2] && g_up_shared_bytes == 8192);
    g_up_alloc_busy[1] = 0; up_cache_trim(); assert(!g_up_slot_blk[1] && !g_up_shared_bytes);
    @autoreleasepool {
        id<MTLBuffer> zero = zero_buf(65536); const uint8_t *bytes = zero.contents;
        for (NSUInteger i = 0; i < zero.length; i++) assert(!bytes[i]);
        retired = zero;
    }
    g_cur_frame += 120; zero_cache_trim(); assert(g_zero_buffer);
    g_done = g_fence + 1; g_up_pending = 1; zero_cache_trim(); assert(g_zero_buffer);
    g_up_pending = 0; @autoreleasepool { zero_cache_trim(); }
    assert(!g_zero_buffer && !retired);
    assert(zero_buf(65536));
    puts("STAGING_RETIREMENT OK: in-flight safety, peak shrink, active reuse, upload completion, zero regeneration");
} return 0; }
'''
with tempfile.TemporaryDirectory(prefix="nvn-staging-retirement-") as directory:
    path = Path(directory)
    (path / "test.m").write_text(PREFIX + ARENAS + ZERO + UPLOAD + TESTS)
    command = ["xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wno-protocol", "-Wno-objc-protocol-property-synthesis",
               "-framework", "Foundation", "-framework", "Metal", str(path / "test.m"), "-o", str(path / "test")]
    if os.environ.get("NVN_TEST_SANITIZE"):
        command[2:2] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    subprocess.run(command, check=True)
    subprocess.run([str(path / "test")], check=True)
