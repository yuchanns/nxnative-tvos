"""Check production fence narrowing with CPU-only Objective-C Metal mocks."""

from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "nvn/nvn_mtl.m").read_text()


def function(name):
    match = re.search(r"^(?:static )?(?:inline )?[^\n]+\b" + name + r"\([^\n]*\) \{", SOURCE, re.M)
    assert match, name
    start = SOURCE.index("{", match.start())
    depth = 0
    for end in range(start, len(SOURCE)):
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        if not depth:
            return SOURCE[match.start():end + 1] + "\n"
    raise AssertionError(name)


PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <assert.h>
#include <stdatomic.h>
#include "nvn_refl.h"
#define MLOG(...) ((void)0)
#define MDIE(...) abort()
static uint64_t g_sub_serial;
static int g_pg;
static void pg_draw(void) {}
static void shader_use_reset(void) {}
static int encoder_fences_on(void) { return 1; }
static id<MTLBuffer> gpu_buffer_required(uint64_t gpu, NSUInteger length, NSUInteger *offset) {
    (void)gpu; (void)length; *offset = 0; return nil;
}
@interface MockDevice : NSObject
- (id)newFence;
@end
@implementation MockDevice
- (id)newFence { return [NSObject new]; }
@end
static MockDevice *g_dev;
@interface MockEncoder : NSObject
@property int kind;
@property NSUInteger waits, updates, draws;
@property BOOL ended;
@property(strong) NSMutableArray *events;
@property(strong) NSMutableSet *updatedFences;
@end
@implementation MockEncoder
- (instancetype)init { if ((self = [super init])) { _events = [NSMutableArray array]; _updatedFences = [NSMutableSet set]; } return self; }
- (BOOL)conformsToProtocol:(Protocol *)p {
    if (protocol_isEqual(p, @protocol(MTLRenderCommandEncoder))) return _kind == 0;
    if (protocol_isEqual(p, @protocol(MTLComputeCommandEncoder))) return _kind == 1;
    return _kind == 2 && protocol_isEqual(p, @protocol(MTLBlitCommandEncoder));
}
- (void)waitForFence:(id)f beforeStages:(MTLRenderStages)s {
    assert(f && !_ended && !_updates); _waits |= s; [_events addObject:@[@"wait", @(s), f]];
}
- (void)waitForFence:(id)f { [self waitForFence:f beforeStages:3]; }
- (void)updateFence:(id)f afterStages:(MTLRenderStages)s {
    assert(f && !_ended && ![_updatedFences containsObject:f]);
    [_updatedFences addObject:f]; _updates |= s; [_events addObject:@[@"update", @(s), f]];
}
- (void)updateFence:(id)f { [self updateFence:f afterStages:3]; }
- (void)endEncoding { _ended = YES; [_events addObject:@[@"end"]]; }
- (void)recordDraw { assert(!_ended && !_updates); _draws++; [_events addObject:@[@"draw", @(_waits)]]; }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)v vertexCount:(NSUInteger)n { [self recordDraw]; }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)v vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b { [self recordDraw]; }
- (void)drawPrimitives:(MTLPrimitiveType)p indirectBuffer:(id)b indirectBufferOffset:(NSUInteger)o { [self recordDraw]; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n indexType:(MTLIndexType)t indexBuffer:(id)b indexBufferOffset:(NSUInteger)o instanceCount:(NSUInteger)i baseVertex:(NSInteger)v baseInstance:(NSUInteger)bi { [self recordDraw]; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexType:(MTLIndexType)t indexBuffer:(id)b indexBufferOffset:(NSUInteger)o indirectBuffer:(id)ib indirectBufferOffset:(NSUInteger)io { [self recordDraw]; }
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {}
@end
@interface NvShader : NSObject {
@public int memory_independent, generated, resources_readonly, vertex_buffers_only, stage; const char *builtin;
} @end
@implementation NvShader @end
@interface NvProg : NSObject { @public NvShader *sh[6]; } @end
@implementation NvProg @end
static struct { int tf_on, rast_discard; } S;
@interface MockTexture : NSObject
@property MTLStorageMode storageMode;
@property(strong) id buffer, heap, parentTexture;
@end
@implementation MockTexture
- (instancetype)init { if ((self = [super init])) _storageMode = MTLStorageModePrivate; return self; }
@end
@interface MockAttachment : NSObject
@property(strong) id texture, resolveTexture;
@end
@implementation MockAttachment @end
@interface MockDescriptor : NSObject
@property(strong) NSArray *colorAttachments;
@property(strong) MockAttachment *depthAttachment, *stencilAttachment;
@end
@implementation MockDescriptor
- (instancetype)init {
    if ((self = [super init])) {
        NSMutableArray *colors = [NSMutableArray array];
        for (int i = 0; i < 8; i++) [colors addObject:[MockAttachment new]];
        _colorAttachments = colors; _depthAttachment = [MockAttachment new]; _stencilAttachment = [MockAttachment new];
    }
    return self;
}
@end
'''

TEST = r'''
static MockEncoder *begin(id cb, int deferred, int kind) {
    MockEncoder *e = [MockEncoder new]; e.kind = kind;
    return encoder_begin_scoped(cb, (id)e, deferred);
}
static void reflection_tests(void) {
    NSMutableDictionary *j = [@{@"route": @"B", @"stage": @"vs", @"msl_entry_kind": @"vertex",
        @"bindings": @[], @"vertex_inputs": @[], @"c1": @""} mutableCopy];
    nvn_refl rf; char err[160];
    assert(!nvn_refl_from_json(j, &rf, err, sizeof err));
    assert(vertex_memory_independent(j, &rf, 0));
    assert(!vertex_memory_independent(j, &rf, 1));
    [j removeObjectForKey:@"vertex_inputs"];
    assert(!nvn_refl_from_json(j, &rf, err, sizeof err));
    assert(!vertex_memory_independent(j, &rf, 0));
    j[@"vertex_inputs"] = @[@{@"location": @0, @"msl_type": @"float4"}];
    assert(!nvn_refl_from_json(j, &rf, err, sizeof err));
    assert(!vertex_memory_independent(j, &rf, 0));
    j[@"vertex_inputs"] = @[];
    assert(!nvn_refl_from_json(j, &rf, err, sizeof err));
    rf.nb = 1; assert(!vertex_memory_independent(j, &rf, 0)); rf.nb = 0;
    rf.has_c1 = 1; assert(!vertex_memory_independent(j, &rf, 0)); rf.has_c1 = 0;
    rf.drv_read[0] = 1; assert(!vertex_memory_independent(j, &rf, 0)); rf.drv_read[0] = 0;
    rf.dyn = 1; assert(!vertex_memory_independent(j, &rf, 0)); rf.dyn = 0;
    rf.route_b = 0; assert(!vertex_memory_independent(j, &rf, 0)); rf.route_b = 1;
    [j removeObjectForKey:@"msl_entry_kind"]; assert(!vertex_memory_independent(j, &rf, 0));
    j[@"msl_entry_kind"] = @"vertex"; j[@"bindings"] = @{};
    assert(!vertex_memory_independent(j, &rf, 0));
}
static void eligibility_tests(void) {
    NvProg *pr = [NvProg new]; pr->sh[0] = [NvShader new]; pr->sh[0]->memory_independent = 1;
    p_draw dw = {.prim = 5, .count = 4}; p_draw_idx di = {0};
    assert(draw_vertex_independent(pr, &dw, NULL, 0));
    assert(!draw_vertex_independent(pr, NULL, &di, 0));
    assert(!draw_vertex_independent(pr, &dw, &di, 0));
    assert(!draw_vertex_independent(pr, &dw, NULL, 1));
    dw.ind = 1; assert(!draw_vertex_independent(pr, &dw, NULL, 0)); dw.ind = 0;
    dw.mdi_cnt = 1; assert(!draw_vertex_independent(pr, &dw, NULL, 0)); dw.mdi_cnt = 0;
    for (int p = 6; p <= 7; p++) { dw.prim = p; assert(!draw_vertex_independent(pr, &dw, NULL, 0)); } dw.prim = 5;
    S.tf_on = 1; assert(!draw_vertex_independent(pr, &dw, NULL, 0)); S.tf_on = 0;
    S.rast_discard = 1; assert(!draw_vertex_independent(pr, &dw, NULL, 0)); S.rast_discard = 0;
    pr->sh[0]->generated = 1; assert(!draw_vertex_independent(pr, &dw, NULL, 0)); pr->sh[0]->generated = 0;
    pr->sh[0]->builtin = "generated"; assert(!draw_vertex_independent(pr, &dw, NULL, 0)); pr->sh[0]->builtin = NULL;
    for (int s = 2; s < 5; s++) { pr->sh[s] = [NvShader new]; assert(!draw_vertex_independent(pr, &dw, NULL, 0)); pr->sh[s] = nil; }
    pr->sh[0]->memory_independent = 0; assert(!draw_vertex_independent(pr, &dw, NULL, 0));
}
static void buffer_reflection_tests(void) {
    NSMutableDictionary *j = [@{@"route": @"B", @"stage": @"vs", @"msl_entry_kind": @"vertex",
        @"drv_reads": @{}, @"bindings": @[]} mutableCopy];
    assert(shader_resources_readonly(j, 0) && shader_resources_readonly(j, 1));
    for (NSString *kind in @[@"cbuf", @"c1", @"support"]) {
        j[@"bindings"] = @[@{@"kind": kind, @"msl_type": @"constant float4*"}];
        assert(shader_resources_readonly(j, 0) && shader_resources_readonly(j, 1));
    }
    j[@"bindings"] = @[@{@"kind": @"sbuf", @"msl_type": @"const device float*", @"rw": @NO}];
    assert(shader_resources_readonly(j, 1));
    j[@"bindings"] = @[@{@"kind": @"sbuf", @"msl_type": @"const device float*"}];
    assert(!shader_resources_readonly(j, 0));
    j[@"bindings"] = @[@{@"kind": @"sbuf", @"msl_type": @"device float*", @"rw": @NO}];
    assert(!shader_resources_readonly(j, 0));
    for (NSString *kind in @[@"tex", @"tbuf"]) {
        j[@"bindings"] = @[@{@"kind": kind, @"msl_type": @"texture2d<float, access::read>"}];
        assert(shader_resources_readonly(j, 0) && !shader_resources_readonly(j, 1));
        j[@"bindings"] = @[@{@"kind": kind, @"msl_type": @"texture2d<float, access::write>"}];
        assert(!shader_resources_readonly(j, 0));
    }
    for (NSString *kind in @[@"samp", @"samp_tex", @"samp_id"]) {
        j[@"bindings"] = @[@{@"kind": kind, @"msl_type": @"sampler"}];
        assert(shader_resources_readonly(j, 0) && !shader_resources_readonly(j, 1));
    }
    for (NSString *kind in @[@"img", @"sbuf_ubo", @"tex_ubo", @"drvcb", @"future_kind"]) {
        j[@"bindings"] = @[@{@"kind": kind, @"msl_type": @"constant float4*"}];
        assert(!shader_resources_readonly(j, 0));
    }
    j[@"bindings"] = @[@{@"kind": @"cbuf", @"msl_type": @"constant float4*", @"rw": @YES}];
    assert(!shader_resources_readonly(j, 0));
    j[@"bindings"] = @[@1]; assert(!shader_resources_readonly(j, 0));
    j[@"bindings"] = @[]; j[@"drv_reads"] = @{@"0": @YES}; assert(!shader_resources_readonly(j, 0));
    [j removeObjectForKey:@"drv_reads"]; assert(!shader_resources_readonly(j, 0)); j[@"drv_reads"] = @{};
    j[@"stage"] = @"ps"; j[@"msl_entry_kind"] = @"fragment";
    assert(shader_resources_readonly(j, 0) && !shader_resources_readonly(j, 1));
    j[@"stage"] = @"cs"; assert(!shader_resources_readonly(j, 0));
}
static void buffer_eligibility_tests(void) {
    NvProg *program = [NvProg new]; program->sh[0] = [NvShader new];
    program->sh[0]->resources_readonly = program->sh[0]->vertex_buffers_only = 1;
    program->sh[1] = [NvShader new]; program->sh[1]->stage = 1; program->sh[1]->resources_readonly = 1;
    p_draw arrays = {.prim = 4, .count = 3}; p_draw_idx indexed = {.prim = 4, .itype = 1, .count = 3};
    assert(draw_resources_readonly(program, 0));
    assert(draw_vertex_buffers_only(program, &arrays, NULL, 0));
    assert(draw_vertex_buffers_only(program, NULL, &indexed, 0));
    indexed.itype = 0; assert(!draw_vertex_buffers_only(program, NULL, &indexed, 0)); indexed.itype = 2;
    assert(draw_vertex_buffers_only(program, NULL, &indexed, 0));
    indexed.ind = 1; assert(!draw_vertex_buffers_only(program, NULL, &indexed, 0)); indexed.ind = 0;
    arrays.mdi_cnt = 1; assert(!draw_vertex_buffers_only(program, &arrays, NULL, 0)); arrays.mdi_cnt = 0;
    arrays.prim = 6; assert(!draw_vertex_buffers_only(program, &arrays, NULL, 0)); arrays.prim = 4;
    program->sh[0]->vertex_buffers_only = 0; assert(!draw_vertex_buffers_only(program, &arrays, NULL, 0));
    assert(draw_resources_readonly(program, 0)); program->sh[0]->vertex_buffers_only = 1;
    program->sh[1]->resources_readonly = 0; assert(!draw_resources_readonly(program, 0)); program->sh[1]->resources_readonly = 1;
    program->sh[0]->generated = 1; assert(!draw_resources_readonly(program, 0)); program->sh[0]->generated = 0;
    S.tf_on = 1; assert(!draw_resources_readonly(program, 0)); S.tf_on = 0;
    S.rast_discard = 1; assert(!draw_resources_readonly(program, 0)); S.rast_discard = 0;
    for (int stage = 2; stage < 6; stage++) {
        program->sh[stage] = [NvShader new]; assert(!draw_resources_readonly(program, 0)); program->sh[stage] = nil;
    }
    assert(!draw_resources_readonly(program, 1));
}
static void buffer_fence_tests(void) {
    MockDescriptor *descriptor = [MockDescriptor new];
    assert(pass_separate_from_buffers((id)descriptor));
    for (int slot = 0; slot < 10; slot++) {
        MockAttachment *attachment = slot < 8 ? descriptor.colorAttachments[slot] :
            slot == 8 ? descriptor.depthAttachment : descriptor.stencilAttachment;
        MockTexture *texture = [MockTexture new], *parent = [MockTexture new]; attachment.texture = texture;
        texture.parentTexture = parent; assert(pass_separate_from_buffers((id)descriptor));
        parent.buffer = [NSObject new]; assert(!pass_separate_from_buffers((id)descriptor)); parent.buffer = nil;
        parent.heap = [NSObject new]; assert(!pass_separate_from_buffers((id)descriptor)); parent.heap = nil;
        parent.storageMode = MTLStorageModeShared; assert(!pass_separate_from_buffers((id)descriptor)); parent.storageMode = MTLStorageModePrivate;
        parent.parentTexture = texture; assert(!pass_separate_from_buffers((id)descriptor)); parent.parentTexture = nil;
        MockTexture *resolve = [MockTexture new]; resolve.heap = [NSObject new]; attachment.resolveTexture = resolve;
        assert(!pass_separate_from_buffers((id)descriptor)); attachment.resolveTexture = nil; attachment.texture = nil;
    }
    id cb = [NSObject new]; MockEncoder *producer = begin(cb, 0, 1); enc_end((id)producer);
    NvEncoderSync *sync = objc_getAssociatedObject(cb, &g_sync_key);
    id all_fence = sync->fence, buffer_fence = sync->buffer_fence;
    assert(all_fence != buffer_fence && producer.updatedFences.count == 2);
    for (int i = 0; i < 2; i++) {
        MockEncoder *read = begin(cb, 1, 0); encoder_prepare_render((id)read, (id)descriptor, 1);
        encoder_prepare_draw((id)read, 1, 1); enc_r_draw_indexed((id)read, 0, 3, 1, 0, 6, 1, 0, 0);
        assert([read.events[0][2] isEqual:all_fence] && [read.events[0][1] intValue] == MTLRenderStageFragment);
        assert([read.events[1][2] isEqual:buffer_fence] && [read.events[1][1] intValue] == MTLRenderStageVertex);
        encoder_prepare_draw((id)read, 1, 1); enc_r_draw((id)read, 0, 0, 3);
        assert(read.events.count == 4); enc_end((id)read);
        assert(read.updatedFences.count == 1 && [read.updatedFences containsObject:all_fence]);
    }
    MockEncoder *mixed = begin(cb, 1, 0); encoder_prepare_render((id)mixed, (id)descriptor, 1);
    encoder_prepare_draw((id)mixed, 1, 1); enc_r_draw((id)mixed, 0, 0, 3);
    encoder_prepare_draw((id)mixed, 1, 0); enc_r_draw((id)mixed, 0, 0, 3);
    assert([mixed.events[3][2] isEqual:all_fence] && [mixed.events[3][1] intValue] == MTLRenderStageVertex);
    enc_end((id)mixed); assert(mixed.updatedFences.count == 1);
    MockEncoder *unknown = begin(cb, 1, 0); encoder_prepare_render((id)unknown, (id)descriptor, 1);
    encoder_prepare_draw((id)unknown, 1, 1); enc_r_draw((id)unknown, 0, 0, 3);
    enc_r_draw((id)unknown, 0, 0, 3); enc_end((id)unknown);
    assert(unknown.updatedFences.count == 2);
    MockEncoder *query = begin(cb, 1, 0); encoder_prepare_render((id)query, (id)descriptor, 1);
    enc_r_visibility((id)query, MTLVisibilityResultModeBoolean, 0);
    encoder_prepare_draw((id)query, 1, 1); enc_r_draw((id)query, 0, 0, 3); enc_end((id)query);
    assert(query.updatedFences.count == 2);
    MockEncoder *helper = begin(cb, 0, 0); encoder_prepare_render((id)helper, (id)descriptor, 0);
    encoder_prepare_draw((id)helper, 1, 1); enc_r_draw((id)helper, 0, 0, 3); enc_end((id)helper);
    assert(helper.updatedFences.count == 2);
    MockEncoder *blit = begin(cb, 0, 2); enc_end((id)blit); assert(blit.updatedFences.count == 2);
    vertex_buffers_apply(); uint64_t generation = mtl_vertex_buffers_request(0); assert(g_vertex_buffers);
    vertex_buffers_apply(); assert(!g_vertex_buffers);
    assert([mtl_vertex_buffers_status()[@"applied_generation"] unsignedLongLongValue] == generation);
    cb = [NSObject new]; producer = begin(cb, 0, 1); enc_end((id)producer); assert(producer.updatedFences.count == 1);
    MockEncoder *off = begin(cb, 1, 0); encoder_prepare_render((id)off, (id)descriptor, 1);
    encoder_prepare_draw((id)off, 1, 1); enc_r_draw((id)off, 0, 0, 3); assert(off.waits == 3); enc_end((id)off);
    assert(!mtl_vertex_buffers_request(2)); mtl_vertex_buffers_request(1); vertex_buffers_apply(); assert(g_vertex_buffers);
    cb = [NSObject new]; MockEncoder *first = begin(cb, 1, 0); encoder_prepare_render((id)first, (id)descriptor, 1);
    encoder_prepare_draw((id)first, 1, 1); enc_r_draw((id)first, 0, 0, 3); assert(!first.waits); enc_end((id)first);
    assert(first.updatedFences.count == 1);
}
int main(void) { @autoreleasepool {
    g_dev = [MockDevice new];
    reflection_tests(); eligibility_tests();
    id cb = [NSObject new];
    MockEncoder *first = begin(cb, 1, 0); assert(!first.waits); enc_end((id)first);
    assert(first.updates == 3 && first.ended);
    MockEncoder *pure = begin(cb, 1, 0); assert(pure.waits == MTLRenderStageFragment);
    enc_r_draw_independent((id)pure, MTLPrimitiveTypeTriangleStrip, 0, 4, 1, 0);
    assert(pure.waits == MTLRenderStageFragment); enc_end((id)pure);
    assert(atomic_load(&g_vertex_overlap_skipped) == 1);
    for (int wrapper = 0; wrapper < 5; wrapper++) {
        MockEncoder *mixed = begin(cb, 1, 0);
        enc_r_draw_independent((id)mixed, MTLPrimitiveTypeTriangleStrip, 0, 4, 1, 0);
        switch (wrapper) {
        case 0: enc_r_draw((id)mixed, 0, 0, 3); break;
        case 1: enc_r_draw_instanced((id)mixed, 0, 0, 3, 2, 0); break;
        case 2: enc_r_draw_indirect((id)mixed, 0, 0); break;
        case 3: enc_r_draw_indexed((id)mixed, 0, 3, 0, 0, 6, 1, 0, 0); break;
        case 4: enc_r_draw_indexed_indirect((id)mixed, 0, 0, 0, 6, 0); break;
        }
        assert(mixed.draws == 2 && mixed.waits == 3);
        assert([mixed.events[1][0] isEqual:@"draw"] && [mixed.events[1][1] intValue] == 2);
        assert([mixed.events[2][0] isEqual:@"wait"] && [mixed.events[2][1] intValue] == 1);
        assert([mixed.events[3][0] isEqual:@"draw"] && [mixed.events[3][1] intValue] == 3);
        enc_end((id)mixed);
    }
    assert(atomic_load(&g_vertex_overlap_skipped) == 1 && atomic_load(&g_vertex_overlap_fallbacks) == 5);
    MockEncoder *empty = begin(cb, 1, 0); enc_end((id)empty);
    assert(atomic_load(&g_vertex_overlap_skipped) == 1);
    for (int kind = 0; kind < 3; kind++) {
        MockEncoder *full = [MockEncoder new]; full.kind = kind; encoder_begin(cb, (id)full);
        assert(full.waits == 3); enc_end((id)full); assert(full.updates == 3);
    }
    id separate = [NSObject new]; MockEncoder *new_cb = begin(separate, 1, 0); assert(!new_cb.waits); enc_end((id)new_cb);
    vertex_overlap_apply(); assert(g_vertex_overlap == 1);
    uint64_t generation = mtl_vertex_overlap_request(0);
    assert(generation && g_vertex_overlap == 1);
    assert([mtl_vertex_overlap_status()[@"applied_generation"] unsignedLongLongValue] != generation);
    vertex_overlap_apply(); assert(!g_vertex_overlap);
    assert([mtl_vertex_overlap_status()[@"applied_generation"] unsignedLongLongValue] == generation);
    MockEncoder *off = begin(cb, g_vertex_overlap, 0); assert(off.waits == 3); enc_end((id)off);
    assert(!mtl_vertex_overlap_request(2));
    mtl_vertex_overlap_request(1); vertex_overlap_apply(); assert(g_vertex_overlap);
    depth_attachments_apply(); assert(g_depth_attachments);
    uint64_t depth_generation = mtl_depth_attachments_request(0);
    assert(depth_generation && g_depth_attachments);
    assert([mtl_depth_attachments_status()[@"applied_generation"] unsignedLongLongValue] != depth_generation);
    depth_attachments_apply(); assert(!g_depth_attachments && g_vertex_overlap);
    assert([mtl_depth_attachments_status()[@"applied_generation"] unsignedLongLongValue] == depth_generation);
    assert(!mtl_depth_attachments_request(-1) && !mtl_depth_attachments_request(2));
    mtl_depth_attachments_request(0);
    depth_generation = mtl_depth_attachments_request(1);
    assert(!g_depth_attachments);
    depth_attachments_apply(); assert(g_depth_attachments && g_vertex_overlap);
    assert([mtl_depth_attachments_status()[@"applied_generation"] unsignedLongLongValue] == depth_generation);
    buffer_reflection_tests(); buffer_eligibility_tests(); buffer_fence_tests();
    puts("VERTEX_OVERLAP OK: read-only proof, buffer/heap aliases, mixed draws, visibility writes, fence identities, CB isolation, switches");
} return 0; }
'''

variables = SOURCE[SOURCE.index("static _Atomic uint64_t g_vertex_overlap_request"):SOURCE.index("static uint64_t render_option_request(")]
sync_class = SOURCE[SOURCE.index("@interface NvEncoderSync"):SOURCE.index("static void shader_use_reset(void);", SOURCE.index("@interface NvEncoderSync"))]
objects = (ROOT / "nvn/nvn_obj.h").read_text()
draw_types = "\n".join(line for line in objects.splitlines() if "} p_draw;" in line or "} p_draw_idx;" in line)
names = ["render_option_request", "render_option_status", "render_option_apply",
         "mtl_vertex_overlap_request", "mtl_vertex_overlap_status", "vertex_overlap_apply",
         "mtl_depth_attachments_request", "mtl_depth_attachments_status", "depth_attachments_apply",
         "mtl_vertex_buffers_request", "mtl_vertex_buffers_status", "vertex_buffers_apply",
         "encoder_begin_scoped", "encoder_begin", "encoder_prepare_draw", "encoder_consume_draw", "encoder_require_vertex", "enc_end",
         "texture_separate_from_buffers", "pass_separate_from_buffers", "encoder_prepare_render", "enc_r_visibility",
         "enc_r_draw", "enc_r_draw_instanced", "enc_r_draw_independent", "enc_r_draw_indirect",
         "enc_r_draw_indexed", "enc_r_draw_indexed_indirect", "vertex_memory_independent", "draw_vertex_independent",
         "shader_resources_readonly", "draw_resources_readonly", "draw_vertex_buffers_only"]
with tempfile.TemporaryDirectory(prefix="nvn-vertex-overlap-") as directory:
    source = Path(directory) / "test.m"
    binary = Path(directory) / "test"
    source.write_text(PREFIX + draw_types + "\n" + variables + sync_class +
                      "#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)\n" +
                      "".join(function(n) for n in names) + "#pragma clang attribute pop\n" + TEST)
    subprocess.run(["xcrun", "clang", "-fobjc-arc", "-fblocks", "-Werror", "-Wno-incompatible-pointer-types",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(ROOT / "nvn"), str(source), str(ROOT / "nvn/nvn_refl.m"),
                    "-framework", "Foundation", "-framework", "Metal", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
