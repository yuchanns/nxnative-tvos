"""Exercise production BC batching with CPU-only completion and texture mocks."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "nvn/nvn_bc_copy.inc").read_text()
start = source.index("// Consecutive copies to one allocation")
end = source.index("static nvn_bc_region bc_copy_region", start)
batch = source[start:end]

PRELUDE = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdatomic.h>
#include "nvn_obj.h"

@interface MockTexture : NSObject {
@public unsigned pixels[2048];
}
@end
@implementation MockTexture @end
@interface NvBcBlocks : NSObject {
@public unsigned bytes[2048];
}
@end
@implementation NvBcBlocks @end
static NSMutableArray<void (^)(void)> *gpu;
static unsigned waits, conversions;
static uint64_t clock_ns;
static _Atomic uint64_t g_bc_copy_wait_ns, g_bc_copy_total_ns;
static uint64_t now_ns(void) { return ++clock_ns; }
static uint64_t bc_copy_wait(void) {
    waits++;
    for (void (^operation)(void) in gpu) operation();
    [gpu removeAllObjects];
    clock_ns += 100;
    return 100;
}
static unsigned plane(NSUInteger level, NSUInteger layer, NSUInteger z) {
    unsigned index = (unsigned)(level + 16 * layer + 512 * z);
    assert(index < 2048);
    return index;
}
static void bc_blocks_materialize_ready(id<MTLTexture> texture, NvBcBlocks *blocks,
                                        NSUInteger level, NSUInteger layer, NSUInteger z, int ready) {
    assert(ready && !gpu.count);
    unsigned index = plane(level, layer, z);
    ((MockTexture *)texture)->pixels[index] = blocks->bytes[index];
    conversions++;
}
static int tex_bc_decoded(const tex_t *texture) {
    return texture && texture->d.format == 0x41;
}
'''

TEST = r'''
static void encode(MockTexture *texture, NvBcBlocks *blocks, unsigned level,
                   unsigned layer, unsigned z, unsigned value) {
    bc_copy_prepare((id)texture);
    unsigned index = plane(level, layer, z);
    [gpu addObject:^{ blocks->bytes[index] = value; }];
    bc_copy_defer_plane((id)texture, blocks, level, layer, z);
}
int main(void) { @autoreleasepool {
    gpu = [NSMutableArray new]; g_bc_copy_defer = 1;
    MockTexture *a = [MockTexture new], *b = [MockTexture new];
    NvBcBlocks *blocks = [NvBcBlocks new];
    encode(a, blocks, 0, 0, 0, 17);
    encode(a, blocks, 1, 0, 0, 23);
    encode(a, blocks, 2, 0, 0, 31);
    assert(!waits && !conversions && !a->pixels[0]);
    bc_copy_flush();
    assert(waits == 1 && conversions == 3);
    assert(a->pixels[0] == 17 && a->pixels[1] == 23 && a->pixels[2] == 31);
    bc_copy_flush(); assert(waits == 1);
    assert(atomic_load(&g_bc_copy_wait_ns) == 100);
    assert(atomic_load(&g_bc_copy_total_ns) >= 100);

    // Repeated partial writes without an intervening reader expose final bytes.
    encode(a, blocks, 0, 0, 0, 41);
    encode(a, blocks, 0, 0, 0, 43);
    bc_copy_flush();
    assert(waits == 2 && conversions == 4 && a->pixels[0] == 43);

    // An allocation alias sees the earlier result before its own write begins.
    encode(a, blocks, 0, 0, 0, 47);
    encode(b, blocks, 0, 0, 0, 53);
    assert(a->pixels[0] == 47 && !b->pixels[0]);
    bc_copy_flush(); assert(b->pixels[0] == 53 && a->pixels[0] == 47);

    // Capacity splits remain bounded and retain every distinct layer and level.
    unsigned old_waits = waits, old_conversions = conversions;
    for (unsigned i = 0; i < BC_COPY_BATCH_CAP + 3; i++)
        encode(a, blocks, i % 16, i / 16, 0, i + 100);
    assert(g_bc_copy_pending_n == 3);
    bc_copy_flush();
    assert(waits == old_waits + 2 && conversions == old_conversions + BC_COPY_BATCH_CAP + 3);
    for (unsigned i = 0; i < BC_COPY_BATCH_CAP + 3; i++) assert(a->pixels[i] == i + 100);
    encode(a, blocks, 2, 1, 1, 997); bc_copy_flush();
    assert(a->pixels[plane(2, 1, 1)] == 997);

    // Queue ownership protects objects until their final sampling upload.
    __weak MockTexture *weak_texture;
    __weak NvBcBlocks *weak_blocks;
    @autoreleasepool {
        MockTexture *temporary = [MockTexture new];
        NvBcBlocks *storage = [NvBcBlocks new];
        weak_texture = temporary; weak_blocks = storage;
        encode(temporary, storage, 0, 0, 0, 61);
    }
    assert(weak_texture && weak_blocks);
    @autoreleasepool { bc_copy_flush(); }
    assert(!weak_texture && !weak_blocks);

    tex_t texture = {0}; texture.d.format = 0x41;
    p_copy_b2t copy = {.tex = &texture};
    for (unsigned op = 0; op <= OP_COUNT_ + 1; op++)
        assert(bc_copy_batch_keeps(op, &copy) == (op == OP_COPY_STRIDE || op == OP_COPY_B2T));
    texture.d.format = 0;
    assert(!bc_copy_batch_keeps(OP_COPY_B2T, &copy));
    copy.tex = NULL; assert(!bc_copy_batch_keeps(OP_COPY_B2T, &copy));
    assert(!g_bc_copy_pending_n && !gpu.count);
    puts("BC_COPY_BATCH OK: completion, coalescing, aliases, capacity, depth, lifetime, consumer boundaries");
} return 0; }
'''

with tempfile.TemporaryDirectory(prefix="nx-bc-copy-batch-") as directory:
    path = Path(directory)
    (path / "test.m").write_text(PRELUDE + batch + TEST)
    subprocess.run([
        "xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", "-fsanitize=address,undefined", "-g",
        "-I" + str(ROOT / "nvn"), "-I" + str(ROOT / "nvn/include"), "-I" + str(ROOT / "tex"),
        str(path / "test.m"), "-o", str(path / "test"), "-framework", "Foundation",
    ], check=True)
    subprocess.run([str(path / "test")], check=True)
