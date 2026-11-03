"""Exercise production pool windows and replay gates with CPU-only Metal mocks."""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "nvn/nvn_mtl.m").read_text()


def function(name, source=SOURCE):
    match = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^\n]*\) \{", source, re.M)
    assert match, name
    start = source.index("{", match.start())
    depth = 0
    for end in range(start, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if not depth:
            return source[match.start():end + 1] + "\n"
    raise AssertionError(name)


COMMON = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include "nvn_pool_range.h"
#define MDIE(...) abort()
'''

POOL = r'''
@interface MockBuffer : NSObject
@property NSUInteger length;
@property void *contents;
@end
@implementation MockBuffer @end
@interface NvPoolRangeUse : NSObject {
@public uint64_t frame, fence; atomic_int excluded;
}
@end
@implementation NvPoolRangeUse @end
@interface NvPoolRange : NSObject {
@public uintptr_t lo, hi; id<MTLBuffer> buffer; uint64_t gpu; NvPoolRangeUse *use;
}
@end
@implementation NvPoolRange @end
static char g_pool_ranges_key, g_pool_range_use_key;
static int g_pool_ranges = 1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_pool_gen = 1, g_reg_gen, g_cur_frame;
static _Atomic uint64_t g_pool_ranges_retired, g_pool_range_bytes_retired;
static uint64_t g_fence, g_done;
static int g_npool = 1, submit_thread = 1;
static int on_sub_thread(void) { return submit_thread; }
typedef struct { uintptr_t lo, hi, base; __unsafe_unretained id<MTLBuffer> buf;
                 uint64_t gpu; const void *key; int res; } pool_ent;
static pool_ent g_pools[1];
typedef struct { uintptr_t lo, hi, base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; } pc_ent;
static __thread struct { uint64_t gen; unsigned n, next; pc_ent e[8];
    __unsafe_unretained NvPoolRangeUse *use[8]; } t_range_pc;
static NSMutableArray *retired;
static unsigned made, full_bindings, registered;
static int reject;
static int pool_find(uint64_t addr, uint64_t len) {
    return len && addr >= g_pools[0].lo && addr <= g_pools[0].hi &&
        len <= g_pools[0].hi - addr ? 0 : -1;
}
static uint64_t buffer_gpu_address(id<MTLBuffer> buffer) { return (uintptr_t)buffer.contents; }
static id<MTLBuffer> guest_pool_buffer(uintptr_t base, size_t bytes) {
    if (reject) return nil;
    MockBuffer *buffer = [MockBuffer new]; buffer.contents = (void *)base; buffer.length = bytes;
    made++; return (id)buffer;
}
static void pool_res(int i) { g_pools[i].res = 1; }
static void res_add(id buffer) { (void)buffer; registered++; }
static void res_commit(void) {}
static void defer_obj_release(id buffer) { [retired addObject:buffer]; }
static int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buffer, uint64_t *offset, uint64_t *gpu) {
    if (pool_find(addr, len) < 0) return 0;
    pool_res(0);
    full_bindings++; *buffer = (__bridge void *)g_pools[0].buf;
    *offset = addr - g_pools[0].base; *gpu = g_pools[0].gpu + *offset; return 1;
}
'''

POOL_TEST = r'''
int main(void) { @autoreleasepool {
    const uintptr_t quantum = 1u << 20;
    uintptr_t base, bytes;
    assert(!nvn_pool_window(0, 0, 0, quantum, quantum, &base, &bytes));
    assert(!nvn_pool_window(UINTPTR_MAX - 1, 4, UINTPTR_MAX - 8, 8, quantum, &base, &bytes));
    assert(!nvn_pool_window(16, 1, 0, 16, quantum, &base, &bytes));
    assert(!nvn_pool_window(1, 1, 0, 16, 3, &base, &bytes));
    for (uintptr_t at = 0; at < 3 * quantum + 16384; at += 7919) {
        uintptr_t size = 3 * quantum + 16384;
        uintptr_t len = MIN((uintptr_t)65539, size - at);
        assert(nvn_pool_window(16384 + at, len, 16384, size, quantum, &base, &bytes));
        assert(base >= 16384 && base <= 16384 + at && !(base % 16384));
        assert(bytes && !(bytes % 16384));
        assert(base + bytes >= 16384 + at + len && base + bytes <= 16384 + size);
    }
    void *storage = NULL;
    assert(posix_memalign(&storage, 16384, 8 * quantum) == 0);
    MockBuffer *owner = [MockBuffer new]; owner.contents = storage; owner.length = 8 * quantum;
    uintptr_t lo = (uintptr_t)storage;
    g_pools[0] = (pool_ent){lo, lo + 8 * quantum, lo, (id)owner, lo, storage, 0};
    retired = [NSMutableArray array];
    __weak id old;
    @autoreleasepool {
    void *a, *b; uint64_t off, gpu;
    assert(mtl_gpu_range(lo + 128, 256, &a, &off, &gpu));
    assert(made == 1 && off == 128 && gpu == lo + 128 && !full_bindings);
    assert(((__bridge id<MTLBuffer>)a).length == quantum);
    assert(mtl_gpu_range(lo + 512, 4096, &b, &off, &gpu) && a == b && made == 1);
    ((unsigned char *)storage)[512] = 77;
    assert(((unsigned char *)((__bridge id<MTLBuffer>)b).contents)[off] == 77);
    assert(mtl_gpu_range(lo + quantum - 16, 32, &b, &off, &gpu) && a != b);
    assert(((__bridge id<MTLBuffer>)b).length == 2 * quantum && made == 2);
    assert(!mtl_gpu_range(lo + 8 * quantum - 4, 8, &b, &off, &gpu));
    assert(!mtl_gpu_range(UINTPTR_MAX - 2, 4, &b, &off, &gpu));
    assert(mtl_gpu_range(lo + 3 * quantum, 0, &b, &off, &gpu));
    assert(b == (__bridge void *)owner && full_bindings == 1);
    // Full-pool residency reuses that alias rather than making another window.
    assert(mtl_gpu_range(lo + 4 * quantum, 64, &b, &off, &gpu));
    assert(b == (__bridge void *)owner && made == 2);
    g_pools[0].res = 0; atomic_fetch_add(&g_pool_gen, 1);
    reject = 1;
    assert(mtl_gpu_range(lo + 4 * quantum, 64, &b, &off, &gpu));
    assert(b == (__bridge void *)owner && full_bindings == 2);
    reject = 0;
    g_pools[0].res = 0;
    @autoreleasepool {
        NSArray *ranges = objc_getAssociatedObject(owner, &g_pool_ranges_key);
        old = ((NvPoolRange *)ranges[0])->buffer;
        pool_ranges_retire((id)owner);
        objc_setAssociatedObject(owner, &g_pool_ranges_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        atomic_fetch_add(&g_pool_gen, 1);
        assert(old != nil && retired.count == 2);
        assert(mtl_gpu_range(lo + 128, 256, &b, &off, &gpu) && b != a && made == 3);
    }
    }
    [retired removeAllObjects];
    assert(old == nil && registered == 3);
    objc_setAssociatedObject(owner, &g_pool_ranges_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    free(storage);
    puts("POOL_RANGES OK: bounds, overflow, reuse, coherent aliases, unknown extents, retirement");
} return 0; }
'''

LIFETIME_TEST = r'''
int main(void) { @autoreleasepool {
    const size_t quantum = 1u << 20;
    void *storage = NULL; assert(!posix_memalign(&storage, 16384, 8 * quantum));
    MockBuffer *owner = [MockBuffer new]; owner.contents = storage; owner.length = 8 * quantum;
    uintptr_t lo = (uintptr_t)storage;
    g_pools[0] = (pool_ent){lo, lo + 8 * quantum, lo, (id)owner, lo, storage, 0};
    retired = [NSMutableArray array];
    g_cur_frame = 10; g_fence = 5;
    __weak id old;
    @autoreleasepool {
        void *a, *b, *c; uint64_t off, gpu;
        assert(mtl_gpu_range(lo + 64, 128, &a, &off, &gpu));
        assert(mtl_gpu_range(lo + 2 * quantum, 128, &b, &off, &gpu));
        assert(mtl_gpu_range(lo + 4 * quantum, 128, &c, &off, &gpu));
        pool_range_keep((__bridge id)b);
        old = (__bridge id)c;
        g_done = 5; g_cur_frame = 130; pool_ranges_trim();
        assert(!retired.count); // Old enough, but the last GPU use is in flight.
        g_fence = 6;
        assert(mtl_gpu_range(lo + 64, 128, &b, &off, &gpu) && b == a);
        g_done = 6; g_cur_frame = 160; pool_ranges_trim();
        assert(retired.count == 1 && old != nil);
        assert(g_pool_ranges_retired == 1 && g_pool_range_bytes_retired == quantum);
        assert(g_reg_gen == 1);
        assert(mtl_gpu_range(lo + 64, 128, &b, &off, &gpu) && b == a);
        // The retired TLS entry must be invalidated before its buffer is freed.
    }
    [retired removeAllObjects]; assert(old == nil);
    void *b; uint64_t off, gpu;
    ((uint8_t *)storage)[4 * quantum] = 71;
    assert(mtl_gpu_range(lo + 4 * quantum, 128, &b, &off, &gpu) && made == 4);
    assert(((uint8_t *)((__bridge id<MTLBuffer>)b).contents)[off] == 71);
    g_done = 7; g_cur_frame = 279; pool_ranges_trim();
    assert(!retired.count); // The refreshed use is only 119 frames old.
    g_cur_frame = 280; pool_ranges_trim(); assert(!retired.count); // Scan cadence.
    g_cur_frame = 310; pool_ranges_trim(); assert(retired.count == 2);
    NSArray *ranges = objc_getAssociatedObject(owner, &g_pool_ranges_key);
    assert(ranges.count == 1 && atomic_load(&((NvPoolRange *)ranges[0])->use->excluded));
    submit_thread = 0;
    assert(mtl_gpu_range(lo + 6 * quantum, 128, &b, &off, &gpu));
    assert(b == (__bridge void *)owner && made == 4);
    objc_setAssociatedObject(owner, &g_pool_ranges_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [retired removeAllObjects]; free(storage);
    puts("POOL_LIFETIME OK: in-flight fence, cached use, idle expiry, view retention, TLS invalidation, off-thread fallback");
} return 0; }
'''

BINDINGS = r'''
#include "nvn_resource_usage.h"
@interface MockDevice : NSObject
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options;
@end
@implementation MockDevice
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    (void)options; MockBuffer *buffer = [MockBuffer new];
    static uint8_t zero[64]; buffer.contents = zero; buffer.length = length; return buffer;
}
@end
@interface MockEncoder : NSObject
- (void)useResource:(id)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages;
@end
@implementation MockEncoder
- (void)useResource:(id)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages {
    (void)resource; (void)usage; (void)stages;
}
@end
static id<MTLDevice> g_dev;
static struct { struct { uint64_t address, size; } vbuf[16]; } S;
static int g_vbuf_cache = 1;
static uint64_t g_vbuf_reuse, g_vbuf_resolve, last_vertex;
#define VB_SLOT(slot) (slot)
static void enc_r_vbuf(id en, unsigned slot, uint64_t address) { (void)en; if (slot == 0) last_vertex = address; }
enum { SHADER_RESOURCE = 1, SHADER_TEXTURE = 2 };
static int shader_resource_kind(id object) { (void)object; return SHADER_RESOURCE; }
static int g_nvn_cpu_bind_fast = 1;
static id g_resource_encoder;
static nvn_resource_usage g_resource_usage;
static uint64_t g_use_calls, g_use_skips;
static NSMutableArray *g_prepare_resources;
static void pkg_cache_use(id object, int write) { (void)object; (void)write; abort(); }
static void rt_cache_use(id object) { (void)object; abort(); }
'''

BINDINGS_TEST = r'''
int main(void) { @autoreleasepool {
    const size_t size = 8u << 20;
    void *storage = NULL; assert(!posix_memalign(&storage, 16384, size));
    MockBuffer *owner = [MockBuffer new]; owner.contents = storage; owner.length = size;
    uintptr_t lo = (uintptr_t)storage;
    g_pools[0] = (pool_ent){lo, lo + size, lo, (id)owner, lo, storage, 0};
    retired = [NSMutableArray array]; g_dev = (id)[MockDevice new];
    S.vbuf[0].address = lo + 64; S.vbuf[0].size = 128;
    g_fence = 5; g_done = 6; g_cur_frame = 10;
    bind_vbufs(nil); assert(g_vbuf_resolve == 1 && last_vertex == lo + 64);
    g_cur_frame = 100; bind_vbufs(nil); assert(g_vbuf_reuse == 1);
    g_cur_frame = 160; pool_ranges_trim(); assert(!retired.count);
    void *buffer; uint64_t off, gpu;
    assert(mtl_gpu_range(lo + (2u << 20), 128, &buffer, &off, &gpu));
    MockEncoder *encoder = [MockEncoder new];
    g_cur_frame = 200;
    shader_use((id)encoder, 1, 0, (__bridge id)buffer, MTLResourceUsageRead);
    shader_use((id)encoder, 1, 0, (__bridge id)buffer, MTLResourceUsageRead);
    assert(g_use_calls == 1 && g_use_skips == 1);
    g_cur_frame = 230; pool_ranges_trim(); assert(retired.count == 1);
    [retired removeAllObjects];
    bind_vbufs(nil); assert(g_vbuf_resolve == 2 && last_vertex == lo + 64);
    g_cur_frame = 300; encoder = [MockEncoder new];
    shader_use((id)encoder, 1, 0, (__bridge id)buffer, MTLResourceUsageRead);
    pool_ranges_trim(); assert(!retired.count && g_use_calls == 2);
    g_cur_frame = 450; pool_ranges_trim(); assert(retired.count == 2);
    objc_setAssociatedObject(owner, &g_pool_ranges_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [retired removeAllObjects]; free(storage);
    puts("POOL_BINDINGS OK: vertex cache hits, argument reuse, encoder reset, generation invalidation");
} return 0; }
'''

UPLOAD = r'''
static const pc_ent *pc_get(uint64_t addr, uint64_t len) { (void)addr; (void)len; return NULL; }
'''

CPU_HASH = r'''
#include <CommonCrypto/CommonDigest.h>
#include "include/nvn.h"
'''

CPU_HASH_TEST = r'''
int main(void) { @autoreleasepool {
    const size_t size = 8u << 20;
    void *storage = NULL; assert(!posix_memalign(&storage, 16384, size));
    MockBuffer *owner = [MockBuffer new]; owner.contents = storage; owner.length = size;
    uintptr_t lo = (uintptr_t)storage;
    g_pools[0] = (pool_ent){lo, lo + size, lo, (id)owner, lo, storage, 0};
    uint8_t control[0x880] = {0}, expected[CC_SHA256_DIGEST_LENGTH], actual[8], direct[8];
    uint32_t code_size = 64;
    memcpy(control + 0x704, &code_size, 4);
    for (unsigned i = 0; i < code_size; i++) ((uint8_t *)storage)[128 + i] = (uint8_t)(3 * i);
    NVNshaderData shader = {.data = lo + 128, .control = control};
    for (uint32_t length = 0x850; length <= 0x880; length += 0x30) {
        memcpy(control + 0x20, &length, 4);
        CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
        CC_SHA256_Update(&hash, control, length);
        CC_SHA256_Update(&hash, (uint8_t *)storage + 128, code_size);
        CC_SHA256_Final(expected, &hash);
        assert(keys_hash(&shader, 1, actual) && !memcmp(actual, expected, 8));
        assert(keys_hash(&shader, 0, direct) && !memcmp(actual, direct, 8));
        assert(!g_pools[0].res && !registered && !made && !full_bindings);
    }
    ((uint8_t *)storage)[128] ^= 1;
    assert(keys_hash(&shader, 1, direct) && memcmp(actual, direct, 8));
    shader.data = lo + size - 32; assert(!keys_hash(&shader, 1, actual));
    shader.data = UINT64_MAX - 15; assert(!keys_hash(&shader, 1, actual));
    shader.data = 0; assert(!keys_hash(&shader, 1, actual));
    shader.data = lo + 128; shader.control = NULL; assert(!keys_hash(&shader, 1, actual));
    assert(!mtl_cpu_addr(lo, 0) && !mtl_cpu_addr(lo + size, 1));
    assert(!mtl_cpu_addr(UINT64_MAX - 15, 32));
    void *binding; uint64_t offset, gpu;
    assert(mtl_gpu_range(lo + 128, 64, &binding, &offset, &gpu));
    assert(((__bridge id<MTLBuffer>)binding).length == (1u << 20));
    assert(!g_pools[0].res && !full_bindings && made == 1);
    objc_setAssociatedObject(owner, &g_pool_ranges_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    free(storage);
    puts("CPU_HASH OK: identical keys, changed bytes, bounds, no full-pool promotion");
} return 0; }
'''

VISIBILITY = r'''
#define PD(...) ((void)0)
#define SBW_REPORT 1
typedef struct { uint64_t slots, total, output, count, publish, ticks; } vis_gpu_job;
typedef struct { vis_gpu_job gpu; uint64_t destination; void *total_buffer, *output_buffer; } vis_pending_job;
static vis_pending_job *g_vis_pending;
static int g_vis_pending_n, g_vis_pending_cap, g_nvs, g_vis_cur, g_pass_readonly = 1;
static struct { void *buf; int slot; } g_vs[1];
static id g_enc;
static MockBuffer *scratch;
static unsigned reports;
static uint64_t gpu_ticks(void) { return 17; }
static id<MTLBuffer> vis_total(void) { return (id)scratch; }
static uint64_t vis_total_gpu(void) { return (uintptr_t)scratch.contents; }
static int vis_batch_on(void) { return 1; }
static int vis_destination_is_attachment(uint64_t address) { (void)address; return 0; }
static void vis_take_input(void *buffer) { CFRelease(buffer); }
static size_t arena_alloc(size_t size, id<MTLBuffer> *buffer) {
    assert(size <= scratch.length); *buffer = (id)scratch; return 0;
}
static void pend_add(uint64_t address, uint64_t size, int kind) {
    assert(address && size == 16 && kind == 4); reports++;
}
static void sbw_note(uint64_t address, uint64_t size, int kind) {
    assert(address && size == 16 && kind == SBW_REPORT);
}
static void rp_flush(void) { abort(); }
static void vis_flush_pending(void) { abort(); }
'''

VISIBILITY_TEST = r'''
int main(void) { @autoreleasepool {
    const size_t quantum = 1u << 20, size = 8 * quantum;
    void *storage = NULL; assert(!posix_memalign(&storage, 16384, size));
    MockBuffer *owner = [MockBuffer new]; owner.contents = storage; owner.length = size;
    uintptr_t lo = (uintptr_t)storage;
    g_pools[0] = (pool_ent){lo, lo + size, lo, (id)owner, lo, storage, 0};
    uint64_t words[8] = {0};
    scratch = [MockBuffer new]; scratch.contents = words; scratch.length = sizeof words;
    g_enc = [NSObject new]; retired = [NSMutableArray array];
    g_fence = 5; g_done = 5; g_cur_frame = 10;
    __weak id alive;
    @autoreleasepool {
        vis_resolve(lo + quantum - 8);
        assert(g_vis_pending_n == 1 && reports == 1 && !full_bindings && !g_pools[0].res);
        vis_pending_job *job = &g_vis_pending[0];
        id<MTLBuffer> output = (__bridge id)job->output_buffer;
        assert(output.length == 2 * quantum && output.contents == storage);
        assert(job->gpu.output == lo + quantum - 8 && job->gpu.publish == 1 && job->gpu.ticks == 17);
        alive = output;
        g_cur_frame = 160; pool_ranges_trim(); assert(!retired.count);
        g_done = 6; g_cur_frame = 190; pool_ranges_trim(); assert(retired.count == 1);
    }
    [retired removeAllObjects]; assert(alive != nil); // The pending report still owns its destination.
    uint64_t value[2] = {93, 17};
    memcpy((void *)(uintptr_t)g_vis_pending[0].gpu.output, value, sizeof value);
    assert(!memcmp((uint8_t *)storage + quantum - 8, value, sizeof value));
    CFRelease(g_vis_pending[0].output_buffer); CFRelease(g_vis_pending[0].total_buffer);
    g_vis_pending_n = 0; free(g_vis_pending); g_vis_pending = NULL;
    assert(alive == nil);
    objc_setAssociatedObject(owner, &g_pool_ranges_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    free(storage);
    puts("QUERY_RANGE OK: 16-byte cross-window write, coherent alias, in-flight and pending ownership");
} return 0; }
'''

UPLOAD_TEST = r'''
int main(void) { @autoreleasepool {
    const size_t page = (size_t)getpagesize(), size = 8u << 20;
    void *storage = NULL;
    assert(posix_memalign(&storage, page, size) == 0);
    MockBuffer *owner = [MockBuffer new]; owner.contents = storage; owner.length = size;
    uintptr_t lo = (uintptr_t)storage;
    g_pools[0] = (pool_ent){lo, lo + size, lo, (id)owner, lo, storage, 0};
    NSMutableArray *batch = [NSMutableArray array];
    for (int ranges = 0; ranges <= 1; ranges++) {
        g_pool_ranges = ranges;
        for (unsigned i = 0; i < 32; i++) {
            __weak id alive;
            @autoreleasepool {
            uint64_t off; int scoped;
            uint64_t address = lo + i * page + page - 64;
            id<MTLBuffer> source = mtl_upload_source(address, 128, &off, &scoped);
            assert(source && scoped && source != (id)owner);
            assert(source.length == 2 * page && off == page - 64);
            ((unsigned char *)storage)[address - lo] = (unsigned char)i;
            assert(((unsigned char *)source.contents)[off] == (unsigned char)i);
            assert(!objc_getAssociatedObject(owner, &g_pool_ranges_key));
            alive = source;
            [batch addObject:source]; source = nil;
            assert(alive != nil);
            }
            [batch removeAllObjects]; assert(alive == nil);
        }
    }
    uint64_t off; int scoped;
    g_pools[0].res = 1;
    assert(mtl_upload_source(lo + 17, 128, &off, &scoped) == (id)owner && !scoped && off == 17);
    g_pools[0].res = 0; reject = 1;
    assert(!mtl_upload_source(lo, page, &off, &scoped) && !scoped);
    assert(!mtl_upload_source(lo + size - 64, 128, &off, &scoped));
    assert(!mtl_upload_source(UINT64_MAX - 1, 4, &off, &scoped));
    assert(!mtl_upload_source(lo, 0, &off, &scoped));
    assert(!full_bindings && !registered);
    free(storage);
    puts("UPLOAD_SOURCE OK: bounded aliases, coherent bytes, completion ownership, no persistent windows");
} return 0; }
'''

GATES = r'''
static NSMutableArray *committed;
@interface MockEvent : NSObject
@property uint64_t value;
@end
@implementation MockEvent @end
@interface MockCommandBuffer : NSObject
@property(strong) NSMutableArray *operations, *handlers;
@property NSUInteger position;
- (void)encodeWaitForEvent:(id)event value:(uint64_t)value;
- (void)encodeSignalEvent:(id)event value:(uint64_t)value;
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler;
- (void)commit;
- (void)work:(dispatch_block_t)work;
@end
@implementation MockCommandBuffer
- (instancetype)init {
    if ((self = [super init])) { _operations = [NSMutableArray array]; _handlers = [NSMutableArray array]; }
    return self;
}
- (void)encodeWaitForEvent:(id)event value:(uint64_t)value { [_operations addObject:@[@"wait", event, @(value)]]; }
- (void)encodeSignalEvent:(id)event value:(uint64_t)value { [_operations addObject:@[@"signal", event, @(value)]]; }
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler { [_handlers addObject:[handler copy]]; }
- (void)commit { [committed addObject:self]; }
- (void)work:(dispatch_block_t)work { [_operations addObject:@[@"work", [work copy]]]; }
@end
@interface MockQueue : NSObject
- (id<MTLCommandBuffer>)commandBuffer;
@end
@implementation MockQueue
- (id<MTLCommandBuffer>)commandBuffer { return (id)[MockCommandBuffer new]; }
@end
@interface MockDevice : NSObject
- (id<MTLEvent>)newEvent;
@end
@implementation MockDevice
- (id<MTLEvent>)newEvent { return (id)[MockEvent new]; }
@end
@interface NvCommitOptions : NSObject
@property(strong) NSMutableArray<MTLCommandBufferHandler> *handlers;
@end
@implementation NvCommitOptions @end
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_replay_q;
static id<MTLEvent> g_replay_complete;
static int g_pool_ranges = 1;
static char g_replay_gate_key;
static uint64_t g_replay_sequence;
static pthread_mutex_t g_commit_mu = PTHREAD_MUTEX_INITIALIZER;
static dispatch_group_t g_feedback_tail;
static dispatch_queue_t g_feedback_q;
static int gpu_errors_on(void) { return 0; }
static void log_gpu_encoders(id cb) { (void)cb; }
static void discard_end_for(id cb) { (void)cb; }
'''

GATE_TEST = r'''
int main(void) { @autoreleasepool {
    committed = [NSMutableArray array];
    g_dev = (id)[MockDevice new]; g_replay_q = (id)[MockQueue new];
    g_replay_complete = [g_dev newEvent];
    g_feedback_q = dispatch_queue_create("test.feedback", DISPATCH_QUEUE_SERIAL);
    __block int value = 0;
    // Main is created first, but the upload must execute before it. A sequence
    // assigned at creation would deadlock here. The last reader tries to run first.
    MockCommandBuffer *main = (id)command_buffer(g_replay_q);
    MockCommandBuffer *upload = (id)command_buffer(g_replay_q);
    [upload work:^{ assert(value == 0); value = 10; }];
    [main work:^{ assert(value == 10); value = 20; }];
    replay_commit((id)upload, nil, nil, 0);
    replay_commit((id)main, nil, nil, 0);
    MockCommandBuffer *reader = (id)command_buffer(g_replay_q);
    [reader work:^{ assert(value == 20); value = 30; }];
    replay_commit((id)reader, nil, nil, 0);
    assert(committed.count == 6);
    NSUInteger completed = 0;
    while (completed != committed.count) {
        BOOL progress = NO;
        for (MockCommandBuffer *cb in committed.reverseObjectEnumerator) {
            if (cb.position >= cb.operations.count) continue;
            NSArray *operation = cb.operations[cb.position];
            NSString *kind = operation[0];
            if ([kind isEqual:@"wait"] && ((MockEvent *)operation[1]).value < [operation[2] unsignedLongLongValue]) continue;
            if ([kind isEqual:@"signal"]) ((MockEvent *)operation[1]).value = [operation[2] unsignedLongLongValue];
            if ([kind isEqual:@"work"]) ((dispatch_block_t)operation[1])();
            cb.position++; progress = YES;
            if (cb.position == cb.operations.count) {
                for (MTLCommandBufferHandler handler in cb.handlers) handler((id)cb);
                completed++;
            }
        }
        assert(progress);
    }
    assert(value == 30 && ((MockEvent *)g_replay_complete).value == 3);
    assert(dispatch_group_wait(g_feedback_tail, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
    id<MTLCommandQueue> other = (id)[MockQueue new];
    assert(((MockCommandBuffer *)command_buffer(other)).operations.count == 0);
    puts("REPLAY_GATES OK: reversed creation/commit order, adversarial scheduling, unrelated queue");
} return 0; }
'''

with tempfile.TemporaryDirectory(prefix="nvn-pool-ranges-") as directory:
    root = Path(directory)
    pool = (COMMON + POOL + function("pool_range_use") + function("pool_range_keep")
            + function("mtl_gpu_range") + function("pool_ranges_trim"))
    cases = {
        "ranges": pool + function("pool_ranges_retire") + POOL_TEST,
        "lifetime": pool + LIFETIME_TEST,
        "bindings": pool + BINDINGS + function("bind_vbufs") + function("shader_use") + BINDINGS_TEST,
        "uploads": COMMON + POOL + UPLOAD + function("mtl_upload_source") + UPLOAD_TEST,
        "cpu_hash": pool + UPLOAD + CPU_HASH + function("mtl_cpu_addr")
            + function("keys_hash", (ROOT / "nvn/hle_nvn.c").read_text()) + CPU_HASH_TEST,
        "visibility": pool + VISIBILITY
            + function("vis_resolve", (ROOT / "nvn/nvn_visibility.inc").read_text()) + VISIBILITY_TEST,
        "gates": COMMON + GATES + function("command_buffer") + function("replay_commit") + GATE_TEST,
    }
    for name, source in cases.items():
        path = root / (name + ".m")
        path.write_text(source)
        executable = root / name
        subprocess.run(["xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra",
                        "-g", "-fsanitize=address,undefined", "-Wno-unused-function", "-Wno-unused-variable",
                        "-I", str(ROOT / "nvn"), "-framework", "Foundation", "-framework", "Metal",
                        str(path), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
