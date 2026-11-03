// Builder sampling state and view overrides must select the correct GPU plane.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define CHECK(condition) do { checks++; if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); failed++; } } while (0)

int main(int argc, char **argv) { @autoreleasepool {
    mtl_init();
    void *memory;
    if (posix_memalign(&memory, getpagesize(), 65536)) abort();
    memset(memory, 0, 65536);
    NVNmemoryPoolBuilder pool_builder = {0}; NVNmemoryPool pool = {0};
    h_nvnMemoryPoolBuilderSetDefaults(&pool_builder);
    h_nvnMemoryPoolBuilderSetStorage(&pool_builder, memory, 65536);
    CHECK(h_nvnMemoryPoolInitialize(&pool, &pool_builder));
    NVNtextureBuilder builder = {0}; NVNtexture depth = {0}, stencil = {0}, image = {0};
    h_nvnTextureBuilderSetDefaults(&builder);
    h_nvnTextureBuilderSetSize2D(&builder, 8, 8);
    h_nvnTextureBuilderSetFormat(&builder, argc > 1 && !strcmp(argv[1], "--depth24") ? NVN_FORMAT_DEPTH24_STENCIL8 : NVN_FORMAT_DEPTH32F_STENCIL8);
    h_nvnTextureBuilderSetFlags(&builder, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    h_nvnTextureBuilderSetStorage(&builder, &pool, 0);
    CHECK(h_nvnTextureInitialize(&depth, &builder));
    h_nvnTextureBuilderSetDepthStencilMode(&builder, NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    CHECK(h_nvnTextureInitialize(&stencil, &builder));
    h_nvnTextureBuilderSetFlags(&builder, NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE);
    CHECK(h_nvnTextureInitialize(&image, &builder));
    id<MTLTexture> base = tex_mtl((const tex_t *)&depth);
    CHECK(base == tex_mtl((const tex_t *)&stencil));

    NVNtextureView inherited, depth_view, stencil_view;
    h_nvnTextureViewSetDefaults(&inherited);
    h_nvnTextureViewSetLevels(&inherited, 0, 1);
    h_nvnTextureViewSetDefaults(&depth_view);
    h_nvnTextureViewSetDepthStencilMode(&depth_view, NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH);
    h_nvnTextureViewSetDefaults(&stencil_view);
    h_nvnTextureViewSetDepthStencilMode(&stencil_view, NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    mtl_reg_tex(2000, (const tex_t *)&depth, NULL);
    mtl_reg_tex(2001, (const tex_t *)&stencil, NULL);
    mtl_reg_tex(2002, (const tex_t *)&stencil, (const view_t *)&inherited);
    mtl_reg_tex(2003, (const tex_t *)&stencil, (const view_t *)&depth_view);
    mtl_reg_tex(2004, (const tex_t *)&depth, (const view_t *)&stencil_view);
    mtl_reg_tex(2005, (const tex_t *)&image, NULL);
    mtl_reg_tex(2006, (const tex_t *)&image, (const view_t *)&depth_view);
    id<MTLTexture> views[7];
    static const int reads_stencil[] = {0, 1, 1, 0, 1, 1, 0};
    for (int i = 0; i < 7; i++) {
        views[i] = tex_by_id(2000 + i);
        CHECK(views[i].pixelFormat == (reads_stencil[i] ? MTLPixelFormatX32_Stencil8 : MTLPixelFormatDepth32Float_Stencil8));
        CHECK(views[i] == tex_by_id(2000 + i));
    }

    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A { texture2d<float> t [[id(0)]]; };\n"
        "kernel void read_plane(constant A &a [[buffer(0)]], device uint *out [[buffer(1)]], uint2 p [[thread_position_in_grid]]) { out[p.y * 8 + p.x] = as_type<uint>(a.t.read(p).x); }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    id<MTLFunction> function = [library newFunctionWithName:@"read_plane"];
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> encoded[7];
    for (int i = 0; i < 7; i++) {
        encoded[i] = [g_dev newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
        [arguments setArgumentBuffer:encoded[i] offset:0];
        [arguments setTexture:views[i] atIndex:0];
    }
    static const uint32_t stencil_values[] = {0, 1, 8, 128, 255};
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    for (int value = 0; value < 5; value++) {
        float depth_value = .25f + value * .0625f;
        uint32_t depth_bits; memcpy(&depth_bits, &depth_value, sizeof depth_bits);
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
        pass.depthAttachment.texture = pass.stencilAttachment.texture = base;
        pass.depthAttachment.loadAction = pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = pass.stencilAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = depth_value;
        pass.stencilAttachment.clearStencil = stencil_values[value];
        [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
        id<MTLBuffer> output[7];
        for (int i = 0; i < 7; i++) {
            output[i] = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
            id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:encoded[i] offset:0 atIndex:0];
            [encoder setBuffer:output[i] offset:0 atIndex:1];
            [encoder useResource:views[i] usage:MTLResourceUsageRead];
            [encoder dispatchThreads:MTLSizeMake(8, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [encoder endEncoding];
        }
        [commands commit]; [commands waitUntilCompleted];
        CHECK(commands.status == MTLCommandBufferStatusCompleted);
        for (int i = 0; i < 7; i++) {
            const uint32_t *pixels = output[i].contents;
            uint32_t want = reads_stencil[i] ? stencil_values[value] : depth_bits;
            for (int pixel = 0; pixel < 64; pixel++) CHECK(pixels[pixel] == want);
        }
    }
    h_nvnTextureFinalize(&depth); h_nvnTextureFinalize(&stencil); h_nvnTextureFinalize(&image);
    h_nvnMemoryPoolFinalize(&pool); mtl_drain(); free(memory);
    printf("t_stencil_mode: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
