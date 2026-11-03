"""Exercise production asset-cache state transitions with CPU-only Metal doubles."""

import itertools
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
        raise AssertionError("Cannot find production function " + name)
    end = SOURCE.index("\n}", match.start()) + 2
    return SOURCE[match.start():end]


PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_asset_retention.h"
#include "nvn_resource_usage.h"

#undef TARGET_OS_IPHONE
#undef TARGET_OS_TV
#define TARGET_OS_IPHONE 1
#define TARGET_OS_TV 1
#define MLOG(...) ((void)0)
#define MDIE(...) abort()
enum { NVN_TEXTURE_FLAGS_IMAGE = 4 };
typedef struct { struct { unsigned flags; } d; } tex_t;

@interface MockTexture : NSObject <MTLTexture>
@property(nonatomic, strong) id<MTLTexture> parentTexture;
@property(nonatomic, strong) id<MTLBuffer> buffer;
@property(nonatomic, strong) id<MTLHeap> heap;
@property(nonatomic) NSUInteger allocatedSize;
@property(nonatomic) MTLPurgeableState state;
@property(nonatomic) unsigned purges;
@property(nonatomic) BOOL registered;
@end
@implementation MockTexture
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)value {
    MTLPurgeableState before = self.state;
    self.state = value;
    if (value == MTLPurgeableStateVolatile) self.purges++;
    return before;
}
@end

@interface MockEncoder : NSObject <MTLRenderCommandEncoder, MTLComputeCommandEncoder>
@property(nonatomic) unsigned declarations;
@end
@implementation MockEncoder
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage {
    (void)resource; (void)usage; self.declarations++;
}
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages {
    (void)resource; (void)usage; (void)stages; self.declarations++;
}
@end

@interface NvPkg : NSObject {
@public
    id<MTLTexture> tex, spare;
    uint64_t used_fence, used_frame, trim_frame, spare_fence;
    int reloadable, volatile_contents;
    tex_t source;
}
@end
@implementation NvPkg
@end

static NSMapTable<id<MTLTexture>, NvPkg *> *g_pkg_cache;
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence, g_done, g_ser_gen;
static _Atomic uint64_t g_cur_frame;
static _Atomic unsigned g_up_pending;
static uint64_t g_pkg_purged_bytes, g_pkg_restored_bytes, g_pkg_reloads;
static int g_nvn_cpu_bind_fast;
static nvn_resource_usage g_resource_usage;
static __unsafe_unretained id<MTLCommandEncoder> g_resource_encoder;
static NSMutableArray<NSArray *> *g_prepare_resources;
static uint64_t g_use_calls, g_use_skips;
enum { SHADER_RESOURCE = 1, SHADER_TEXTURE = 2 };
static size_t os_proc_available_memory(void) { return 512u << 20; }
static void res_remove(id<MTLResource> texture) { ((MockTexture *)texture).registered = NO; }
static void res_commit(void) {}
static void rt_cache_use(id<MTLTexture> texture) { (void)texture; }
static char g_pool_range_use_key;
static void pool_range_use(id usage) { (void)usage; }
'''


TEST = r'''
static unsigned checks, failures;
#define CHECK(value) do { checks++; if (!(value)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); failures++; } } while (0)

static NvPkg *package(unsigned flags, int sparse) {
    MockTexture *texture = [MockTexture new];
    texture.allocatedSize = 16u << 20;
    texture.registered = YES;
    texture.state = MTLPurgeableStateNonVolatile;
    NvPkg *pk = [NvPkg new]; pk->tex = texture;
    tex_t source = {.d.flags = flags};
    pk->reloadable = pkg_cache_initial_reloadable(&source, sparse);
    pkg_cache_track(pk, &source);
    return pk;
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 5) return 2;
    setenv("DM_NVN_RESOURCE_CACHE", argv[1], 1);
    g_nvn_cpu_bind_fast = atoi(argv[2]);
    setenv("DM_NVN_ASSET_KEEP_RECENT", argv[3], 1);
    if (!strcmp(argv[4], "unset")) unsetenv("DM_NVN_ASSET_PURGE_IMAGES");
    else setenv("DM_NVN_ASSET_PURGE_IMAGES", argv[4], 1);
    setenv("DM_GUEST_FILE_BACKING", "1", 1);
    const int images = strcmp(argv[4], "0") != 0;
    g_fence = 7; g_done = 7; g_cur_frame = 10;
    NvPkg *read = package(NVN_TEXTURE_FLAGS_IMAGE, 0);
    CHECK(read->reloadable == images);
    CHECK(read->source.d.flags == NVN_TEXTURE_FLAGS_IMAGE);
    CHECK(read->used_fence == 8 && read->used_frame == 10);
    MockEncoder *encoder = [MockEncoder new];
    shader_use(encoder, 1, 0, read->tex, MTLResourceUsageRead);
    shader_use(encoder, 1, 0, read->tex, MTLResourceUsageRead);
    CHECK(read->reloadable == images);
    CHECK(encoder.declarations == (atoi(argv[1]) ? 1u : 2u));
    // Age alone cannot evict an allocation still used by the GPU.
    g_cur_frame = 200; pkg_cache_trim();
    CHECK(!read->volatile_contents);
    CHECK(((MockTexture *)read->tex).registered);
    g_done = 8; g_cur_frame = 201; pkg_cache_trim();
    CHECK(read->volatile_contents == images);
    CHECK(((MockTexture *)read->tex).purges == (unsigned)images);
    CHECK(((MockTexture *)read->tex).registered == !images);
    CHECK(g_pkg_purged_bytes == (images ? 16u << 20 : 0));
    // A restored allocation becomes usable before its next shader binding.
    [read->tex setPurgeableState:MTLPurgeableStateNonVolatile];
    read->volatile_contents = 0;
    ((MockTexture *)read->tex).registered = YES;
    g_fence = 9; g_cur_frame = 210;
    encoder = [MockEncoder new];
    shader_use(encoder, 1, 0, read->tex, MTLResourceUsageRead);
    CHECK(read->used_fence == 10 && read->used_frame == 210);
    CHECK(read->reloadable == images);
    // A write upgrade in the same encoder must reach cache accounting.
    shader_use(encoder, 1, 0, read->tex, MTLResourceUsageRead | MTLResourceUsageWrite);
    CHECK(!read->reloadable);
    CHECK(encoder.declarations == 2);
    g_done = 10; g_cur_frame = 500; pkg_cache_trim();
    CHECK(!read->volatile_contents);
    CHECK(((MockTexture *)read->tex).registered);
    // A nested view's writes revoke the base allocation's reloadability.
    NvPkg *viewed = package(NVN_TEXTURE_FLAGS_IMAGE, 0);
    MockTexture *view = [MockTexture new], *nested = [MockTexture new];
    view.parentTexture = viewed->tex; nested.parentTexture = view;
    encoder = [MockEncoder new];
    shader_use(encoder, 5, -1, nested, MTLResourceUsageRead | MTLResourceUsageWrite);
    CHECK(!viewed->reloadable && viewed->used_fence == 10);
    g_cur_frame = 700; pkg_cache_trim();
    CHECK(!viewed->volatile_contents);
    // Indirect dispatch preparation records access before it has an encoder.
    NvPkg *prepared = package(NVN_TEXTURE_FLAGS_IMAGE, 0);
    g_prepare_resources = [NSMutableArray array];
    shader_use(nil, 5, -1, prepared->tex, MTLResourceUsageRead);
    shader_use(nil, 5, -1, prepared->tex, MTLResourceUsageRead | MTLResourceUsageWrite);
    CHECK(!prepared->reloadable && g_prepare_resources.count == 2);
    // Direct attachment/copy/clear writers use this same accounting entry.
    NvPkg *written = package(0, 0);
    CHECK(written->reloadable);
    pkg_cache_use(written->tex, 1);
    CHECK(!written->reloadable);
    g_cur_frame = 900; pkg_cache_trim();
    CHECK(!written->volatile_contents);
    CHECK(!prepared->volatile_contents);
    NvPkg *sparse = package(NVN_TEXTURE_FLAGS_IMAGE, 1);
    CHECK(!sparse->reloadable);
    NvPkg *ordinary = package(0, 0);
    CHECK(ordinary->reloadable);
    NvPkg *young = package(0, 0);
    g_cur_frame = 904; pkg_cache_trim();
    CHECK(!young->volatile_contents && !ordinary->volatile_contents);
    // Pending uploads block trimming even after the last draw completed.
    g_up_pending = 1; g_cur_frame = 1100; pkg_cache_trim();
    CHECK(!ordinary->volatile_contents);
    g_up_pending = 0; g_cur_frame = 1101; pkg_cache_trim();
    CHECK(ordinary->volatile_contents && young->volatile_contents);
    CHECK(!sparse->volatile_contents && !written->volatile_contents);
    CHECK(!read->volatile_contents && !viewed->volatile_contents);
    // Already volatile resources are not counted a second time.
    uint64_t purged = g_pkg_purged_bytes;
    g_cur_frame = 1300; pkg_cache_trim();
    CHECK(g_pkg_purged_bytes == purged);
    // Preserve both kinds of externally owned backing storage.
    NvPkg *buffer = package(0, 0), *heap = package(0, 0);
    ((MockTexture *)buffer->tex).buffer = (id<MTLBuffer>)[NSObject new];
    ((MockTexture *)heap->tex).heap = (id<MTLHeap>)[NSObject new];
    g_cur_frame = 1500; pkg_cache_trim();
    CHECK(!buffer->volatile_contents && !heap->volatile_contents);
    printf("asset cache: cache=%s fast=%s recent=%s images=%s checks=%u failures=%u\n",
           argv[1], argv[2], argv[3], argv[4], checks, failures);
    return failures ? 1 : 0;
} }
'''


def main():
    # These direct writers must invalidate reloadability before encoding writes.
    for writer in ("attach", "copy_b2t", "copy_t2t", "clear_tex", "clear_rect_zero"):
        if not re.search(r"pkg_cache_use\([^,]+, 1\)", function(writer)):
            raise AssertionError("Missing write accounting in " + writer)
    names = ("pkg_cache_on", "pkg_cache_initial_reloadable", "pkg_cache_track",
             "pkg_cache_use", "pkg_cache_trim", "shader_resource_kind", "shader_use")
    source = PREFIX + "\n".join(function(name) for name in names) + TEST
    with tempfile.TemporaryDirectory(prefix="nvn-asset-cache-") as directory:
        path = Path(directory)
        (path / "test.m").write_text(source)
        binary = path / "test"
        subprocess.run([
            "clang", "-x", "objective-c", "-fobjc-arc", "-std=gnu11",
            "-fsanitize=address,undefined", "-Wno-protocol", "-Wno-incomplete-implementation",
            "-Wno-objc-protocol-property-synthesis",
            "-I", str(ROOT / "nvn"), str(path / "test.m"),
            "-framework", "Foundation", "-framework", "Metal", "-o", str(binary),
        ], check=True)
        variants = list(itertools.product(("0", "1"), repeat=4)) + [("1", "1", "0", "unset")]
        for values in variants:
            subprocess.run([str(binary), *values], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


if __name__ == "__main__":
    main()
