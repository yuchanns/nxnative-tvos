static void pass_batch_targets(tex_t *texture, int levels) {
    p_set_rt targets = {.n = 1, .color = {texture}, .has_cview = {1}};
    targets.cview[0] = (view_t){.set_levels = 1, .base_level = 0, .nlevels = levels};
    uint8_t commands[sizeof targets + 16] __attribute__((aligned(8)));
    put_hdr(commands, OP_SET_RT, sizeof targets + 8); memcpy(commands + 8, &targets, sizeof targets);
    put_hdr(commands + sizeof targets + 8, OP_END, 8); mtl_exec(commands);
}
static void pass_batch_uniform(uint64_t address) {
    p_bind_buf binding = {.stage = 1, .index = 0, .addr = address, .size = 16};
    state_op(OP_BIND_UBO, &binding);
}
static void pass_batch_pixel_check(id<MTLTexture> target, const uint8_t expected[4]) {
    uint8_t pixels[256]; [target getBytes:pixels bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    for (unsigned i = 0; i < 64; i++) EXPECT("pass continuation preserves ordered pixel writes", 0, memcmp(pixels + 4 * i, expected, 4));
}
static void render_pass_continuation(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "float4 position(uint v) { return float4(v==1?3.0:-1.0,v==2?3.0:-1.0,0.5,1); }\n"
        "vertex float4 triangle(uint v [[vertex_id]]) { return position(v); }\n"
        "struct Color { constant float4 *value [[id(0)]]; };\n"
        "fragment float4 color(constant Color &a [[buffer(0)]]) { return *a.value; }\n"
        "struct Image { texture2d<float> value [[id(0)]]; };\n"
        "fragment float4 feedback(constant Image &a [[buffer(0)]], float4 p [[position]]) { return a.value.read(uint2(p.xy)) + float4(0,0.25,0,0); }\n"
        "struct Write { device uint *value [[id(0)]]; };\n"
        "vertex float4 write(constant Write &a [[buffer(0)]], uint v [[vertex_id]]) { a.value[v]=100+v; return position(v); }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvProg *programs[3]; static prog_t guests[3];
    NvShader *vertex = [NvShader new]; vertex->fn = [library newFunctionWithName:@"triangle"];
    NvShader *fragment = [NvShader new]; fragment->fn = [library newFunctionWithName:@"color"]; fragment->stage = 1;
    fragment->rf.color_known = fragment->rf.color_mask = 1; fragment->rf.nb = 1;
    fragment->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 0};
    fragment->ae = [fragment->fn newArgumentEncoderWithBufferIndex:0]; fragment->arg_len = fragment->ae.encodedLength;
    for (unsigned i = 0; i < 3; i++) {
        programs[i] = fixture_program(); programs[i]->sh[0] = vertex; programs[i]->sh[1] = fragment;
        guests[i].serial = nvn_new_serial(); serial_put(guests[i].serial, programs[i]);
    }
    NvShader *feedback = [NvShader new]; feedback->fn = [library newFunctionWithName:@"feedback"]; feedback->stage = 1;
    feedback->rf.color_known = feedback->rf.color_mask = 1; feedback->rf.nb = 1;
    feedback->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .aid = 0, .dtt = MTLTextureType2D};
    feedback->ae = [feedback->fn newArgumentEncoderWithBufferIndex:0]; feedback->arg_len = feedback->ae.encodedLength;
    programs[1]->sh[1] = feedback;
    NvShader *writer = [NvShader new]; writer->fn = [library newFunctionWithName:@"write"];
    writer->rf.nb = 1; writer->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 0, .rw = 1};
    writer->ae = [writer->fn newArgumentEncoderWithBufferIndex:0]; writer->arg_len = writer->ae.encodedLength;
    programs[2]->sh[0] = writer;
    size_t page = (size_t)getpagesize(); uint8_t *memory;
    if (posix_memalign((void **)&memory, page, 2 * page)) abort();
    memset(memory, 0, 2 * page); mtl_pool_add(memory, 2 * page);
    const float colors[3][4] = {{0.25,0,0,0}, {0,0.25,0,0}, {0,0,0.25,0}};
    memcpy(memory, colors, sizeof colors); uint64_t address = (uint64_t)(uintptr_t)memory;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:YES];
    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:descriptor];
    pool_t pool = {.mem = memory, .size = 2 * page};
    tex_t texture = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial(), .pool = &pool, .mem = memory + page};
    texture.d = (nvn_texdesc){.width = 8, .height = 8, .depth = 1, .levels = 4, .samples = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8};
    texture.L.size = 340; serial_put(texture.serial, target); mtl_reg_tex(704, &texture, NULL);
    tex_t alias = texture; alias.serial = nvn_new_serial();
    serial_put(alias.serial, target); mtl_reg_tex(705, &alias, NULL);
    p_set_rt first = {.n = 1, .color = {&texture}, .has_cview = {1}};
    first.cview[0] = (view_t){.set_levels = 1, .nlevels = 1}; p_set_rt later = first;
    later.cview[0].nlevels = 4;
    EXPECT("positive mip-count differences select the same attachment", 1, rt_same_attachments(&first, &later));
    later.cview[0].nlevels = 0;
    EXPECT("unknown mip-count differences stay conservative", 0, rt_same_attachments(&first, &later));
    later.cview[0].nlevels = 5;
    EXPECT("out-of-bounds mip-count differences stay conservative", 0, rt_same_attachments(&first, &later));
    later.cview[0].nlevels = 1; later.cview[0].base_level = 1;
    EXPECT("different attached mip levels do not merge", 0, rt_same_attachments(&first, &later));
    EXPECT("private texture storage can be bounded", 1, rt_batch_storage_known(&first));
    texture.d.flags |= NVN_TEXTURE_FLAGS_LINEAR;
    EXPECT("linear targets retain buffer-alias boundaries", 0, rt_batch_storage_known(&first));
    texture.d.flags &= ~NVN_TEXTURE_FLAGS_LINEAR;
    pool.flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL;
    EXPECT("virtual targets retain mapping boundaries", 0, rt_batch_storage_known(&first)); pool.flags = 0;
    int old_batch = pass_batch_on(), old_queries = vis_batch_on(), old_textures = vis_textures_on();
    g_nvn_query_batch = g_nvn_query_textures = 0;
    p_draw triangle = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    for (unsigned enabled = 0; enabled <= 1; enabled++) {
        g_nvn_pass_batch = enabled; state_reset(); memset(&R, 0, sizeof R); S.prog = &guests[0];
        S.ds = (ds_st){.df = NVN_DEPTH_FUNC_ALWAYS}; S.color.blend_en = 1;
        S.blend[0] = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
        mtl_sub_begin(); uint64_t passes = g_stat_pass;
        for (unsigned i = 0; i < 3; i++) {
            pass_batch_targets(&texture, i == 1 ? 4 : 1);
            if (!i) { R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1); }
            pass_batch_uniform(address + 16 * i); draw(&triangle, NULL);
        }
        EXPECT("ordinary passes merge only when enabled", enabled ? 1 : 3, g_stat_pass - passes);
        EXPECT("ordinary batch submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("ordinary batch GPU succeeds", 0, mtl_last_commit_error());
        const uint8_t mixed[4] = {64,64,64,255}; pass_batch_pixel_check(target, mixed);

        S.prog = &guests[0]; mtl_sub_begin(); passes = g_stat_pass;
        pass_batch_targets(&texture, 1); R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
        pass_batch_uniform(address); draw(&triangle, NULL);
        pass_batch_targets(&texture, 4); S.prog = &guests[1]; S.tex[1][0] = 705;
        color_st overwrite = S.color; overwrite.blend_en = 0;
        p_bind_state state = {.kind = 1}; memcpy(state.bytes, &overwrite, sizeof overwrite); state_op(OP_BIND_STATE, &state);
        draw(&triangle, NULL);
        EXPECT("feedback through a different texture serial keeps its boundary", 2, g_stat_pass - passes);
        EXPECT("feedback batch submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("feedback batch GPU succeeds", 0, mtl_last_commit_error());
        const uint8_t feedback_color[4] = {64,64,0,255}; pass_batch_pixel_check(target, feedback_color);

        S.prog = &guests[0]; mtl_sub_begin(); passes = g_stat_pass;
        pass_batch_targets(&texture, 1); R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
        pass_batch_uniform(address); draw(&triangle, NULL);
        pass_batch_targets(&texture, 4); S.prog = &guests[2];
        p_bind_buf storage = {.stage = 0, .index = 0, .addr = address + 128, .size = 16}; state_op(OP_BIND_SSBO, &storage);
        draw(&triangle, NULL);
        EXPECT("external storage writes keep their original boundary", 2, g_stat_pass - passes);
        EXPECT("storage-writing batch submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("storage-writing batch GPU succeeds", 0, mtl_last_commit_error());
        for (unsigned i = 0; i < 3; i++) EXPECT("storage writer still executes", 100 + i, ((uint32_t *)(memory + 128))[i]);

        memcpy(memory, colors, sizeof colors); S.prog = &guests[0];
        color_st additive = S.color; additive.blend_en = 1;
        memcpy(state.bytes, &additive, sizeof additive); state_op(OP_BIND_STATE, &state);
        mtl_sub_begin(); passes = g_stat_pass; pass_batch_targets(&texture, 1);
        R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
        pass_batch_uniform(address); draw(&triangle, NULL);
        mtl_sub_copy(address + 16, address, 16);
        pass_batch_targets(&texture, 4); pass_batch_uniform(address); draw(&triangle, NULL);
        EXPECT("intervening GPU copy keeps its boundary", 2, g_stat_pass - passes);
        EXPECT("copied-uniform batch submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("copied-uniform batch GPU succeeds", 0, mtl_last_commit_error());
        pass_batch_pixel_check(target, feedback_color);
        memcpy(memory, colors, sizeof colors);
    }
    g_nvn_pass_batch = old_batch; g_nvn_query_batch = old_queries; g_nvn_query_textures = old_textures;
    state_reset(); memset(&R, 0, sizeof R); mtl_reg_tex(704, NULL, NULL); serial_del(texture.serial);
    mtl_reg_tex(705, NULL, NULL); serial_del(alias.serial);
    for (unsigned i = 0; i < 3; i++) serial_del(guests[i].serial);
    mtl_pool_del(memory); mtl_drain(); free(memory);
}
