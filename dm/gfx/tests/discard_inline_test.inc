#include <sys/mman.h>
#include <unistd.h>
static uint8_t *inline_test_mapping(size_t bytes) {
    char path[] = "/tmp/nxnative-inline-XXXXXX";
    int file = mkstemp(path); if (file < 0 || ftruncate(file, bytes)) abort();
    uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
    unlink(path); close(file); if (memory == MAP_FAILED) abort();
    memset(memory, 0, bytes); return memory;
}
static void discard_inline_dependencies(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Input { const device uint *value [[id(0)]]; constant uint *expected [[id(1)]]; };\n"
        "struct Out { float4 position [[position]]; uint value [[flat]]; };\n"
        "vertex Out triangle(constant Input &a [[buffer(0)]], uint v [[vertex_id]]) { uint old=*a.value;"
        "return {old==*a.expected ? float4(v==1?3.0:-1.0,v==2?3.0:-1.0,0.5,1) : float4(4,4,4,1),old}; }\n"
        "struct Color { constant uint *bias [[id(0)]]; };\n"
        "fragment float4 color(Out in [[stage_in]], constant Color &a [[buffer(0)]]) { return float4(float(in.value+*a.bias)/255,0,0,1); }\n"
        "struct Write { device uint *value [[id(0)]]; };\n"
        "vertex float4 ordinary(constant Write &a [[buffer(0)]]) { ++*a.value; return float4(0,0,0,1); }\n"
        "vertex void update(constant Write &a [[buffer(0)]]) { ++*a.value; }\n"
        "vertex void collect(constant Write &a [[buffer(0)]], device uint *out [[buffer(8)]], uint v [[vertex_id]]) { out[v*142]=*a.value; ++*a.value; }\n"
        "struct Attribute { uint value [[attribute(0)]]; };\n"
        "vertex float4 ordinary_attribute(Attribute in [[stage_in]], constant Write &a [[buffer(0)]]) { *a.value=in.value+1; return float4(0,0,0,1); }\n"
        "vertex void update_attribute(Attribute in [[stage_in]], constant Write &a [[buffer(0)]]) { *a.value=in.value+1; }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvProg *render = fixture_program(), *update = fixture_program();
    NvShader *vertex = [NvShader new]; vertex->fn = [library newFunctionWithName:@"triangle"];
    vertex->rf.nb = 2;
    vertex->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 0};
    vertex->rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 1, .aid = 1};
    vertex->ae = [vertex->fn newArgumentEncoderWithBufferIndex:0]; vertex->arg_len = vertex->ae.encodedLength;
    NvShader *fragment = [NvShader new]; fragment->fn = [library newFunctionWithName:@"color"]; fragment->stage = 1;
    fragment->rf.color_known = fragment->rf.color_mask = 1; fragment->rf.nb = 1;
    fragment->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 0};
    fragment->ae = [fragment->fn newArgumentEncoderWithBufferIndex:0]; fragment->arg_len = fragment->ae.encodedLength;
    render->sh[0] = vertex; render->sh[1] = fragment;
    NvShader *writer = [NvShader new]; writer->fn = [library newFunctionWithName:@"ordinary"];
    writer->discard_fn = [library newFunctionWithName:@"update"];
    writer->gc_fn = [library newFunctionWithName:@"collect"];
    writer->rf.nb = 1; writer->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 0, .rw = 1};
    writer->ae = [writer->fn newArgumentEncoderWithBufferIndex:0]; writer->arg_len = writer->ae.encodedLength;
    update->sh[0] = writer;
    static prog_t guests[2]; guests[0].serial = nvn_new_serial(); guests[1].serial = nvn_new_serial();
    serial_put(guests[0].serial, render); serial_put(guests[1].serial, update);
    size_t page = (size_t)getpagesize();
    uint8_t *memory = inline_test_mapping(2 * page), *uniforms = inline_test_mapping(2 * page);
    uint64_t address = (uintptr_t)memory, uniform = (uintptr_t)uniforms;
    mach_vm_address_t alias = 0; vm_prot_t current, maximum;
    if (mach_vm_remap(mach_task_self(), &alias, page, 0, VM_FLAGS_ANYWHERE, mach_task_self(), address, FALSE,
                      &current, &maximum, VM_INHERIT_NONE)) abort();
    mtl_pool_add(memory, 2 * page); mtl_pool_add((void *)(uintptr_t)alias, page); mtl_pool_add(uniforms, 2 * page);
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:8 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:descriptor];
    pool_t pool = {.mem = uniforms, .size = 2 * page};
    tex_t texture = {.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial(), .mem = uniforms + page, .pool = &pool};
    texture.d = (nvn_texdesc){.width = 16, .height = 8, .depth = 1, .levels = 1, .samples = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8};
    texture.L.size = 512; serial_put(texture.serial, target);
    id<MTLTexture> other_target = [g_dev newTextureWithDescriptor:descriptor];
    tex_t other_texture = texture; other_texture.serial = nvn_new_serial(); other_texture.mem += 512;
    serial_put(other_texture.serial, other_target);
    int old_inline = discard_inline_on(), old_discard = discard_batch_on(), old_pass = pass_batch_on();
    int old_geometry = geometry_inline_on();
    int old_queries = vis_batch_on(), old_textures = vis_textures_on(), old_vis_on = g_vis_on, old_counting = g_vis_counting;
    g_nvn_discard_batch = 1; g_nvn_pass_batch = g_nvn_query_batch = g_nvn_query_textures = 0;
    EXPECT("inline continuation ends for a copy", 0, discard_inline_keeps(OP_COPY_B2B, NULL));
    EXPECT("inline continuation ends for a query reset", 0, discard_inline_keeps(OP_RESET_COUNTER, NULL));
    for (unsigned mode = 0; mode < 7; mode++) {
        g_nvn_discard_inline = mode != 0; g_vis_on = g_vis_counting = mode == 3;
        g_nvn_geometry_inline = mode >= 5;
        writer->fn = [library newFunctionWithName:mode == 4 ? @"ordinary_attribute" : @"ordinary"];
        writer->discard_fn = [library newFunctionWithName:mode == 4 ? @"update_attribute" : @"update"];
        writer->rf.nv = mode == 4;
        writer->rf.v[0].loc = writer->rf.v[0].nvn = 0; writer->rf.v[0].ct = NVN_CT_UINT;
        memset(memory, 0, 2 * page); memset(uniforms, 0, 2 * page); *(uint32_t *)(uniforms + 128) = 1;
        for (unsigned i = 0; i < 128; i++) *(uint32_t *)(uniforms + 256 + 16 * i) = i;
        state_reset(); memset(&R, 0, sizeof R); S.ds = (ds_st){.df = NVN_DEPTH_FUNC_ALWAYS};
        S.nattr = S.nstream = 1; S.attr[0] = (va_st){.stream = 0, .fmt = NVN_FORMAT_R32UI}; S.stream[0].stride = 4;
        mtl_sub_begin(); if (mode == 3) vis_reset(); uint64_t passes = g_stat_pass, discards = g_discard_passes;
        uint64_t captures = g_geometry_capture_passes, continued = g_geometry_inline_reuses;
        uint32_t *captured = NULL; uint64_t captured_gpu = 0;
        if (mode >= 5) captured_gpu = geometry_alloc(128 * 142 * 4, (void **)&captured);
        R.rt = (p_set_rt){.n = 1, .color = {&texture}}; R.has_rt = R.clear_c[0] = 1;
        R.cc[0] = MTLClearColorMake(0,0,0,0);
        p_draw triangle = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
        p_draw point = {.prim = NVN_DRAW_PRIMITIVE_POINTS, .count = 1, .instances = 1};
        for (unsigned i = 0; i < 128; i++) {
            S.prog = &guests[0]; S.rast_discard = 0;
            R.has_sc = 1; R.sc = (p_scissor){(int)(i % 16), (int)(i / 16), 1, 1};
            p_bind_buf binding = {.stage = 0, .index = 0, .addr = i & 1 ? alias : address, .size = 16};
            state_op(OP_BIND_SSBO, &binding);
            binding = (p_bind_buf){.stage = 0, .index = 1, .addr = uniform + 256 + 16 * i, .size = 16};
            state_op(OP_BIND_UBO, &binding);
            binding = (p_bind_buf){.stage = 1, .index = 0, .addr = mode == 2 || mode == 6 ? address : uniform + 128, .size = 16};
            state_op(OP_BIND_UBO, &binding); draw(&triangle, NULL);
            if (mode >= 5) {
                if (mode == 5 && i == 0) {
                    EXPECT("ordinary vertex stages need their original draw proof", 0, geometry_raster_prefix_safe(render));
                    vertex->generated = 1;
                    EXPECT("completed generated raster stages can supply a read-only prefix", 1, geometry_raster_prefix_safe(render));
                    nvn_rbind saved = fragment->rf.b[0];
                    fragment->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .rw = 1};
                    EXPECT("generated raster fragments with storage writes retain a boundary", 0, geometry_raster_prefix_safe(render));
                    fragment->rf.b[0] = saved; vertex->generated = 0;
                    writer->rf.nb = 2; writer->rf.b[1] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0};
                    EXPECT("texture-consuming capture retains a fragment-to-vertex boundary", 0, geometry_inline_can(writer));
                    writer->rf.nb = 1;
                }
                pass_batch_targets(&other_texture, 1);
                if (i == 0) { pass_batch_targets(&texture, 1); pass_batch_targets(&other_texture, 1); }
                EXPECT("target state waits without ending the prior render encoder", 1, g_geometry_rt_pending);
            }
            S.prog = &guests[1]; S.rast_discard = 1;
            S.vbuf[0] = (NVNbufferRange){i & 1 ? alias : address, 16};
            binding = (p_bind_buf){.stage = 0, .index = 0, .addr = i & 1 ? address : alias, .size = 16};
            state_op(OP_BIND_SSBO, &binding);
            if (mode >= 5) {
                geometry_capture_encoder capture = geometry_capture_begin(writer);
                args_dirty_all(); bind_vbufs(capture.encoder); encode_args(capture.encoder, writer, 0, 1);
                enc_r_vbuf(capture.encoder, 8, captured_gpu);
                enc_r_draw(capture.encoder, MTLPrimitiveTypePoint, i, 1);
                geometry_capture_end(capture); args_dirty_all();
                EXPECT("capture publishes the pending logical render target", other_texture.serial, R.rt.color[0]->serial);
                EXPECT("capture consumes the deferred target state", 0, g_geometry_rt_pending);
                pass_batch_targets(&texture, 1);
            } else draw(&point, NULL);
            if (i == 63) mtl_sub_copy(alias, address + page, 4);
        }
        int combined = mode == 1 || mode == 3 || mode == 4;
        EXPECT("proved disjoint writes retain two passes across a copy", combined ? 2 : 128, g_stat_pass - passes);
        EXPECT("fragment aliases retain separate discarded passes", combined || mode >= 5 ? 0 : 128, g_discard_passes - discards);
        EXPECT("disjoint geometry capture reuses the prior encoder", mode == 5 ? 128 : 0, g_geometry_inline_reuses - continued);
        EXPECT("geometry fragment aliases preserve separate capture encoders", mode == 6 ? 128 : 0, g_geometry_capture_passes - captures);
        if (mode == 3) vis_resolve(uniform + page + 16);
        EXPECT("disjoint continuation GPU completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("disjoint continuation GPU succeeds", 0, mtl_last_commit_error());
        EXPECT("all aliased vertex writes execute", 128, *(uint32_t *)memory);
        EXPECT("copy observes preceding vertex writes", 64, *(uint32_t *)(memory + page));
        if (captured) for (unsigned i = 0; i < 128; i++) EXPECT("geometry capture preserves preceding aliased vertex values", i, captured[i * 142]);
        if (mode == 3) EXPECT("discarded draws add no visibility samples", 128, *(uint64_t *)(uniforms + page + 16));
        uint8_t pixels[512]; [target getBytes:pixels bytesPerRow:64 fromRegion:MTLRegionMake2D(0,0,16,8) mipmapLevel:0];
        for (unsigned i = 0; i < 128; i++) {
            const uint8_t expected[4] = {(uint8_t)(mode == 2 || mode == 6 ? i * 2 : i + 1),0,0,255};
            EXPECT("vertex reads finish before alias writes; fragment reads stay ordered", 0, memcmp(pixels + i * 4, expected, 4));
        }
    }
    g_vis_on = old_vis_on; g_vis_counting = old_counting;
    g_nvn_discard_inline = old_inline; g_nvn_discard_batch = old_discard; g_nvn_pass_batch = old_pass;
    g_nvn_geometry_inline = old_geometry;
    g_nvn_query_batch = old_queries; g_nvn_query_textures = old_textures;
    for (unsigned i = 0; i < 2; i++) serial_del(guests[i].serial);
    serial_del(texture.serial); serial_del(other_texture.serial);
    mtl_pool_del((void *)(uintptr_t)alias); mtl_pool_del(memory); mtl_pool_del(uniforms); mtl_drain();
    mach_vm_deallocate(mach_task_self(), alias, page); munmap(memory, 2 * page); munmap(uniforms, 2 * page);
    state_reset(); memset(&R, 0, sizeof R);
}
