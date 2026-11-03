"""Check allocation attribution with CPU-only objects; no Metal device is created."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "nvn/nvn_mtl.m").read_text()
start = source.index("NSDictionary *mtl_memory_snapshot(void) {")
function = source[start:source.index("\n}", start) + 2]
prefix = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
@interface FakeResource : NSObject
@property NSUInteger allocatedSize;
@property MTLStorageMode storageMode;
@property(strong) id<MTLHeap> heap;
@end
@implementation FakeResource @end
@interface FakeBuffer : FakeResource <MTLBuffer>
@property NSUInteger length;
@end
@implementation FakeBuffer @end
@interface FakeTexture : FakeResource <MTLTexture>
@property(strong) id<MTLTexture> parentTexture;
@property(strong) id<MTLBuffer> buffer;
@property NSUInteger width, height, depth, arrayLength, mipmapLevelCount;
@property MTLPixelFormat pixelFormat;
@property MTLTextureUsage usage;
@property MTLTextureType textureType;
@end
@implementation FakeTexture @end
@interface FakeHeap : NSObject <MTLHeap>
@property NSUInteger size;
@end
@implementation FakeHeap @end
static pthread_mutex_t g_res_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static NSHashTable *g_res_objects;
@interface NvPoolRange : NSObject {
@public uintptr_t lo, hi; id<MTLBuffer> buffer; uint64_t gpu;
}
@end
@implementation NvPoolRange @end
static char g_pool_ranges_key;
static int g_pool_ranges;
static struct { uintptr_t lo, hi, base; __unsafe_unretained id<MTLBuffer> buf; int res; } g_pools[2];
static int g_npool, g_pool_res_n;
static id<MTLDevice> g_dev;
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static _Atomic uint64_t g_pool_ranges_retired, g_pool_range_bytes_retired;
static NSDictionary *rt_cache_status(void) { return @{}; }
static NSDictionary *buffer_cache_status(void) { return @{}; }
static NSDictionary *cache_status(void) { return @{}; }
'''
test = r'''
int main(void) { @autoreleasepool {
    g_res_objects = [NSHashTable weakObjectsHashTable];
    NSDictionary *empty = mtl_memory_snapshot();
    assert([empty[@"unique_roots"] unsignedIntValue] == 0);
    FakeBuffer *pool = [FakeBuffer new]; pool.length = pool.allocatedSize = 1024;
    FakeBuffer *unused = [FakeBuffer new]; unused.length = unused.allocatedSize = 512;
    g_pools[0].buf = pool; g_pools[1].buf = unused; g_npool = 2; g_pool_res_n = 1;
    FakeTexture *view = [FakeTexture new]; view.buffer = pool; view.allocatedSize = 1024;
    FakeTexture *nested = [FakeTexture new]; nested.parentTexture = view; nested.allocatedSize = 1024;
    FakeTexture *rt = [FakeTexture new]; rt.allocatedSize = 2048;
    rt.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite;
    rt.width = rt.height = 16; rt.storageMode = MTLStorageModePrivate;
    FakeHeap *heap = [FakeHeap new]; heap.size = 4096;
    FakeTexture *a = [FakeTexture new], *b = [FakeTexture new];
    a.heap = b.heap = heap; a.allocatedSize = b.allocatedSize = 4096;
    for (id resource in @[pool, view, nested, rt, a, b]) [g_res_objects addObject:resource];
    NSDictionary *snapshot = mtl_memory_snapshot();
    assert([snapshot[@"registered_resources"] unsignedIntValue] == 6);
    assert([snapshot[@"unique_roots"] unsignedIntValue] == 5);
    assert([snapshot[@"texture_aliases"] unsignedIntValue] == 2);
    assert([snapshot[@"standalone_allocated_bytes"] unsignedLongLongValue] == 3584);
    assert([snapshot[@"referenced_heap_bytes"] unsignedLongLongValue] == 4096);
    unsigned pools = 0, targets = 0, heap_resources = 0;
    for (NSDictionary *group in snapshot[@"groups"]) {
        if ([group[@"kind"] isEqual:@"guest_pool"]) {
            pools += [group[@"count"] unsignedIntValue];
            assert([group[@"buffer_length_bytes"] unsignedIntValue] == 1536);
        }
        if ([group[@"kind"] isEqual:@"render_target"]) targets += [group[@"count"] unsignedIntValue];
        if ([group[@"heap_backed"] boolValue]) heap_resources += [group[@"count"] unsignedIntValue];
    }
    assert(pools == 2 && targets == 1 && heap_resources == 2);
    assert([NSJSONSerialization isValidJSONObject:snapshot]);
    // The returned diagnostics contain values only and must not keep resources alive.
    __weak FakeTexture *released;
    @autoreleasepool {
        FakeTexture *temporary = [FakeTexture new]; temporary.allocatedSize = 128;
        released = temporary; [g_res_objects addObject:temporary];
        snapshot = mtl_memory_snapshot();
    }
    assert(released == nil);
    assert([snapshot[@"unique_roots"] unsignedIntValue] == 6);
    puts("MEMORY_SNAPSHOT OK: aliases, pools, render targets, overlapping heaps, released resources");
} return 0; }
'''
with tempfile.TemporaryDirectory(prefix="nvn-memory-snapshot-") as directory:
    path = Path(directory)
    (path / "test.m").write_text(prefix + function + test)
    subprocess.run(["xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wno-protocol",
                    "-Wno-objc-property-implementation", "-Wno-objc-protocol-property-synthesis",
                    "-framework", "Foundation", "-framework", "Metal",
                    str(path / "test.m"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
