// GPU readback across blit, compute and render encoders, including indirect APIs.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#include "../nvn/hle_nvn.c"
#include "gen/tf_shader.h"
#include "gen/geometry_shader.h"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

// Production shader caches retain identities used by pipeline keys. Fixtures
// need the same lifetime; releasing one test's shaders lets later allocations
// reuse those addresses while their compiled pipelines remain cached.
static NvProg *fixture_program(void) {
    static NSMutableArray<NvProg *> *programs;
    if (!programs) programs = [NSMutableArray array];
    NvProg *program = [NvProg new]; [programs addObject:program]; return program;
}

static const void *lease_base;
static size_t lease_bytes;
static unsigned leases, lease_releases;
int dm_guest_storage_lease_acquire(const void *base, size_t bytes, void **out) {
    if (!lease_base || base != lease_base) return 0;
    if (bytes != lease_bytes) return -1;
    leases++; *out = &leases; return 1;
}
void dm_guest_storage_lease_release(void *lease) {
    EXPECT("storage lease token", (uintptr_t)&leases, (uintptr_t)lease);
    EXPECT("storage lease released once", 1, leases);
    leases--; lease_releases++;
}

static void storage_lifetime(void) {
    size_t page = (size_t)getpagesize(); void *memory;
    if (posix_memalign(&memory, page, page)) abort();
    lease_base = memory; lease_bytes = page;
    @autoreleasepool {
        id<MTLBuffer> buffer = guest_pool_buffer((uintptr_t)memory, page);
        EXPECT("leased Metal buffer created", 1, buffer != nil);
        EXPECT("Metal buffer holds storage", 1, leases);
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [buffer newTextureWithDescriptor:descriptor offset:0 bytesPerRow:256];
        EXPECT("leased buffer texture created", 1, texture != nil);
        buffer = nil;
        EXPECT("texture retains storage", 1, leases);
        EXPECT("texture retains its buffer", page, texture.buffer.length);
        texture = nil;
    }
    EXPECT("last Metal owner released storage", 0, leases);
    EXPECT("Metal deallocator ran once", 1, lease_releases);
    NvGuestStorageLease *owner = [NvGuestStorageLease new];
    leases = 1; atomic_init(&owner->token, &leases);
    [owner releaseStorage]; [owner releaseStorage]; owner = nil;
    EXPECT("failure/callback release is idempotent", 2, lease_releases);
    lease_base = NULL; lease_bytes = 0; free(memory);
}

static void feedback(const char *msl, int constant, int interleaved) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@(msl) options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvShader *shader = [NvShader new];
    // Production shaders live in the shader cache; pipeline keys use their identities.
    static NSMutableArray<NvShader *> *shaders;
    if (!shaders) shaders = [NSMutableArray array];
    [shaders addObject:shader];
    shader->fn = [library newFunctionWithName:@"main0"]; shader->tf_fn = [library newFunctionWithName:@"main0_tf"]; shader->tf_stride[0] = interleaved ? 24 : 4;
    NvProg *program = fixture_program(); program->sh[0] = shader;
    size_t size = (size_t)getpagesize(); float *memory;
    if (posix_memalign((void **)&memory, size, size)) abort();
    for (size_t i = 0; i < size / sizeof(float); i++) memory[i] = -1;
    mtl_pool_add(memory, size);
    state_reset(); S.tf_on = 1; S.tf[0] = (p_tf_bind){.addr = (uint64_t)memory, .size = size};
    mtl_sub_begin();
    p_draw draw = {.first = 17, .count = 3, .instances = 1}; tf_draw(program, &draw, NULL);
    draw.first = 7; draw.count = 2; tf_draw(program, &draw, NULL);
    unsigned components = interleaved ? 6 : 1;
    EXPECT("feedback byte count", 20 * components, S.tf_off[0]);
    EXPECT("feedback submission completed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("feedback GPU succeeded", 0, mtl_last_commit_error());
    const unsigned values[] = {17, 18, 19, 7, 8};
    for (unsigned i = 0; i < 5; i++) for (unsigned component = 0; component < components; component++)
        EXPECT("feedback contiguous output", (constant ? 7 : values[i]) + (component < 3 ? component : 100 + component - 3), memory[i * components + component]);
    EXPECT("feedback tail untouched", 1, memory[5 * components] == -1);
    mtl_pool_del(memory); mtl_drain(); free(memory);
}

static void vertex_prefix(void) {
    // Force a rebased conversion beyond the standard 4 MiB block. Only the
    // drawn vertices are converted; unused prefix storage is retired at completion.
    const size_t first = ARENA_SIZE / 16 + 17, count = first + 3;
    size_t size = (count + getpagesize() - 1) & ~(size_t)(getpagesize() - 1); uint8_t *memory;
    if (posix_memalign((void **)&memory, (size_t)getpagesize(), size)) abort();
    memset(memory, 0, size); memory[first] = 21; memory[first + 1] = 34; memory[first + 2] = 55;
    mtl_pool_add(memory, size); state_reset();
    S.nattr = S.nstream = 1; S.attr[0] = (va_st){.stream = 0, .fmt = 0x51}; S.stream[0].stride = 1;
    S.vbuf[0] = (NVNbufferRange){(uint64_t)memory, count};
    nvn_refl reflection = {0}; reflection.nv = 1; reflection.v[0].nvn = 0; reflection.v[0].ct = NVN_CT_FLOAT;
    p_draw draw = {.first = (int32_t)first, .count = 3, .instances = 1};
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex void converted(const device float4 *in[[buffer(1)]], device float *out[[buffer(8)]],"
        "uint v[[vertex_id]], uint base[[base_vertex]]){out[v-base]=in[v].x;}" options:nil error:&error];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"converted"]; descriptor.rasterizationEnabled = NO;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    id<MTLBuffer> output = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; res_add(output);
    mtl_sub_begin(); uint64_t converted = g_vc_conv;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    pass.renderTargetWidth = pass.renderTargetHeight = pass.defaultRasterSampleCount = 1;
    id<MTLRenderCommandEncoder> encoder = enc_r_begin(pass); enc_r_pso(encoder, pipeline);
    vconv_bind(encoder, &reflection, &draw, NULL);
    EXPECT("conversion remains limited to drawn vertices", 3, g_vc_conv - converted);
    NSUInteger offset; id<MTLBuffer> buffer = gpu_buffer_required(g_vt_addr[VCONV_SLOT0], 1, &offset);
    EXPECT("large prefix allocated within buffer", 1, buffer.length >= offset + (first + 3) * 16);
    const float *values = (const float *)((const uint8_t *)buffer.contents + offset + first * 16);
    EXPECT("converted vertex 0", 21, values[0]); EXPECT("converted vertex 1", 34, values[4]); EXPECT("converted vertex 2", 55, values[8]);
    enc_r_vbuf(encoder, 8, buffer_gpu_address(output)); enc_r_draw(encoder, MTLPrimitiveTypePoint, first, 3); enc_r_end(encoder);
    uint64_t gpu = buffer_gpu_address(buffer);
    EXPECT("conversion submission completed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    const float *result = output.contents;
    EXPECT("GPU converted vertex 0", 21, result[0]); EXPECT("GPU converted vertex 1", 34, result[1]); EXPECT("GPU converted vertex 2", 55, result[2]);
    // mtl_wait publishes CPU-visible writes before the deferred-release drain.
    mtl_drain();
    EXPECT("oversized prefix removed from registry", 1, gpu_buffer(gpu, 1, &offset) == nil);
    mtl_pool_del(memory); mtl_drain(); free(memory);
}

static id<MTLRenderCommandEncoder> render(id<MTLRenderPipelineState> ps, id<MTLBuffer> source, id<MTLBuffer> output, NSUInteger offset) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    pass.renderTargetWidth = pass.renderTargetHeight = pass.defaultRasterSampleCount = 1;
    id<MTLRenderCommandEncoder> encoder = enc_r_begin(pass);
    enc_r_pso(encoder, ps);
    enc_r_vbuf(encoder, 16, buffer_gpu_address(source));
    enc_r_vbuf(encoder, 8, buffer_gpu_address(output) + offset);
    return encoder;
}

static void visibility_command(unsigned operation, uint64_t address) {
    uint8_t commands[32] __attribute__((aligned(8))); uint64_t type = NVN_COUNTER_TYPE_SAMPLES_PASSED;
    unsigned size = operation == OP_REPORT_COUNTER ? 24 : 16;
    put_hdr(commands, operation, size); memcpy(commands + 8, &type, 8);
    if (size == 24) memcpy(commands + 16, &address, 8);
    put_hdr(commands + size, OP_END, 8); mtl_exec(commands);
}
static void visibility_draw(id<MTLRenderCommandEncoder> encoder) {
    uint32_t slot = g_vis_n++;
    if (g_nvs == g_vscap) { g_vscap = g_vscap ? g_vscap * 2 : 64; g_vs = realloc(g_vs, g_vscap * sizeof *g_vs); }
    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(g_vis), slot};
    enc_r_visibility(encoder, MTLVisibilityResultModeCounting, slot * 8);
    enc_r_draw_instanced(encoder, MTLPrimitiveTypeTriangle, 0, 3, 1, 0);
}
static void visibility_triangle(id<MTLRenderPipelineState> pipeline, id<MTLTexture> target) {
    rp_flush(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore; pass.visibilityResultBuffer = g_vis;
    id<MTLRenderCommandEncoder> encoder = enc_r_begin(pass); enc_r_pso(encoder, pipeline);
    visibility_draw(encoder); enc_r_end(encoder);
}
static void visibility_queries(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 triangle(uint v [[vertex_id]]) { return float4(v == 1 ? 3.0 : -1.0, v == 2 ? 3.0 : -1.0, 0.5, 1.0); }\n"
        "fragment float4 color() { return float4(1.0); }\n" options:nil error:&error];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"triangle"]; descriptor.fragmentFunction = [library newFunctionWithName:@"color"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    MTLTextureDescriptor *texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    texture.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:texture];
    size_t size = (size_t)getpagesize(); uint64_t *memory;
    if (posix_memalign((void **)&memory, size, size)) abort();
    memset(memory, 0xa5, size); mtl_pool_add(memory, size); state_reset(); memset(&R, 0, sizeof R);
    uint64_t address = (uint64_t)memory;
    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0);
    visibility_triangle(pipeline, target); visibility_command(OP_REPORT_COUNTER, address);
    mtl_sub_copy(address, address + 128, 8);
    visibility_triangle(pipeline, target); visibility_command(OP_REPORT_COUNTER, address + 16);
    mtl_sub_copy(address + 16, address + 136, 8);
    visibility_command(OP_REPORT_COUNTER, address + 32); mtl_sub_copy(address + 32, address + 144, 8);
    mtl_sub_fill(address, 8, 0);  // Completion must not overwrite a later GPU write.
    visibility_command(OP_RESET_COUNTER, 0); visibility_command(OP_REPORT_COUNTER, address + 48);
    mtl_sub_copy(address + 48, address + 152, 8);
    EXPECT("visibility consumer submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("visibility GPU succeeds", 0, mtl_last_commit_error());
    EXPECT("query is visible to the next GPU consumer", 64, memory[16]);
    EXPECT("second query includes both render passes", 128, memory[17]);
    EXPECT("query without new draws retains its sum", 128, memory[18]);
    EXPECT("query reset is visible in GPU order", 0, memory[19]);
    EXPECT("completion preserves later GPU overwrites", 0, memory[0]);
    EXPECT("query timestamp is published", 1, memory[1] != 0);
    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0);
    for (unsigned i = 0; i < 3; i++) visibility_triangle(pipeline, target);
    EXPECT("unreported visibility submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    id<MTLBuffer> previous = g_vis;
    mtl_sub_begin(); EXPECT("visibility test reuses a completed result buffer", 1, previous == g_vis);
    visibility_triangle(pipeline, target); visibility_command(OP_REPORT_COUNTER, address + 64);
    mtl_sub_copy(address + 64, address + 160, 8);
    EXPECT("cross-submission visibility completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("reusing visibility slots retains unreported samples", 256, memory[20]);
    EXPECT("cross-submission visibility GPU succeeds", 0, mtl_last_commit_error());
    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0);
    visibility_triangle(pipeline, target); visibility_command(OP_REPORT_COUNTER, address + 192);
    id<MTLBuffer> old_total = g_vis_total;
    for (unsigned i = 0; i < 8192; i++) visibility_command(OP_RESET_COUNTER, 0);
    EXPECT("visibility accumulator blocks rotate", 1, old_total != g_vis_total);
    visibility_triangle(pipeline, target); visibility_command(OP_REPORT_COUNTER, address + 208);
    EXPECT("rotating accumulator submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("retired accumulator retains pending query", 64, memory[24]);
    EXPECT("new accumulator starts at zero", 64, memory[26]);
    EXPECT("rotating visibility GPU succeeds", 0, mtl_last_commit_error());
    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore; pass.visibilityResultBuffer = g_vis;
    id<MTLRenderCommandEncoder> encoder = enc_r_begin(pass); enc_r_pso(encoder, pipeline);
    visibility_draw(encoder); visibility_command(OP_RESET_COUNTER, 0);
    visibility_draw(encoder); enc_r_end(encoder);
    visibility_command(OP_REPORT_COUNTER, address + 224);
    EXPECT("mid-pass reset submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("mid-pass reset excludes earlier samples", 64, memory[28]);
    EXPECT("mid-pass reset GPU succeeds", 0, mtl_last_commit_error());
    vis_clear(); g_vis_counting = g_vis_on = 0;
    mtl_pool_del(memory); mtl_drain(); free(memory); state_reset();
}

#include "visibility_batch_test.inc"
#include "render_pass_batch_test.inc"
#include "discard_batch_test.inc"
#include "discard_inline_test.inc"

static void color_clears(int kind) {
    PFNNVNDEVICEGETPROCADDRESSPROC get = (PFNNVNDEVICEGETPROCADDRESSPROC)h_nvnBootstrapLoader("nvnDeviceGetProcAddress");
    NVNT_nvnCommandBufferInitialize initialize = (NVNT_nvnCommandBufferInitialize)get(NULL, "nvnCommandBufferInitialize");
    NVNT_nvnCommandBufferAddCommandMemory memory = (NVNT_nvnCommandBufferAddCommandMemory)get(NULL, "nvnCommandBufferAddCommandMemory");
    NVNT_nvnCommandBufferBeginRecording begin = (NVNT_nvnCommandBufferBeginRecording)get(NULL, "nvnCommandBufferBeginRecording");
    NVNT_nvnCommandBufferEndRecording end = (NVNT_nvnCommandBufferEndRecording)get(NULL, "nvnCommandBufferEndRecording");
    NVNT_nvnCommandBufferSetRenderTargets targets = (NVNT_nvnCommandBufferSetRenderTargets)get(NULL, "nvnCommandBufferSetRenderTargets");
    NVNT_nvnCommandBufferSetScissor scissor = (NVNT_nvnCommandBufferSetScissor)get(NULL, "nvnCommandBufferSetScissor");
    NVNT_nvnCommandBufferClearColor clear_f = (NVNT_nvnCommandBufferClearColor)get(NULL, "nvnCommandBufferClearColor");
    NVNT_nvnCommandBufferClearColori clear_i = (NVNT_nvnCommandBufferClearColori)get(NULL, "nvnCommandBufferClearColori");
    NVNT_nvnCommandBufferClearColorui clear_u = (NVNT_nvnCommandBufferClearColorui)get(NULL, "nvnCommandBufferClearColorui");
    const uint32_t values_u[2][4] = {{0xffffffff, 0x1000001, 0x80000001, 17}, {0x1000003, 29, 0xfffffffd, 0}};
    const int32_t values_i[2][4] = {{INT32_MIN, -16777217, INT32_MAX, -1}, {16777219, -3, -16777219, INT32_MIN}};
    const float values_f[2][4] = {{0.25f, -0.5f, 2, 1}, {-0.25f, 0.75f, 0.5f, 0}};
    uint32_t bits[2][4];
    memcpy(bits, kind == 1 ? (const void *)values_u : kind == 2 ? (const void *)values_i : (const void *)values_f, sizeof bits);
    MTLPixelFormat format = kind == 1 ? MTLPixelFormatRGBA32Uint : kind == 2 ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Float;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:4 height:4 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> output = [g_dev newTextureWithDescriptor:descriptor];
    tex_t texture = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial()};
    texture.d = (nvn_texdesc){.width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1, .target = NVN_TEXTURE_TARGET_2D,
        .format = kind == 1 ? NVN_FORMAT_RGBA32UI : kind == 2 ? NVN_FORMAT_RGBA32I : NVN_FORMAT_RGBA32F};
    texture.pool = calloc(1, sizeof *texture.pool); serial_put(texture.serial, output);
    const NVNtexture *attachments[] = {(const NVNtexture *)&texture};
    uint8_t commands[4096] __attribute__((aligned(8)));
    pool_t pool = {.mem = commands, .size = sizeof commands};
    NVNcommandBuffer command;
    EXPECT("clear command initializes", 1, initialize(&command, NULL));
    state_reset(); memset(&R, 0, sizeof R);
    for (int phase = 0; phase < 3; phase++) {
        memory(&command, (NVNmemoryPool *)&pool, 0, sizeof commands); begin(&command);
        targets(&command, 1, attachments, NULL, NULL, NULL);
        scissor(&command, phase == 1 ? 1 : 0, phase == 1 ? 1 : 0, phase == 1 ? 2 : 4, phase == 1 ? 2 : 4);
        int row = phase != 0, mask = phase == 1 ? 5 : 15;
        if (kind == 1) clear_u(&command, 0, values_u[row], mask);
        else if (kind == 2) clear_i(&command, 0, values_i[row], mask);
        else clear_f(&command, 0, values_f[row], mask);
        NVNcommandHandle handle = end(&command);
        mtl_sub_begin(); mtl_exec((const uint8_t *)handle);
        EXPECT("clear submission completed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("clear GPU succeeded", 0, mtl_last_commit_error());
        uint32_t pixels[4][4][4];
        [output getBytes:pixels bytesPerRow:sizeof pixels[0] fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) for (int channel = 0; channel < 4; channel++) {
            int changed = phase == 2 || (phase == 1 && x >= 1 && x < 3 && y >= 1 && y < 3 && (mask & (1 << channel)));
            EXPECT("clear readback preserves exact component bits", bits[changed][channel], pixels[y][x][channel]);
        }
    }
    memset(&R, 0, sizeof R); free(texture.pool);
}

static void uploaded_render_target(void) {
    tex_t texture = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial(), .has_layout = 1};
    texture.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8,
        .width = 4, .height = 4, .depth = 2, .levels = 3, .samples = 1};
    nvn_tex_layout(&texture.d, &texture.L);
    size_t page = getpagesize(), size = (texture.L.size + page - 1) & ~(page - 1);
    void *memory; if (posix_memalign(&memory, page, size)) abort();
    memset(memory, 0, size);
    pool_t pool = {.mem = memory, .size = size}; texture.pool = &pool; texture.mem = memory;
    for (unsigned layer = 0; layer < 2; layer++) for (unsigned level = 0; level < 3; level++) {
        uint32_t pixels[16]; unsigned width = 4 >> level;
        for (unsigned i = 0; i < 16; i++) pixels[i] = 0xff302010u + layer * 0x100 + level;
        EXPECT("render-target fixture swizzles", 0, nvn_swizzle(&texture.d, level, layer, pixels, width * 4, memory));
    }
    mtl_pool_add(memory, size); nvn_dirty_add_range(memory, size); mtl_tex_init(&texture);
    mtl_sub_begin(); id<MTLTexture> original = tex_mtl(&texture);
    EXPECT("uploaded texture starts without render-target usage", 0, original.usage & MTLTextureUsageRenderTarget);
    EXPECT("texture upload completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    mtl_sub_begin();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    view_t view = {.set_levels = 1, .base_level = 1, .set_layers = 1, .min_layer = 1};
    attach(pass.colorAttachments[0], &texture, &view, 1);
    id<MTLTexture> upgraded = pass.colorAttachments[0].texture;
    EXPECT("attachment adds render-target usage", MTLTextureUsageRenderTarget, upgraded.usage & MTLTextureUsageRenderTarget);
    EXPECT("attachment selects the requested mip", 1, pass.colorAttachments[0].level);
    EXPECT("attachment selects the requested layer", 1, pass.colorAttachments[0].slice);
    EXPECT("sampling resolves the upgraded registration", 1, tex_mtl(&texture) == upgraded);
    uint64_t upgrades = g_pfv_upgrades;
    MTLRenderPassDescriptor *again = [MTLRenderPassDescriptor new];
    attach(again.colorAttachments[0], &texture, &view, 1);
    EXPECT("repeated attachment reuses the upgraded texture", 1, again.colorAttachments[0].texture == upgraded);
    EXPECT("compatible attachment needs no further upgrade", upgrades, g_pfv_upgrades);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    enc_r_end(enc_r_begin(pass));
    id<MTLBuffer> readback = [g_dev newBufferWithLength:6 * 1024 options:MTLResourceStorageModeShared];
    res_add(readback);
    for (unsigned layer = 0; layer < 2; layer++) for (unsigned level = 0; level < 3; level++) {
        unsigned width = 4 >> level;
        enc_b_copy_texture_buffer(blit(), upgraded, layer, level, MTLOriginMake(0, 0, 0), MTLSizeMake(width, width, 1),
                                 readback, (layer * 3 + level) * 1024, 256, width * 256);
    }
    EXPECT("upgraded attachment submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("upgraded attachment GPU succeeds", 0, mtl_last_commit_error());
    for (unsigned layer = 0; layer < 2; layer++) for (unsigned level = 0; level < 3; level++) {
        unsigned width = 4 >> level;
        uint32_t expected = layer == 1 && level == 1 ? 0xffff00ffu : 0xff302010u + layer * 0x100 + level;
        for (unsigned y = 0; y < width; y++) for (unsigned x = 0; x < width; x++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)readback.contents + (layer * 3 + level) * 1024 + y * 256);
            EXPECT("attachment writes preserve the other layers and mips", expected, row[x]);
        }
    }
    mtl_tex_release(&texture); nvn_dirty_remove_range(memory); mtl_pool_del(memory);
    res_remove(readback); mtl_drain(); free(memory);
}

static void cube_array_view(void) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:4 mipmapped:YES];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> cube = [g_dev newTextureWithDescriptor:d];
    for (unsigned mip = 0; mip < 3; mip++) for (unsigned face = 0; face < 6; face++) {
        uint8_t pixels[64]; memset(pixels, 1 + face + 10 * mip, sizeof pixels);
        [cube replaceRegion:MTLRegionMake2D(0, 0, 4 >> mip, 4 >> mip) mipmapLevel:mip slice:face withBytes:pixels bytesPerRow:4 * (4 >> mip) bytesPerImage:0];
    }
    tex_t texture = {0}; mtl_reg_tex(700, &texture, NULL);
    uint64_t rid = 0; id<MTLTexture> view = tex_fit(700, cube, MTLTextureTypeCubeArray, &rid);
    EXPECT("cube array view created", 1, view != nil); if (!view) return;
    EXPECT("cube array keeps mip levels", 3, view.mipmapLevelCount);
    EXPECT("cube array has one cube", 1, view.arrayLength);
    EXPECT("cube array cache keeps resource identity", rid, resource_gpu_token(tex_fit(700, cube, MTLTextureTypeCubeArray, &rid)));
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void sample_cube(texturecube_array<float> t[[texture(0)]], device uint *o[[buffer(0)]], uint i[[thread_position_in_grid]]){"
        "constexpr sampler s(filter::nearest,mip_filter::nearest); const float3 dir[6]={float3(1,0,0),float3(-1,0,0),float3(0,1,0),float3(0,-1,0),float3(0,0,1),float3(0,0,-1)};"
        "o[i]=uint(round(t.sample(s,dir[i%6],0,level(i/6)).r*255.0f));}"
        options:nil error:&error];
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"sample_cube"] error:&error];
    EXPECT("cube sampler compiles", 1, pipeline != nil); if (!pipeline) return;
    id<MTLBuffer> result = [g_dev newBufferWithLength:18 * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [[g_dev newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline]; [encoder setTexture:view atIndex:0]; [encoder setBuffer:result offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(18, 1, 1) threadsPerThreadgroup:MTLSizeMake(18, 1, 1)]; [encoder endEncoding];
    [command commit]; [command waitUntilCompleted]; EXPECT("cube view GPU succeeds", 1, command.error == nil);
    for (unsigned i = 0; i < 18; i++) EXPECT("cube view preserves every face and mip", 1 + i % 6 + 10 * (i / 6), ((uint32_t *)result.contents)[i]);
    mtl_reg_tex(700, NULL, NULL);
}

static void integer_blending(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]){return float4(float2((i<<1)&2,i&2)*2.0-1.0,0,1);}\n"
        "fragment uint4 u(){return uint4(0x1000001,0xffffffff,17,29);}\n"
        "fragment int4 s(){return int4(-16777217,2147483647,-17,29);}\nfragment float4 f(){return float4(0.25,0.5,0.75,1);}\n"
        options:nil error:&error];
    NvShader *vs = [NvShader new], *fs = [NvShader new]; vs->fn = [library newFunctionWithName:@"v"];
    MTLPixelFormat formats[] = {MTLPixelFormatRGBA32Uint, MTLPixelFormatRGBA32Sint, MTLPixelFormatRGBA8Unorm};
    for (int kind = 0; kind < 3; kind++) {
        fs->fn = [library newFunctionWithName:@[@"u", @"s", @"f"][kind]];
        pso_key key = {.samples = 1, .chmask = 15, .blend_en = 1}; key.cfmt[0] = formats[kind];
        MTLRenderPipelineDescriptor *d = pso_desc(&key, vs, fs, nil, 0);
        EXPECT("integer targets ignore blending; normalized target keeps it", kind == 2, d.colorAttachments[0].blendingEnabled);
        id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
        EXPECT("blending pipeline creates", 1, pipeline != nil); if (!pipeline || kind == 2) continue;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[kind] width:1 height:1 mipmapped:NO];
        td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> output = [g_dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new]; pass.colorAttachments[0].texture = output;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> command = [[g_dev newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline]; [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [encoder endEncoding];
        [command commit]; [command waitUntilCompleted]; EXPECT("integer attachment GPU succeeds", 1, command.error == nil);
        uint32_t pixels[4], expected[2][4] = {{0x1000001,0xffffffff,17,29}, {(uint32_t)-16777217,2147483647,(uint32_t)-17,29}};
        [output getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
        for (int i = 0; i < 4; i++) EXPECT("integer attachment writes exact bits", expected[kind][i], pixels[i]);
    }
}

static void geometry_capture_test(int indexed) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@(geometry_capture_source) options:nil error:&error];
    if (!library) fprintf(stderr, "geometry capture compiler: %s\n", error.localizedDescription.UTF8String);
    NvShader *vertex = [NvShader new]; vertex->gc_fn = [library newFunctionWithName:@"main0_gc"];
    EXPECT("geometry capture entry compiles", 1, vertex->gc_fn != nil); if (!vertex->gc_fn) return;
    mtl_sub_begin();
    uint32_t *state, *values, *ids;
    uint64_t info = geometry_alloc(1056, (void **)&state); memset(state, 0, 1056);
    uint64_t output = geometry_alloc(4 * 142 * 4, (void **)&values); memset(values, 0xa5, 4 * 142 * 4);
    uint64_t indices = geometry_alloc(8, (void **)&ids); ids[0] = 2; ids[1] = 100;
    state[0] = 2; state[1] = 2; state[2] = 7; state[3] = 11; state[6] = indexed;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new]; pass.renderTargetWidth = pass.renderTargetHeight = 1; pass.defaultRasterSampleCount = 1;
    id<MTLRenderCommandEncoder> encoder = enc_r_begin(pass);
    enc_r_pso(encoder, geometry_capture_pso(vertex, 0)); enc_r_vbuf(encoder, 8, output); enc_r_vbuf(encoder, 9, info); enc_r_vbuf(encoder, 14, indices);
    if (indexed) enc_r_draw_indexed(encoder, MTLPrimitiveTypePoint, 2, MTLIndexTypeUInt32, indices, 8, 2, 7, 11);
    else enc_r_draw_instanced(encoder, MTLPrimitiveTypePoint, 7, 2, 2, 11);
    enc_r_end(encoder);
    EXPECT("canonical capture completed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("canonical capture has no GPU error", 0, mtl_last_commit_error());
    for (unsigned instance = 0; instance < 2; instance++) for (unsigned v = 0; v < 2; v++) {
        uint32_t *at = values + (2 * instance + v) * 142;
        uint32_t vid = indexed ? ids[v] + 7 : v + 7;
        float x = (float)vid; uint32_t xbits; memcpy(&xbits, &x, 4);
        EXPECT("canonical capture original vertex position", xbits, at[0]);
        EXPECT("canonical capture preserves negative pre-geometry depth", 0xbf000000, at[2]);
        EXPECT("canonical capture preserves position w", 0x40000000, at[3]);
        EXPECT("canonical capture default point size", 0x3f800000, at[4]);
        EXPECT("canonical capture clip zero", 0xc0400000, at[5]);
        EXPECT("canonical capture clip one", 0x40800000, at[6]);
        EXPECT("canonical capture original vertex ID", vid, at[14]);
        EXPECT("canonical capture original instance ID", instance + 11, at[15]);
        EXPECT("canonical capture NaN payload bits", 0x7fc12345, at[16]);
        EXPECT("canonical capture integer bits", UINT32_MAX, at[17]);
        EXPECT("canonical capture missing attribute w", 0x3f800000, at[21]);
    }
}
static void geometry_assembly_case(unsigned width, uint32_t groups, uint32_t maximum, int empty) {
    mtl_sub_begin(); uint32_t *raw;
    uint32_t stride = 2 * maximum;
    uint64_t address = geometry_alloc((uint64_t)groups * stride * 4, (void **)&raw);
    memset(raw, 0xff, (size_t)groups * stride * 4);
    for (uint32_t group = 0; group < groups; group++) {
        if (empty || (groups > 1 && group % 3 == 0)) continue;
        for (unsigned i = 0; i < maximum; i++) raw[(size_t)group * stride + i] = maximum * group + i;
        if (group % 3 == 2) { raw[(size_t)group * stride + maximum - 1] = UINT32_MAX; raw[(size_t)group * stride + maximum] = maximum * group + maximum - 1; }
    }
    geometry_mesh mesh = {0}; geometry_assemble(&mesh, address, groups, maximum, stride, width, nil);
    NSUInteger offset; id<MTLBuffer> arguments = gpu_buffer_required(mesh.indirect, 20, &offset);
    const uint32_t *draw = (const uint32_t *)((uint8_t *)arguments.contents + offset);
    id<MTLBuffer> buffer = gpu_buffer_required(mesh.indices, mesh.index_bytes, &offset);
    const uint32_t *indices = (const uint32_t *)((uint8_t *)buffer.contents + offset);
    EXPECT("geometry scan/assembly completed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("geometry scan/assembly has no GPU error", 0, mtl_last_commit_error());
    uint32_t at = 0;
    for (uint32_t group = 0; group < groups && !empty; group++) {
        if (groups > 1 && group % 3 == 0) continue;
        unsigned n = group % 3 == 2 ? maximum - 1 : maximum;
        for (unsigned i = 0; i < n; i++) {
            if (i + 1 < width) continue;
            if (width == 1) EXPECT("assembled point", maximum * group + i, indices[at++]);
            else if (width == 2) { EXPECT("assembled line start", maximum * group + i - 1, indices[at++]); EXPECT("assembled line end", maximum * group + i, indices[at++]); }
            else {
                EXPECT("assembled triangle winding a", maximum * group + i - (i & 1 ? 1 : 2), indices[at++]);
                EXPECT("assembled triangle winding b", maximum * group + i - (i & 1 ? 2 : 1), indices[at++]);
                EXPECT("assembled triangle c", maximum * group + i, indices[at++]);
            }
        }
        if (width == 1 && group % 3 == 2) EXPECT("assembled point after cut", maximum * group + maximum - 1, indices[at++]);
    }
    EXPECT("assembled indirect count", at, draw[0]); EXPECT("assembled indirect instance count", 1, draw[1]);
    for (unsigned i = 2; i < 5; i++) EXPECT("assembled indirect origins", 0, draw[i]);
}
static void geometry_assembly_test(unsigned width, uint32_t groups, int empty) {
    geometry_assembly_case(width, groups, 4, empty);
}
static void geometry_small_assembly_test(void) {
    int saved = geometry_small_on();
    const uint32_t cases[][2] = {{1, 4}, {2, 4}, {3, 4}, {9, 4}, {20, 3}, {32, 4}, {33, 4},
                                {1, 128}, {2, 128}, {2, 64}, {4, 32}, {8, 16}, {3, 1}};
    for (int mode = 0; mode < 2; mode++) {
        g_nvn_geometry_small = mode;
        for (unsigned width = 1; width <= 3; width++) {
            for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
                geometry_assembly_case(width, cases[i][0], cases[i][1], 0);
            geometry_assembly_case(width, 17, 4, 1);
        }
    }
    g_nvn_geometry_small = saved;
    const uint32_t expected[] = {0, NVN_GEOMETRY_BAD_INDEX,
        NVN_GEOMETRY_BAD_EMISSION | NVN_GEOMETRY_BAD_CAPACITY, NVN_GEOMETRY_BAD_CAPACITY,
        NVN_GEOMETRY_BAD_INDEX};
    for (unsigned kind = 0; kind < 5; kind++) {
        mtl_sub_begin(); uint32_t *raw, *output, *status, *arguments;
        uint64_t raw_gpu = geometry_alloc(32, (void **)&raw), output_gpu = geometry_alloc(32, (void **)&output);
        uint64_t status_gpu = geometry_alloc(4, (void **)&status), args_gpu = geometry_alloc(20, (void **)&arguments);
        memset(raw, 0xff, 32); memset(output, 0xa5, 32); *status = kind == 4 ? NVN_GEOMETRY_BAD_INDEX : 0;
        for (unsigned i = 0; i < 4; i++) raw[i] = i;
        if (kind == 1) raw[0] = 4;
        if (kind == 2) raw[4] = 0;
        geometry_params *p; uint64_t params_gpu = geometry_alloc(sizeof *p, (void **)&p);
        *p = (geometry_params){1, 4, 8, 3, kind == 3 ? 3 : 6, 1, {0}};
        id<MTLComputeCommandEncoder> encoder = enc_c_begin(nil);
        geometry_pass(encoder, 5, raw_gpu, raw_gpu, output_gpu, args_gpu, status_gpu, params_gpu, 1);
        enc_c_end(encoder);
        EXPECT("small assembly validation completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("small assembly validation has no GPU error", 0, mtl_last_commit_error());
        EXPECT("small assembly preserves validation flags", expected[kind], *status);
        EXPECT("invalid small assembly suppresses the indirect draw", kind ? 0 : 6, arguments[0]);
        EXPECT("small assembly preserves the instance count", 1, arguments[1]);
        for (unsigned i = 2; i < 5; i++) EXPECT("small assembly clears indirect origins", 0, arguments[i]);
        for (unsigned i = kind ? 0 : 6; i < 8; i++) EXPECT("small assembly preserves output guards", 0xa5a5a5a5u, output[i]);
    }
}

static void resource_usage_policy(void) {
    NvShader *shader = [NvShader new]; shader->sb_ro = 1u << 3 | 1u << 31;
    nvn_rbind binding = {.kind = NVN_RB_SBUFP, .rw = 1, .reg = 3};
    EXPECT("proven storage reads omit write usage", MTLResourceUsageRead, storage_usage(shader, &binding));
    binding.reg = 4;
    EXPECT("unproven storage keeps write usage", MTLResourceUsageRead | MTLResourceUsageWrite, storage_usage(shader, &binding));
    binding.reg = 31;
    EXPECT("last proven storage slot is read only", MTLResourceUsageRead, storage_usage(shader, &binding));
    binding.reg = 32;
    EXPECT("out-of-range proof keeps write usage", MTLResourceUsageRead | MTLResourceUsageWrite, storage_usage(shader, &binding));
    binding.reg = 3; binding.kind = NVN_RB_UBOP;
    EXPECT("storage proof does not apply to a uniform slot", MTLResourceUsageRead | MTLResourceUsageWrite, storage_usage(shader, &binding));
    binding.kind = NVN_RB_SBUFU;
    EXPECT("storage proof does not apply to a bindless address", MTLResourceUsageRead | MTLResourceUsageWrite, storage_usage(shader, &binding));
    binding.kind = NVN_RB_SBUFP; g_nvn_storage_readonly = 0;
    EXPECT("legacy policy retains conservative write usage", MTLResourceUsageRead | MTLResourceUsageWrite, storage_usage(shader, &binding));
    g_nvn_storage_readonly = 1; binding.rw = 0;
    EXPECT("declared read-only binding stays read only", MTLResourceUsageRead, storage_usage(shader, &binding));
}

static void geometry_feedback_layout_test(void) {
    NSArray *layout = @[@[@[@[@0, @3], @[@2, @4]], @28], NSNull.null];
    NSArray *offsets = @[
        @{ @"storage": @"Input", @"variable": @"UserDefined", @"location": @0, @"component": @0, @"offset": @99 },
        @{ @"storage": @"Output", @"variable": @"UserDefined", @"location": @0, @"component": @0, @"offset": @7 },
        @{ @"storage": @"Output", @"variable": @"UserDefined", @"location": @0, @"component": @2, @"offset": @9 },
        @{ @"storage": @"Output", @"variable": @"UserDefined", @"location": @0, @"component": @0, @"offset": @11 }
    ];
    geometry_feedback_params parameters;
    geometry_feedback_layout(layout, offsets, &parameters);
    EXPECT("feedback layout stride is in words", 7, parameters.strides[0]);
    EXPECT("feedback layout ignores an empty stream", 0, parameters.strides[1]);
    EXPECT("feedback duplicate output retains its last mapping", 11, parameters.map[0]);
    EXPECT("feedback missing component is zero", UINT32_MAX, parameters.map[1]);
    EXPECT("feedback sparse output is retained", 9, parameters.map[2]);
    for (unsigned i = 3; i < 6; i++) EXPECT("feedback absent attribute defaults to zero", UINT32_MAX, parameters.map[i]);
    EXPECT("feedback absent fourth component defaults to one", UINT32_MAX - 1, parameters.map[6]);
    EXPECT("feedback unused map entries stay unbound", UINT32_MAX, parameters.map[511]);
    geometry_feedback_layout(layout, nil, &parameters);
    const uint32_t canonical[] = {14, 15, 16, 22, 23, 24, 25};
    for (unsigned i = 0; i < 7; i++) EXPECT("feedback canonical vertex record offsets", canonical[i], parameters.map[i]);
}

static void geometry_feedback_test(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@(geometry_capture_source) options:nil error:&error];
    NvShader *shader = [NvShader new]; shader->fn = [library newFunctionWithName:@"main0"];
    shader->gc_fn = [library newFunctionWithName:@"main0_gc"]; shader->gp_fn = [library newFunctionWithName:@"main0_gp_z"];
    shader->tf_layout = @[@[@[@[@0, @3], @[@1, @4]], @28], @[@[@[@0, @4]], @16]];
    NvProg *program = fixture_program(); program->sh[0] = shader;
    size_t size = (size_t)getpagesize(); uint32_t *memory;
    if (posix_memalign((void **)&memory, size, size)) abort(); memset(memory, 0xa5, size);
    mtl_pool_add(memory, size); state_reset();
    S.tf[0] = (p_tf_bind){.addr = (uint64_t)memory, .size = 8 * 28};
    S.tf[1] = (p_tf_bind){.addr = (uint64_t)memory + size / 2, .size = 10 * 16};
    p_tf_state begin = {.what = 0}, pause = {.what = 2}, resume = {.what = 3}; geometry_feedback_state(&begin);
    mtl_sub_begin(); geometry_mesh mesh = {0};
    p_draw points = {.prim = 0, .first = 7, .count = 2, .instances = 2, .base_instance = 11};
    EXPECT("vertex feedback prepares canonical raster stage", 1, geometry_prepare(program, &points, NULL, &mesh) != nil);
    NSData *point_map = shader->tf_maps[1];
    EXPECT("feedback caches an immutable layout", sizeof(geometry_feedback_params), point_map.length);
    id<MTLBuffer> counters = g_tf_counters;
    EXPECT("feedback first submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("feedback first submission GPU succeeds", 0, mtl_last_commit_error());
    EXPECT("feedback first stream byte count", 4 * 28, ((uint32_t *)counters.contents)[0]);
    EXPECT("feedback second stream byte count", 4 * 16, ((uint32_t *)counters.contents)[1]);
    geometry_feedback_state(&pause); EXPECT("feedback pauses", 0, S.tf_on); geometry_feedback_state(&resume); EXPECT("feedback resumes", 1, S.tf_on);
    mtl_sub_begin(); p_draw strip = {.prim = 3, .first = 20, .count = 4, .instances = 1, .base_instance = 2};
    geometry_prepare(program, &strip, NULL, &mesh);
    EXPECT("feedback input topologies have separate cached maps", 1, shader->tf_maps[2] != nil && shader->tf_maps[2] != point_map);
    EXPECT("feedback resume retains GPU counters", 1, counters == g_tf_counters);
    EXPECT("feedback append submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("feedback append GPU succeeds", 0, mtl_last_commit_error());
    EXPECT("feedback capacity truncates complete lines", 8 * 28, ((uint32_t *)counters.contents)[0]);
    EXPECT("feedback streams truncate together", 8 * 16, ((uint32_t *)counters.contents)[1]);
    uint32_t ids[] = {7, 8, 7, 8, 20, 21, 21, 22}, instances[] = {11, 11, 12, 12, 2, 2, 2, 2};
    uint32_t *second = (uint32_t *)((uint8_t *)memory + size / 2);
    for (unsigned i = 0; i < 8; i++) {
        EXPECT("feedback primitive vertex order", ids[i], memory[i * 7]); EXPECT("feedback instance IDs", instances[i], memory[i * 7 + 1]);
        EXPECT("feedback preserves NaN bits", 0x7fc12345, memory[i * 7 + 2]);
        for (unsigned c = 3; c < 6; c++) EXPECT("feedback default missing components", 0, memory[i * 7 + c]);
        EXPECT("feedback default missing w", 0x3f800000, memory[i * 7 + 6]);
        EXPECT("feedback second stream identity", ids[i], second[i * 4]); EXPECT("feedback second stream integer bits", UINT32_MAX, second[i * 4 + 3]);
    }
    EXPECT("feedback first stream overflow canary", 0xa5a5a5a5, memory[8 * 7]); EXPECT("feedback second stream overflow canary", 0xa5a5a5a5, second[8 * 4]);
    geometry_feedback_state(&begin); mtl_sub_begin(); points.first = 100; points.count = points.instances = 1;
    geometry_prepare(program, &points, NULL, &mesh);
    EXPECT("feedback replay reuses its immutable map", 1, point_map == shader->tf_maps[1]);
    EXPECT("feedback reset submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("feedback reset GPU succeeds", 0, mtl_last_commit_error());
    EXPECT("feedback begin resets byte count", 28, ((uint32_t *)g_tf_counters.contents)[0]); EXPECT("feedback begin overwrites first record", 100, memory[0]);
    uint32_t *indirect = (uint32_t *)((uint8_t *)memory + 512); uint32_t args[4] = {1, 1, 200, 17}; memcpy(indirect, args, sizeof args);
    uint16_t *index = (uint16_t *)((uint8_t *)memory + 1024); uint16_t old_indices[3] = {100, 2, 100}, updated[3] = {3, 1, 3}; memcpy(index, old_indices, sizeof old_indices);
    mtl_sub_begin(); points.ind = (uint64_t)indirect;
    geometry_prepare(program, &points, NULL, &mesh);
    EXPECT("indirect arguments update the driver base instance", 17, g_draw_binst);
    uint32_t probe;
    EXPECT("CPU feedback read refuses pending GPU output", 0, geometry_read((uint64_t)memory, 4, &probe));
    mtl_sub_update(updated, (uint64_t)index, sizeof updated);
    p_draw_idx indexed = {.prim = 0, .itype = 1, .count = 3, .base_vertex = 7, .instances = 1, .ibuf = (uint64_t)index};
    geometry_prepare(program, NULL, &indexed, &mesh);
    EXPECT("indirect and indexed feedback completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("indirect and indexed feedback GPU succeeds", 0, mtl_last_commit_error());
    EXPECT("stable indirect draw uses argument origin", 200, memory[7]);
    EXPECT("stable indirect draw uses argument base instance", 17, memory[8]);
    uint32_t updated_ids[] = {10, 8, 10};
    for (unsigned i = 0; i < 3; i++) EXPECT("indexed feedback uses inline updates and dense duplicate IDs", updated_ids[i], memory[(i + 2) * 7]);
    EXPECT("mixed feedback GPU append byte count", 5 * 28, ((uint32_t *)g_tf_counters.contents)[0]);
    static prog_t guest = {.serial = 0x7ffff002}; serial_put(guest.serial, program); S.prog = &guest; S.rast_discard = 1;
    uint32_t *count = (uint32_t *)((uint8_t *)memory + 1536); *count = 1;
    points = (p_draw){.prim = 0, .first = 300, .count = 1, .instances = 1, .mdi_cnt = (uint64_t)count};
    uint64_t waits = g_mdi_waits; mtl_sub_begin(); draw(&points, NULL);
    EXPECT("canonical feedback does not wait for stable MDI counts", waits, g_mdi_waits);
    EXPECT("canonical feedback MDI completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("canonical feedback MDI GPU succeeds", 0, mtl_last_commit_error());
    EXPECT("canonical feedback MDI preserves vertex origin", 300, memory[5 * 7]);
    uint64_t control_a = (uint64_t)memory + 3072, control_b = control_a + 32;
    p_tf_state save_a = {.what = 2, .ctl = control_a}, save_b = {.what = 1, .ctl = control_b};
    p_tf_state restore_a = {.what = 3, .ctl = control_a};
    g_nvn_tf_layout_cache = 0;
    mtl_sub_begin(); geometry_feedback_state(&begin);
    points = (p_draw){.prim = 0, .first = 400, .count = 3, .instances = 1};
    geometry_prepare(program, &points, NULL, &mesh); geometry_feedback_state(&save_a);
    // Another capture reuses the live counters before the first control is restored.
    geometry_feedback_state(&begin); points.first = 500; points.count = 1;
    geometry_prepare(program, &points, NULL, &mesh); geometry_feedback_state(&save_b);
    geometry_feedback_state(&restore_a); points.first = 600; points.count = 2;
    geometry_prepare(program, &points, NULL, &mesh); geometry_feedback_state(&save_a);
    uint32_t *drawn = (uint32_t *)((uint8_t *)memory + 3200);
    for (unsigned i = 0; i < 16; i++) drawn[i] = 0xa5a5a5a5;
    id<MTLLibrary> consume = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex void consume(device uint *out [[buffer(8)]], uint v [[vertex_id]]) { out[v] = v + 1; }\n"
        options:nil error:&error];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [consume newFunctionWithName:@"consume"]; descriptor.rasterizationEnabled = NO;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    void *object; uint64_t offset, gpu;
    EXPECT("feedback control and output resolve", 1, mtl_gpu_addr((uint64_t)memory, size, &object, &offset, &gpu));
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    pass.renderTargetWidth = pass.renderTargetHeight = pass.defaultRasterSampleCount = 1;
    id<MTLRenderCommandEncoder> encoder = enc_r_begin(pass); enc_r_pso(encoder, pipeline);
    enc_r_vbuf(encoder, 8, gpu + 3200); enc_r_draw_indirect(encoder, MTLPrimitiveTypePoint, gpu + 3072 + 16);
    enc_r_vbuf(encoder, 8, gpu + 3200 + 32); enc_r_draw_indirect(encoder, MTLPrimitiveTypePoint, gpu + 3104 + 16);
    enc_r_end(encoder);
    EXPECT("feedback control replay completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("feedback control replay GPU succeeds", 0, mtl_last_commit_error());
    for (unsigned i = 0; i < 5; i++) EXPECT("GPU feedback count drives the consumed vertices", i + 1, drawn[i]);
    EXPECT("GPU feedback count preserves next vertex", 0xa5a5a5a5, drawn[5]);
    EXPECT("independent feedback control retains its count", 1, drawn[8]);
    EXPECT("independent feedback control preserves next vertex", 0xa5a5a5a5, drawn[9]);
    EXPECT("restoring counters does not roll back output data", 500, memory[0]);
    EXPECT("resumed feedback appends at saved offset", 600, memory[3 * 7]);
    EXPECT("resumed feedback appends all vertices", 601, memory[4 * 7]);
    mtl_sub_begin(); geometry_feedback_state(&begin);
    points = (p_draw){.prim = 0, .first = 700, .count = 6, .instances = 1};
    geometry_prepare(program, &points, NULL, &mesh); points.first = 800;
    geometry_prepare(program, &points, NULL, &mesh); geometry_feedback_state(&save_a);
    geometry_feedback_state(&begin);
    points = (p_draw){.prim = 3, .ind = control_a + 16, .instances = 1};
    EXPECT("GPU-counted feedback can feed another capture", 1, geometry_prepare(program, &points, NULL, &mesh) != nil);
    NSUInteger dynamic_offset; id<MTLBuffer> dynamic_arguments = gpu_buffer_required(mesh.indirect, 20, &dynamic_offset);
    EXPECT("GPU-counted capture chain completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("GPU-counted capture chain succeeds", 0, mtl_last_commit_error());
    EXPECT("GPU-counted strip uses actual vertices instead of the upper bound", 14,
           ((const uint32_t *)((const uint8_t *)dynamic_arguments.contents + dynamic_offset))[0]);
    uint32_t strip_ids[] = {0, 1, 1, 2, 2, 3, 3, 4};
    for (unsigned i = 0; i < 8; i++) EXPECT("GPU-counted recapture follows complete strip primitives", strip_ids[i], memory[i * 7]);
    g_nvn_tf_layout_cache = 1;
    EXPECT("uncached feedback preserves the cached layout", 1, point_map == shader->tf_maps[1]);
    id<MTLBuffer> predicted = g_tf_counters; state_op(OP_TF_STATE, &begin);
    EXPECT("pipeline prediction preserves the live feedback resource", 1, predicted == g_tf_counters);
    res_remove(g_tf_counters); g_tf_counters = nil; mtl_pool_del(memory); mtl_drain(); free(memory); state_reset();
}

static void geometry_indirect_topologies(void) {
    const unsigned topologies[] = {0, 1, 2, 3, 4, 5, 6, 10, 11, 12, 13};
    const uint32_t bound = 23;
    size_t size = (size_t)getpagesize(); uint32_t *memory;
    if (posix_memalign((void **)&memory, size, size)) abort();
    mtl_pool_add(memory, size); state_reset();
    void *object; uint64_t offset, gpu;
    if (!mtl_gpu_addr((uint64_t)memory, size, &object, &offset, &gpu)) abort();
    for (unsigned t = 0; t < sizeof topologies / sizeof *topologies; t++) for (uint32_t count = 0; count <= 19; count++) {
        unsigned primitive = topologies[t], width = nvn_geometry_input_width(primitive);
        unsigned output_width = width == 4 ? 2 : width == 6 ? 3 : width;
        uint32_t capacity = nvn_geometry_input_count(primitive, bound), primitives = nvn_geometry_input_count(primitive, count);
        memory[0] = bound; memory[1] = 1; memory[2] = memory[3] = 0;
        uint32_t actual[] = {count, 1, 0, 0};
        mtl_sub_begin(); mtl_sub_update(actual, (uint64_t)memory, sizeof actual);
        uint32_t *info, *remap;
        uint64_t info_gpu = geometry_alloc(1056, (void **)&info); memset(info, 0, 1056);
        size_t bytes = ((size_t)capacity * width + 1) * 4;
        uint64_t remap_gpu = geometry_alloc(bytes, (void **)&remap); memset(remap, 0xa5, bytes);
        geometry_mesh mesh = {0}; uint64_t capture = geometry_indirect_input(&mesh, gpu, info_gpu, remap_gpu, bound, primitive, 0);
        NSUInteger capture_offset, arguments_offset, indices_offset;
        id<MTLBuffer> capture_buffer = gpu_buffer_required(capture, 16, &capture_offset);
        id<MTLBuffer> arguments_buffer = gpu_buffer_required(mesh.indirect, 20, &arguments_offset);
        id<MTLBuffer> indices_buffer = gpu_buffer_required(mesh.indices, mesh.index_bytes, &indices_offset);
        EXPECT("dynamic topology submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("dynamic topology GPU succeeds", 0, mtl_last_commit_error());
        const uint32_t *capture_args = (const uint32_t *)((const uint8_t *)capture_buffer.contents + capture_offset);
        const uint32_t *draw_args = (const uint32_t *)((const uint8_t *)arguments_buffer.contents + arguments_offset);
        const uint32_t *indices = (const uint32_t *)((const uint8_t *)indices_buffer.contents + indices_offset);
        EXPECT("dynamic capture uses actual count", count, capture_args[0]);
        EXPECT("dynamic capture has one instance", 1, capture_args[1]);
        EXPECT("dynamic geometry receives actual vertices", count, info[0]);
        EXPECT("dynamic geometry receives actual primitives", primitives, info[4]);
        EXPECT("dynamic raster receives actual indices", primitives * output_width, draw_args[0]);
        for (uint32_t i = 0; i < primitives; i++) {
            uint32_t positions[6]; nvn_geometry_input_indices(primitive, count, i, positions);
            for (unsigned slot = 0; slot < width; slot++) EXPECT("dynamic geometry input topology", positions[slot], remap[i * width + slot]);
            for (unsigned slot = 0; slot < output_width; slot++) {
                unsigned selected = width == 4 ? slot + 1 : width == 6 ? slot * 2 : slot;
                EXPECT("dynamic feedback raster topology", positions[selected], indices[i * output_width + slot]);
            }
        }
        EXPECT("dynamic topology leaves unused capacity untouched", 0xa5a5a5a5, remap[primitives * width]);
    }
    mtl_pool_del(memory); mtl_drain(); free(memory); state_reset();
}

static void geometry_stage_arguments(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Arguments { const device uint *input [[id(0)]]; };\n"
        "kernel void copy_stage(constant Arguments &args [[buffer(0)]], device uint *out [[buffer(1)]]) { out[0] = args.input[0]; }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvShader *shader = [NvShader new]; shader->stage = 2; shader->fn = [library newFunctionWithName:@"copy_stage"];
    shader->rf.nb = 1; shader->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 0};
    shader->ae = [shader->fn newArgumentEncoderWithBufferIndex:0]; shader->arg_len = shader->ae.encodedLength;
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:shader->fn error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    size_t bytes = (size_t)getpagesize(); uint32_t *memory;
    if (posix_memalign((void **)&memory, bytes, bytes)) abort();
    memory[0] = 0x12345678; mtl_pool_add(memory, bytes); state_reset();
    S.ubo[2][0] = (p_bind_buf){.stage = 2, .index = 0, .addr = (uint64_t)memory, .size = bytes};
    id<MTLBuffer> output = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared]; res_add(output);
    mtl_sub_begin(); id<MTLComputeCommandEncoder> encoder = enc_c_begin(nil); enc_c_pso(encoder, pipeline);
    encode_args(encoder, shader, 2, -1); enc_c_args(encoder, buffer_gpu_address(output), 1);
    enc_c_dispatch(encoder, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(encoder);
    EXPECT("guest geometry argument submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("guest geometry uses compute encoder resource API", 0, mtl_last_commit_error());
    EXPECT("compute backend reads the guest geometry uniform slot", 0x12345678, *(uint32_t *)output.contents);
    res_remove(output); mtl_pool_del(memory); mtl_drain(); free(memory); state_reset();
}

#include "argument_encoding_test.inc"
#include "border_sampling_test.inc"

static void rasterizer_discard_storage(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Arguments { device uint *output [[id(0)]]; texture2d<float> input [[id(1)]]; };\n"
        "vertex float4 write_vertices(constant Arguments &args [[buffer(0)]], uint v [[vertex_id]], uint i [[instance_id]]) {"
        "args.output[i * 64 + v] = 1000 + i * 64 + v + uint(args.input.read(uint2(0)).x); return float4(0, 0, 0, 1); }\n"
        "vertex void discard_vertices(constant Arguments &args [[buffer(0)]], uint v [[vertex_id]], uint i [[instance_id]]) {"
        "args.output[i * 64 + v] = 1000 + i * 64 + v + uint(args.input.read(uint2(0)).x); }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvShader *vertex = [NvShader new]; vertex->fn = [library newFunctionWithName:@"write_vertices"];
    vertex->discard_fn = [library newFunctionWithName:@"discard_vertices"];
    vertex->layer_out = 1;
    vertex->rf.nb = 2; vertex->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 0, .rw = 1};
    vertex->rf.b[1] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .aid = 1, .dtt = MTLTextureType2D};
    vertex->ae = [vertex->fn newArgumentEncoderWithBufferIndex:0]; vertex->arg_len = vertex->ae.encodedLength;
    NvProg *program = fixture_program(); program->sh[0] = vertex;
    static prog_t guest = {.serial = 0x7ffff020}; serial_put(guest.serial, program);
    size_t page = (size_t)getpagesize(); uint32_t *memory;
    if (posix_memalign((void **)&memory, page, 2 * page)) abort();
    mtl_pool_add(memory, 2 * page);
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:4 height:4 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> input = [g_dev newTextureWithDescriptor:descriptor];
    float initial[16] = {0}; [input replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:initial bytesPerRow:16];
    tex_t texture = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial()};
    texture.d = (nvn_texdesc){.width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F};
    texture.pool = calloc(1, sizeof *texture.pool); serial_put(texture.serial, input); mtl_reg_tex(701, &texture, NULL);
    id<MTLBuffer> result = [g_dev newBufferWithLength:page options:MTLResourceStorageModeShared]; res_add(result);
    for (unsigned kind = 0; kind < 6; kind++) {
        memset(memory, 0, 2 * page); memset(result.contents, 0, page);
        uint32_t *commands = (uint32_t *)((uint8_t *)memory + page);
        const uint32_t array_arguments[] = {3, 2, 5, 7}, indexed_arguments[] = {3, 2, 1, 7, 3};
        state_reset(); memset(&R, 0, sizeof R); S.prog = &guest; S.rast_discard = 1; S.ms.a2c = 1;
        S.ssbo[0][0] = (p_bind_buf){.addr = (uint64_t)memory, .size = page};
        S.tex[0][0] = 701;
        p_draw arrays = {.prim = 0, .first = 5, .count = 3, .instances = 2, .base_instance = 7};
        p_draw_idx indexed = {.prim = 0, .itype = kind == 4 ? 0 : 2, .count = 3, .base_vertex = 7,
            .base_instance = 3, .instances = 2, .ibuf = (uint64_t)(commands + 16)};
        uint32_t indices[] = {2, 4, 6}; memcpy(commands + 16, indices, sizeof indices);
        if (kind == 3) { commands[16] = 99; memcpy(commands + 17, indices, sizeof indices); indexed.ind = (uint64_t)commands; }
        if (kind == 4) { uint8_t bytes[] = {2, 4, 6}; memcpy(commands + 16, bytes, sizeof bytes); }
        if (kind == 1) arrays.ind = (uint64_t)commands;
        mtl_sub_begin();
        if (kind == 5) {
            R.rt = (p_set_rt){.n = 1, .color = {&texture}}; R.has_rt = R.clear_c[0] = 1;
            R.cc[0] = MTLClearColorMake(17, 0, 0, 1); lc_push(); memset(&R, 0, sizeof R);
        }
        if (kind == 1 || kind == 3) mtl_sub_update(kind == 1 ? array_arguments : indexed_arguments, (uint64_t)commands,
                                                  kind == 1 ? sizeof array_arguments : sizeof indexed_arguments);
        if (kind < 2 || kind == 5) draw(&arrays, NULL); else draw(NULL, &indexed);
        EXPECT("discard does not enable transform feedback", 0, S.tf_on);
        void *object; uint64_t offset; mtl_addr((uint64_t)memory, page, &object, &offset);
        id<MTLBlitCommandEncoder> encoder = enc_b_begin(g_cb);
        enc_b_copy_buffer(encoder, (__bridge id<MTLBuffer>)object, offset, result, 0, page); enc_b_end(encoder);
        EXPECT("discard storage submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("discard storage GPU succeeds", 0, mtl_last_commit_error());
        for (unsigned i = 0; i < page / 4; i++) {
            unsigned instance = i / 64, vertex_id = i % 64;
            int written = kind < 2 || kind == 5 ? (instance == 7 || instance == 8) && vertex_id >= 5 && vertex_id < 8
                                  : (instance == 3 || instance == 4) && (vertex_id == 9 || vertex_id == 11 || vertex_id == 13);
            EXPECT("discard vertex writes reach the following GPU consumer", written ? 1000 + i + (kind == 5 ? 17 : 0) : 0, ((uint32_t *)result.contents)[i]);
        }
    }
    res_remove(result); mtl_pool_del(memory); mtl_drain(); free(memory); free(texture.pool); state_reset();
}

static void geometry_optimizer_boundaries(void) {
    static prog_t guest = {.serial = 0x7ffff001};
    NvProg *program = fixture_program(); NvShader *vertex = [NvShader new];
    program->sh[0] = vertex; serial_put(guest.serial, program);
    uint64_t textures[6][NTEX] = {0}, images[6][32] = {0}; p_set_rt target = {0};
    EXPECT("ordinary vertex shader can enter dependency analysis", 0, defer_prog_hits(&guest, textures, images, &target, 1, 1));
    program->sh[2] = [NvShader new];
    EXPECT("geometry expansion is a deferred replay boundary", 1, defer_prog_hits(&guest, textures, images, &target, 1, 1));
    program->sh[2] = nil; vertex->generated = 1;
    EXPECT("generated raster stage cannot reuse vertex coverage proof", 1, defer_prog_hits(&guest, textures, images, &target, 1, 1));
    state_reset(); S.prog = &guest; p_draw draw = {.prim = 4, .count = 3, .instances = 1}; g_fd_dw = &draw;
    EXPECT("generated vertices do not elide attachment contents", 0, first_draw_covers(0, nil, 0));
    g_fd_dw = NULL;
    EXPECT("missing original draw has no coverage proof", 0, first_draw_covers(0, nil, 0));
    state_reset();
    defer_reset(); memset(&R, 0, sizeof R); R.has_sc = 1; R.sc = (p_scissor){7, 9, 11, 13}; R.clear_c[3] = 1; R.cc[3].red = 0.5;
    typeof(R) render = R; g_draw_binst = 17;
    typeof(S) *snapshot = malloc(sizeof S); memcpy(snapshot, &S, sizeof S); snapshot->prog = &guest; snapshot->poly.cull = NVN_FACE_FRONT_AND_BACK;
    defer_t *deferred = &g_defer_slots[0]; deferred->active = deferred->in_p = deferred->n = 1; deferred->born = 1;
    deferred->snap[0].S_ = snapshot; deferred->snap[0].dw = (p_draw){.prim = 4, .count = 3, .instances = 1, .base_instance = 99};
    g_defer_nact = 1; defer_fallback();
    EXPECT("replay boundary restores all pending render state", 0, memcmp(&render, &R, sizeof R));
    EXPECT("replay boundary restores current base instance", 17, g_draw_binst);
    EXPECT("replay boundary drains pending groups", 0, g_defer_nact);
    state_reset(); memset(&R, 0, sizeof R);
    vertex->rf.c1_len = 4; vertex->rf.c1 = malloc(4); memcpy(vertex->rf.c1, "test", 4);
    int valid = g_c1_full_valid; NSMutableSet *saved = g_c1_full;
    g_c1_full_valid = 1; g_c1_full = [NSMutableSet set]; c1_store(vertex, 1);
    EXPECT("generated function owns complete constant window", C1_DECL_SIZE, vertex->c1buf.length);
    EXPECT("generated constant payload retained", 0, memcmp(vertex->c1buf.contents, "test", 4));
    EXPECT("generated constant padding stays zero", 0, ((uint8_t *)vertex->c1buf.contents)[C1_DECL_SIZE - 1]);
    g_c1_full_valid = valid; g_c1_full = saved;
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_VCONV_RANGE", "1", 1);
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void stamp(device uint *out[[buffer(0)]], uint i[[thread_position_in_grid]]){out[i]=100+i;}\n"
        "kernel void relay(const device uint *in[[buffer(0)]], device uint *out[[buffer(1)]], uint i[[thread_position_in_grid]]){out[i]=in[i]+7;}\n"
        "vertex void capture(const device uint *in[[buffer(16)]], device uint *out[[buffer(8)]], uint i[[vertex_id]]){out[i]=in[i];}\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> stamp = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"stamp"] error:&error];
    id<MTLComputePipelineState> relay = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"relay"] error:&error];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"capture"]; descriptor.rasterizationEnabled = NO;
    id<MTLRenderPipelineState> capture = [g_dev newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!stamp || !relay || !capture) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }

    id<MTLBuffer> source = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    id<MTLBuffer> middle = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    id<MTLBuffer> result = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [g_dev newBufferWithLength:96 options:MTLResourceStorageModeShared];
    uint32_t indices[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    id<MTLBuffer> index = [g_dev newBufferWithBytes:indices length:sizeof indices options:MTLResourceStorageModeShared];
    uint32_t commands[] = {4, 1, 1, 0, 8, 1, 0, 0, 8, 1, 0, 0, 0};
    id<MTLBuffer> indirect = [g_dev newBufferWithBytes:commands length:sizeof commands options:MTLResourceStorageModeShared];
    for (id<MTLBuffer> buffer in @[source, middle, result, output, index, indirect]) res_add(buffer);

    mtl_sub_begin();
    id<MTLBlitCommandEncoder> empty_blit = enc_b_begin(g_cb);
    [empty_blit fillBuffer:source range:NSMakeRange(0, 32) value:0xff]; enc_b_end(empty_blit);
    id<MTLComputeCommandEncoder> empty_compute = enc_c_begin(NULL);
    enc_c_pso(empty_compute, stamp); enc_c_args(empty_compute, buffer_gpu_address(source), 0);
    enc_c_dispatch(empty_compute, MTLSizeMake(0, 1, 1), MTLSizeMake(1, 1, 1));
    enc_c_dispatch(empty_compute, MTLSizeMake(1, 0, 1), MTLSizeMake(1, 1, 1));
    enc_c_dispatch(empty_compute, MTLSizeMake(1, 1, 0), MTLSizeMake(1, 1, 1)); enc_c_end(empty_compute);
    EXPECT("empty grids complete", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("empty grids do not fault", 0, mtl_last_commit_error());
    for (unsigned i = 0; i < 8; i++) EXPECT("empty grids leave storage untouched", UINT32_MAX, ((uint32_t *)source.contents)[i]);

    mtl_sub_begin();
    id<MTLBlitCommandEncoder> blit = enc_b_begin(g_cb);
    [blit fillBuffer:source range:NSMakeRange(0, 32) value:0xff]; enc_b_end(blit);
    id<MTLComputeCommandEncoder> compute = enc_c_begin(NULL);
    enc_c_pso(compute, stamp); enc_c_args(compute, buffer_gpu_address(source), 0);
    enc_c_dispatch_indirect(compute, buffer_gpu_address(indirect), MTLSizeMake(2, 1, 1)); enc_c_end(compute);
    blit = enc_b_begin(g_cb);
    enc_b_copy_buffer(blit, source, 0, middle, 0, 32); enc_b_end(blit);
    compute = enc_c_begin(NULL);
    enc_c_pso(compute, relay); enc_c_args(compute, buffer_gpu_address(middle), 0); enc_c_args(compute, buffer_gpu_address(result), 1);
    enc_c_dispatch(compute, MTLSizeMake(4, 1, 1), MTLSizeMake(2, 1, 1)); enc_c_end(compute);
    id<MTLRenderCommandEncoder> encoder = render(capture, result, output, 0);
    enc_r_draw_indexed(encoder, MTLPrimitiveTypePoint, 8, MTLIndexTypeUInt32, buffer_gpu_address(index), index.length, 1, 0, 0); enc_r_end(encoder);
    encoder = render(capture, result, output, 32);
    enc_r_draw_indirect(encoder, MTLPrimitiveTypePoint, buffer_gpu_address(indirect) + 16); enc_r_end(encoder);
    encoder = render(capture, result, output, 64);
    enc_r_draw_indexed_indirect(encoder, MTLPrimitiveTypePoint, MTLIndexTypeUInt32, buffer_gpu_address(index), index.length, buffer_gpu_address(indirect) + 32); enc_r_end(encoder);
    EXPECT("submission completed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("no GPU error", 0, mtl_last_commit_error());
    for (unsigned i = 0; i < 24; i++) EXPECT("ordered GPU result", 107 + i % 8, ((const uint32_t *)output.contents)[i]);

    NSUInteger offset = 0;
    EXPECT("buffer subrange resolves", 1, gpu_buffer(buffer_gpu_address(index) + 4, 28, &offset) == index);
    EXPECT("buffer subrange offset", 4, offset);
    EXPECT("one byte past the end rejected", 1, gpu_buffer(buffer_gpu_address(index) + index.length, 1, &offset) == nil);
    EXPECT("oversized range rejected", 1, gpu_buffer(buffer_gpu_address(index), UINT64_MAX, &offset) == nil);
    res_remove(index);
    EXPECT("retired buffer invalidates cached lookup", 1, gpu_buffer(buffer_gpu_address(index), 4, &offset) == nil);
    res_add(index);
    EXPECT("registered buffer resolves again", 1, gpu_buffer(buffer_gpu_address(index), 4, &offset) == index);
    feedback(tf_source_0, 0, 0); feedback(tf_source_1, 0, 0); feedback(tf_source_2, 1, 0); feedback(tf_source_3, 0, 1);
    geometry_feedback_layout_test();
    resource_usage_policy();
    geometry_feedback_test();
    geometry_indirect_topologies();
    geometry_stage_arguments();
    mixed_argument_bindings();
    compact_argument_samplers();
    sampler_border_addressing();
    legacy_border_sampling();
    converted_indirect_indices();
    geometry_optimizer_boundaries();
    rasterizer_discard_storage();
    discard_batch_dependencies();
    discard_inline_dependencies();
    geometry_capture_test(0); geometry_capture_test(1);
    geometry_assembly_test(1, 37, 0); geometry_assembly_test(2, 37, 0); geometry_assembly_test(3, 65537, 0); geometry_assembly_test(3, 17, 1);
    geometry_small_assembly_test();
    vertex_prefix();
    color_clears(0); color_clears(1); color_clears(2);
    visibility_queries();
    visibility_batched_queries();
    visibility_textured_queries();
    visibility_attachmentless_queries();
    render_pass_continuation();
    uploaded_render_target(); cube_array_view(); integer_blending(); storage_lifetime();
    fprintf(stderr, "t_metal3: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
} }
