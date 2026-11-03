// GPU validation of the production offline export ABI and pipeline selection.
#include "../nvn/nvn_mtl.m"
#include "gen/fragment_outputs.h"
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) { (void)p; (void)stage; memset(key, 0, 8); return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static unsigned checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { if (failed < 20) fprintf(stderr, "%s: expected %#llx, got %#llx\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static NSDictionary *json(const char *text) {
    NSError *error = nil;
    NSDictionary *value = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:text length:strlen(text)] options:0 error:&error];
    if (!value) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    return value;
}
static id<MTLLibrary> library(const char *source) {
    NSError *error = nil;
    id<MTLLibrary> result = [g_dev newLibraryWithSource:@(source) options:nil error:&error];
    if (!result) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    return result;
}
static MTLPixelFormat format(unsigned width, unsigned kind) {
    static const MTLPixelFormat formats[3][3] = {
        {MTLPixelFormatR32Float, MTLPixelFormatR32Uint, MTLPixelFormatR32Sint},
        {MTLPixelFormatRG32Float, MTLPixelFormatRG32Uint, MTLPixelFormatRG32Sint},
        {MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint, MTLPixelFormatRGBA32Sint},
    };
    return formats[width > 2 ? 2 : width - 1][kind];
}
static NvShader *fragment(id<MTLLibrary> lib) {
    return shader_from_json(@"outputs_b", 1, lib, @{
        @"route": @"B", @"entry": @"main0", @"bindings": @[],
        @"raster": @{@"colors": @[@0, @2, @3, @7], @"discard": @0, @"sample_mask": @0},
        @"fragment_outputs": json(output_metadata), @"fetch": json(output_fetch),
    });
}
static void output_draw(NvShader *vertex, NvShader *pixel, unsigned combination, int mapped) {
    const unsigned colors[] = {0, 2, 3, 7}, widths[] = {1, 2, 4, 4};
    // The merged pass moves each logical output, including an integer output,
    // to a different physical attachment. Types follow the physical targets.
    const uint8_t physical[] = {6, 4, 0, 2};
    pso_key key = {0}; key.samples = 1; key.chmask = UINT32_MAX;
    memset(key.lmap, 0xff, sizeof key.lmap);
    id<MTLTexture> textures[4]; unsigned kinds[4], remaining = combination;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    for (unsigned i = 0; i < 4; i++) {
        unsigned slot = mapped ? physical[i] : colors[i]; kinds[i] = remaining % 3; remaining /= 3;
        key.cfmt[slot] = (uint32_t)format(widths[i], kinds[i]); key.lmap[slot] = (uint8_t)colors[i];
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)key.cfmt[slot] width:1 height:1 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget;
        textures[i] = [g_dev newTextureWithDescriptor:descriptor];
        pass.colorAttachments[slot].texture = textures[i];
        pass.colorAttachments[slot].loadAction = MTLLoadActionClear; pass.colorAttachments[slot].storeAction = MTLStoreActionStore;
    }
    id<MTLFunction> function = nil;
    if (mapped) {
        int fetch[32] = {0}; function = bf_fn(pixel, fetch, 0, key.lmap, key.cfmt);
        EXPECT("mapped function exists", 1, function != nil);
        EXPECT("mapped function cache hit", (uintptr_t)(__bridge void *)function,
            (uintptr_t)(__bridge void *)bf_fn(pixel, fetch, 0, key.lmap, key.cfmt));
    } else {
        function = output_function(pixel, key.cfmt);
        uint32_t same_types[8]; memcpy(same_types, key.cfmt, sizeof same_types);
        for (unsigned i = 0; i < 4; i++) same_types[colors[i]] = (uint32_t)format(1, kinds[i]);
        EXPECT("function cache depends on component type", (uintptr_t)(__bridge void *)function,
            (uintptr_t)(__bridge void *)output_function(pixel, same_types));
    }
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:pso_desc(&key, vertex, pixel, mapped ? function : nil, mapped) error:&error];
    EXPECT("pipeline accepts mixed target types", 1, pipeline != nil);
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return; }
    // NaNs, infinities, signed zero and high integer bits must survive integer
    // exports as register bits, rather than a numeric float-to-integer cast.
    static const uint32_t patterns[] = {0, 0xffffffff, 0x7fc01234, 0x7f801234, 0x80000000, 0x80000001, 0x7f800000, 0xff800000, 0x12345678, 0x7fffffff};
    for (unsigned sample = 0; sample < sizeof patterns / sizeof patterns[0]; sample++) {
        uint32_t input[17] = {0}; input[16] = sample & 1;
        for (unsigned i = 0; i < 4; i++) for (unsigned lane = 0; lane < 4; lane++) {
            uint32_t value = patterns[(sample + lane) % (sizeof patterns / sizeof patterns[0])];
            input[4 * i + lane] = kinds[i] ? value : 0x3f000000u | (value & 0x807fffffu);
        }
        id<MTLCommandBuffer> command = [g_q commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline]; [encoder setFragmentBytes:input length:sizeof input atIndex:16];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [encoder endEncoding];
        [command commit]; [command waitUntilCompleted];
        EXPECT("GPU command completed", MTLCommandBufferStatusCompleted, command.status);
        for (unsigned i = 0; i < 4; i++) {
            uint32_t actual[4] = {0}; unsigned stored = widths[i] > 2 ? 4 : widths[i];
            [textures[i] getBytes:actual bytesPerRow:4 * stored fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
            for (unsigned lane = 0; lane < widths[i]; lane++) EXPECT("export preserves register bits", input[4 * i + lane], actual[lane]);
        }
    }
}

int main(void) { @autoreleasepool {
    mtl_init();
    NvShader *vertex = [NvShader new];
    vertex->fn = [library("#include <metal_stdlib>\nusing namespace metal; vertex float4 v(uint i [[vertex_id]]) { return float4(float2((i<<1)&2,i&2)*2.0-1.0,0,1); }") newFunctionWithName:@"v"];
    NvShader *pixel = fragment(library(output_source));
    EXPECT("reflection loader accepts export ABI", 1, pixel != nil);
    EXPECT("default function exists", 1, pixel->fn != nil);
    NvShader *legacy = [NvShader new]; legacy->fn = vertex->fn;
    EXPECT("legacy shader has no specialization", (uintptr_t)(__bridge void *)legacy->fn, (uintptr_t)(__bridge void *)output_function(legacy, NULL));
    g_bf_fn_cache = [NSMutableDictionary dictionary]; g_bf_lib_cache = [NSMutableDictionary dictionaryWithObject:library(output_mapped_source) forKey:@"outputs"];
    for (unsigned mapped = 0; mapped < 2; mapped++) for (unsigned combination = 0; combination < 81; combination++) {
        @autoreleasepool { output_draw(vertex, pixel, combination, (int)mapped); }
    }
    EXPECT("all type combinations cached", 81, pixel->output_functions.count);
    EXPECT("mapped cache separates target types", 81, g_bf_fn_cache.count);
    printf("FRAGMENT_OUTPUT_GPU checks=%u failed=%u\n", checks, failed);
    return failed != 0;
} }
