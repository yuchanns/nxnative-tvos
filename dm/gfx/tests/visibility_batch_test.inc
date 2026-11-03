static void visibility_batched_queries(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "float4 position(uint v) { return float4(v == 1 ? 3.0 : -1.0, v == 2 ? 3.0 : -1.0, 0.5, 1.0); }\n"
        "vertex float4 triangle(uint v [[vertex_id]]) { return position(v); }\n"
        "struct Arguments { const device ulong *counter [[id(0)]]; };\n"
        "vertex float4 consume(constant Arguments &args [[buffer(0)]], uint v [[vertex_id]]) {"
        "return *args.counter == 64 ? position(v) : float4(4,4,4,1); }\n"
        "fragment float4 red() { return float4(1,0,0,1); } fragment float4 green() { return float4(0,1,0,1); }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvProg *programs[2]; static prog_t guests[2];
    for (unsigned i = 0; i < 2; i++) {
        programs[i] = fixture_program();
        programs[i]->sh[0] = [NvShader new]; programs[i]->sh[1] = [NvShader new];
        programs[i]->sh[0]->fn = [library newFunctionWithName:i ? @"consume" : @"triangle"];
        programs[i]->sh[1]->fn = [library newFunctionWithName:i ? @"green" : @"red"];
        programs[i]->sh[1]->stage = 1;
        programs[i]->sh[1]->rf.color_known = programs[i]->sh[1]->rf.color_mask = 1;
        guests[i].serial = nvn_new_serial(); serial_put(guests[i].serial, programs[i]);
    }
    NvShader *consumer = programs[1]->sh[0];
    consumer->rf.nb = 1; consumer->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 0};
    consumer->ae = [consumer->fn newArgumentEncoderWithBufferIndex:0]; consumer->arg_len = consumer->ae.encodedLength;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:descriptor];
    tex_t texture = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial()};
    texture.d = (nvn_texdesc){.width = 8, .height = 8, .depth = 1, .levels = 1, .samples = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8};
    texture.pool = calloc(1, sizeof *texture.pool); serial_put(texture.serial, target);
    size_t size = (size_t)getpagesize(); uint64_t *memory;
    if (posix_memalign((void **)&memory, size, size)) abort();
    memset(memory, 0xa5, size); mtl_pool_add(memory, size);
    uint64_t address = (uint64_t)memory;
    int previous = g_nvn_query_batch; g_nvn_query_batch = 1;
    state_reset(); memset(&R, 0, sizeof R); S.prog = &guests[0];
    S.ds = (ds_st){.df = NVN_DEPTH_FUNC_ALWAYS};
    R.rt = (p_set_rt){.n = 1, .color = {&texture}}; R.has_rt = R.clear_c[0] = 1;
    R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
    texture.mem = (void *)(uintptr_t)(address + 256); texture.L.size = 16;
    EXPECT("query output alias of an attachment is rejected", 1, vis_destination_is_attachment(address + 256));
    EXPECT("query output ending at an attachment is disjoint", 0, vis_destination_is_attachment(address + 240));
    EXPECT("query output starting after an attachment is disjoint", 0, vis_destination_is_attachment(address + 272));
    EXPECT("query output partly overlapping an attachment is rejected", 1, vis_destination_is_attachment(address + 255));
    texture.L.size = 0;
    EXPECT("unknown attachment storage stays conservative", 1, vis_destination_is_attachment(address));
    texture.mem = NULL;
    p_draw triangle = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0);
    uint64_t passes = g_stat_pass;
    draw(&triangle, NULL); id<MTLRenderCommandEncoder> first = g_enc;
    visibility_command(OP_REPORT_COUNTER, address);
    EXPECT("query report retains its render encoder", 1, first == g_enc);
    EXPECT("query report is queued", 1, g_vis_pending_n);
    uint8_t pools[2 * (sizeof(op_hdr) + sizeof(p_set_pool)) + sizeof(op_hdr)] __attribute__((aligned(8))) = {0};
    size_t pool_size = sizeof(op_hdr) + sizeof(p_set_pool);
    put_hdr(pools, OP_SET_TEX_POOL, pool_size);
    put_hdr(pools + pool_size, OP_SET_SAMP_POOL, pool_size);
    put_hdr(pools + 2 * pool_size, OP_END, sizeof(op_hdr));
    mtl_exec(pools);
    EXPECT("descriptor pool records retain the render encoder", 1, first == g_enc);
    EXPECT("descriptor pool records retain pending query output", 1, g_vis_pending_n);
    uint8_t discard[sizeof(op_hdr) + sizeof(p_rast_discard) + sizeof(op_hdr)] __attribute__((aligned(8))) = {0};
    size_t discard_size = sizeof(op_hdr) + sizeof(p_rast_discard);
    put_hdr(discard, OP_RAST_DISCARD, discard_size);
    put_hdr(discard + discard_size, OP_END, sizeof(op_hdr));
    mtl_exec(discard);
    EXPECT("disabling rasterizer discard retains the render encoder", 1, first == g_enc);
    EXPECT("disabling rasterizer discard retains pending query output", 1, g_vis_pending_n);
    uint8_t targets[sizeof(p_set_rt) + 16] __attribute__((aligned(8)));
    put_hdr(targets, OP_SET_RT, sizeof(p_set_rt) + 8); memcpy(targets + 8, &R.rt, sizeof R.rt);
    put_hdr(targets + sizeof(p_set_rt) + 8, OP_END, 8); mtl_exec(targets);
    EXPECT("same-target query continuation retains the render encoder", 1, first == g_enc);
    draw(&triangle, NULL); visibility_command(OP_REPORT_COUNTER, address + 16);
    visibility_command(OP_REPORT_COUNTER, address + 32);
    visibility_command(OP_RESET_COUNTER, 0); visibility_command(OP_REPORT_COUNTER, address + 48);
    draw(&triangle, NULL); visibility_command(OP_REPORT_COUNTER, address + 64);
    EXPECT("consecutive reports share one render pass", 1, g_stat_pass - passes);
    EXPECT("all reports await the same boundary", 5, g_vis_pending_n);
    mtl_sub_copy(address + 64, address + 128, 8);
    EXPECT("immediate copy drains pending reports", 0, g_vis_pending_n);
    mtl_sub_fill(address + 64, 8, 0);
    EXPECT("batched query submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("batched query GPU succeeds", 0, mtl_last_commit_error());
    const uint64_t expected[] = {64, 128, 128, 0, 0};
    for (unsigned i = 0; i < 5; i++) EXPECT("batched report preserves cumulative/reset order", expected[i], memory[2 * i]);
    EXPECT("batched result reaches immediate GPU consumer", 64, memory[16]);
    EXPECT("batched timestamp is published", 1, memory[1] != 0);

    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0); draw(&triangle, NULL);
    const uint64_t earlier_update[2] = {999, 999}; mtl_sub_update(earlier_update, address + 368, sizeof earlier_update);
    visibility_command(OP_REPORT_COUNTER, address + 368);
    EXPECT("update-before-query submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("update-before-query GPU succeeds", 0, mtl_last_commit_error());
    EXPECT("query publishes after the preceding inline update", 64, memory[46]);

    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0);
    S.prog = &guests[0]; draw(&triangle, NULL); visibility_command(OP_REPORT_COUNTER, address + 256);
    EXPECT("shader-consumer test has a pending report", 1, g_vis_pending_n);
    S.prog = &guests[1]; S.ssbo[0][0] = (p_bind_buf){.addr = address + 256, .size = 16};
    draw(&triangle, NULL);
    EXPECT("shader read drains its producer before encoding", 0, g_vis_pending_n);
    EXPECT("shader-consumer submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("shader-consumer GPU succeeds", 0, mtl_last_commit_error());
    uint8_t pixels[256]; [target getBytes:pixels bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    const uint8_t green[4] = {0, 255, 0, 255};
    for (unsigned i = 0; i < 64; i++) EXPECT("next shader observes the resolved query", 0, memcmp(pixels + 4 * i, green, 4));

    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0); S.prog = &guests[0];
    for (unsigned i = 0; i < 3; i++) draw(&triangle, NULL);
    EXPECT("unreported batched samples commit", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    mtl_sub_begin(); draw(&triangle, NULL); visibility_command(OP_REPORT_COUNTER, address + 272);
    EXPECT("cross-submission batched query completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("batched accumulator retains unreported samples", 256, memory[34]);

    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0); g_vis_n = VIS_SLOTS - 1;
    S.prog = &guests[0]; draw(&triangle, NULL); visibility_command(OP_REPORT_COUNTER, address + 304);
    id<MTLBuffer> full = g_vis;
    visibility_command(OP_RESET_COUNTER, 0); draw(&triangle, NULL);
    EXPECT("full visibility buffer closes before the next query", 1, full != g_vis);
    visibility_command(OP_REPORT_COUNTER, address + 320);
    EXPECT("visibility overflow batch completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("last slot of the old visibility buffer is retained", 64, memory[38]);
    EXPECT("first slot of the new visibility buffer is independent", 64, memory[40]);
    EXPECT("visibility overflow GPU succeeds", 0, mtl_last_commit_error());

    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0); draw(&triangle, NULL);
    visibility_command(OP_REPORT_COUNTER, address + 336);
    uintptr_t old_total = (uintptr_t)(__bridge void *)g_vis_total;
    for (unsigned i = 0; i < 8192; i++) visibility_command(OP_RESET_COUNTER, 0);
    EXPECT("pending report retains its retired accumulator block", 1,
        old_total != (uintptr_t)(__bridge void *)g_vis_total);
    draw(&triangle, NULL); visibility_command(OP_REPORT_COUNTER, address + 352);
    const uint64_t later_update[2] = {7, 0}; mtl_sub_update(later_update, address + 352, sizeof later_update);
    EXPECT("direct update drains the pending query batch", 0, g_vis_pending_n);
    EXPECT("retired query accumulator submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("retired query accumulator GPU succeeds", 0, mtl_last_commit_error());
    EXPECT("retired accumulator preserves its original report", 64, memory[42]);
    EXPECT("later direct update wins over its preceding report", 7, memory[44]);

    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0); draw(&triangle, NULL);
    for (unsigned i = 0; i < 600; i++) visibility_command(OP_REPORT_COUNTER, address + 288);
    EXPECT("bounded query queue flush completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("bounded query queue preserves its last report", 64, memory[36]);
    EXPECT("bounded query queue GPU succeeds", 0, mtl_last_commit_error());

    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0); draw(&triangle, NULL);
    visibility_command(OP_REPORT_COUNTER, address + 400);
    p_rast_discard enabled = {.on = 1}; memcpy(discard + sizeof(op_hdr), &enabled, sizeof enabled);
    mtl_exec(discard);
    EXPECT("enabling rasterizer discard drains pending queries", 0, g_vis_pending_n);
    EXPECT("enabling rasterizer discard closes the render encoder", 1, g_enc == nil);
    enabled.on = 0; memcpy(discard + sizeof(op_hdr), &enabled, sizeof enabled); mtl_exec(discard);
    EXPECT("discard boundary submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("discard boundary preserves the earlier query result", 64, memory[50]);
    EXPECT("discard boundary GPU succeeds", 0, mtl_last_commit_error());
    g_nvn_query_batch = previous; vis_clear(); g_vis_counting = g_vis_on = 0;
    mtl_pool_del(memory); mtl_drain(); free(memory); serial_del(texture.serial); free(texture.pool);
    serial_del(guests[0].serial); serial_del(guests[1].serial); state_reset(); memset(&R, 0, sizeof R);
}

static void visibility_textured_queries(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 triangle(uint v [[vertex_id]]) { return float4(v == 1 ? 3.0 : -1.0, v == 2 ? 3.0 : -1.0, 0.5, 1.0); }\n"
        "struct Image { texture2d<uint> value [[id(0)]]; };\n"
        "struct Buffer { texture_buffer<uint> value [[id(0)]]; };\n"
        "fragment float4 red() { return float4(1,0,0,1); }\n"
        "fragment float4 image(constant Image &a [[buffer(0)]]) { return a.value.read(uint2(0)).x == 64 ? float4(0,1,0,1) : float4(1,0,0,1); }\n"
        "fragment float4 typed(constant Buffer &a [[buffer(0)]]) { return a.value.read(uint(0)).x == 64 ? float4(0,1,0,1) : float4(1,0,0,1); }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvProg *programs[3]; static prog_t guests[3];
    for (unsigned i = 0; i < 3; i++) {
        programs[i] = fixture_program();
        NvShader *vertex = [NvShader new], *fragment = [NvShader new];
        vertex->fn = [library newFunctionWithName:@"triangle"];
        fragment->fn = [library newFunctionWithName:i == 0 ? @"red" : i == 1 ? @"image" : @"typed"];
        fragment->stage = 1; fragment->rf.color_known = fragment->rf.color_mask = 1;
        if (i) {
            fragment->rf.nb = 1;
            fragment->rf.b[0] = (nvn_rbind){.kind = i == 1 ? NVN_RB_TEX : NVN_RB_TBUF, .reg = 0, .aid = 0,
                .dtt = i == 1 ? MTLTextureType2D : MTLTextureTypeTextureBuffer, .dpf = 1};
            fragment->ae = [fragment->fn newArgumentEncoderWithBufferIndex:0];
            fragment->arg_len = fragment->ae.encodedLength;
        }
        programs[i]->sh[0] = vertex; programs[i]->sh[1] = fragment;
        guests[i].serial = nvn_new_serial(); serial_put(guests[i].serial, programs[i]);
    }
    size_t page = (size_t)getpagesize(); uint8_t *memory;
    if (posix_memalign((void **)&memory, page, 2 * page)) abort();
    memset(memory, 0, 2 * page); mtl_pool_add(memory, 2 * page);
    uint64_t address = (uint64_t)(uintptr_t)memory;
    void *object; uint64_t offset;
    if (!mtl_addr(address, 2 * page, &object, &offset)) abort();
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)object;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:descriptor];
    pool_t pool = {.mem = memory, .size = 2 * page};
    tex_t output = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial(), .pool = &pool};
    output.d = (nvn_texdesc){.width = 8, .height = 8, .depth = 1, .levels = 1, .samples = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8};
    serial_put(output.serial, target);
    int old_batch = vis_batch_on(), old_textures = vis_textures_on(), old_passes = pass_batch_on(), old_inline = discard_inline_on();
    g_nvn_query_batch = 1; g_nvn_pass_batch = g_nvn_discard_inline = 0;
    p_draw triangle = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    for (unsigned kind = 1; kind <= 2; kind++) {
        MTLTextureDescriptor *input_descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:4 height:1 mipmapped:NO];
        input_descriptor.textureType = kind == 1 ? MTLTextureType2D : MTLTextureTypeTextureBuffer;
        input_descriptor.storageMode = MTLStorageModeShared; input_descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> input = [buffer newTextureWithDescriptor:input_descriptor offset:offset + page bytesPerRow:256];
        if (!input) abort();
        tex_t sampled = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial(), .pool = &pool, .mem = memory + page};
        sampled.d = (nvn_texdesc){.width = 4, .height = 1, .depth = 1, .levels = 1, .samples = 1,
            .target = kind == 1 ? NVN_TEXTURE_TARGET_2D : NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_R32UI};
        sampled.L.size = 256; serial_put(sampled.serial, input); mtl_reg_tex(702, &sampled, NULL);
        for (unsigned enabled = 0; enabled <= 1; enabled++) {
            memset(memory, 0, 2 * page); state_reset(); memset(&R, 0, sizeof R);
            g_nvn_query_textures = enabled; S.prog = &guests[kind]; S.tex[1][0] = 702;
            S.ds = (ds_st){.df = NVN_DEPTH_FUNC_ALWAYS};
            R.rt = (p_set_rt){.n = 1, .color = {&output}}; R.has_rt = R.clear_c[0] = 1;
            R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
            mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0);
            draw(&triangle, NULL); id<MTLRenderCommandEncoder> first = g_enc;
            visibility_command(OP_REPORT_COUNTER, address);
            EXPECT("fixed-texture query follows its switch", enabled, g_vis_pending_n);
            EXPECT("fixed-texture report keeps its encoder only when enabled", enabled, g_enc == first);
            if (enabled) {
                EXPECT("disjoint fixed-texture read can stay in the pass", 1, render_draw_readonly(programs[kind], &triangle, NULL));
                p_bind_handles bindings = {.stage = 1, .n = 1, .h = {702}};
                EXPECT("ordinary texture binding keeps pending reports", 1, vis_pending_keeps(OP_BIND_TEX, &bindings));
                bindings.defer = 1;
                EXPECT("CPU-fetched texture handles keep their boundary", 0, vis_pending_keeps(OP_BIND_TEX, &bindings));
                tex_t saved = g_treg[702].t;
                g_treg[702].t.L.size = 0;
                EXPECT("unknown texture storage ends a pending batch", 0, vis_texture_safe(1, 0));
                g_treg[702].t = saved; pool.flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL;
                EXPECT("virtual texture mappings keep their boundary", 0, vis_texture_safe(1, 0));
                pool.flags = 0;
                g_treg[702].t.mem = memory + 16; g_treg[702].t.L.size = 1;
                EXPECT("texture starting after a report is disjoint", 1, vis_texture_safe(1, 0));
                g_treg[702].t.mem = memory + 15;
                EXPECT("one-byte texture overlap ends a pending batch", 0, vis_texture_safe(1, 0));
                g_treg[702].t = saved;
            }
            draw(&triangle, NULL); visibility_command(OP_REPORT_COUNTER, address + 16);
            EXPECT("textured batch completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
            EXPECT("textured batch GPU succeeds", 0, mtl_last_commit_error());
            EXPECT("first textured query counts all samples", 64, *(uint64_t *)memory);
            EXPECT("second textured query accumulates samples", 128, *(uint64_t *)(memory + 16));
        }
        memset(memory, 0, 2 * page); g_nvn_query_textures = 1; S.prog = &guests[0];
        mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0); draw(&triangle, NULL);
        visibility_command(OP_REPORT_COUNTER, address + page);
        EXPECT("texture-alias producer initially remains pending", 1, g_vis_pending_n);
        S.prog = &guests[kind]; S.tex[1][0] = 702; draw(&triangle, NULL);
        EXPECT("texture alias drains the query before the next draw", 0, g_vis_pending_n);
        EXPECT("texture-alias submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("texture-alias GPU succeeds", 0, mtl_last_commit_error());
        uint8_t pixels[256]; [target getBytes:pixels bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
        const uint8_t green[4] = {0, 255, 0, 255};
        for (unsigned i = 0; i < 64; i++) EXPECT("texture shader observes its preceding query", 0, memcmp(pixels + 4 * i, green, 4));
        mtl_reg_tex(702, NULL, NULL); serial_del(sampled.serial);
    }
    g_nvn_query_batch = old_batch; g_nvn_query_textures = old_textures; g_nvn_pass_batch = old_passes; g_nvn_discard_inline = old_inline;
    vis_clear(); g_vis_counting = g_vis_on = 0; state_reset(); memset(&R, 0, sizeof R);
    serial_del(output.serial); for (unsigned i = 0; i < 3; i++) serial_del(guests[i].serial);
    mtl_pool_del(memory); mtl_drain(); buffer = nil; free(memory);
}

static void visibility_attachmentless_queries(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 triangle(uint v [[vertex_id]]) { return float4(v==1?3.0:-1.0,v==2?3.0:-1.0,0.5,1); }\n"
        "struct Arguments { texture2d<uint> image [[id(0)]]; constant uint *limit [[id(1)]]; };\n"
        "fragment uint4 count(constant Arguments &a [[buffer(0)]], float4 p [[position]]) {"
        "if(a.image.read(uint2(p.xy)).x >= *a.limit) discard_fragment(); return uint4(77); }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvProg *program = fixture_program(); NvShader *vertex = [NvShader new], *fragment = [NvShader new];
    vertex->fn = [library newFunctionWithName:@"triangle"];
    fragment->fn = [library newFunctionWithName:@"count"]; fragment->stage = 1;
    fragment->rf.nb = 2; fragment->rf.color_known = fragment->rf.color_mask = fragment->rf.may_discard = 1;
    fragment->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .aid = 0, .dtt = MTLTextureType2D, .dpf = NVN_DPF_UINT};
    fragment->rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 1};
    fragment->ae = [fragment->fn newArgumentEncoderWithBufferIndex:0]; fragment->arg_len = fragment->ae.encodedLength;
    program->sh[0] = vertex; program->sh[1] = fragment;
    static prog_t guest; guest.serial = nvn_new_serial(); serial_put(guest.serial, program);
    size_t page = (size_t)getpagesize(); uint8_t *memory;
    if (posix_memalign((void **)&memory, page, 2 * page)) abort();
    memset(memory, 0, 2 * page); mtl_pool_add(memory, 2 * page);
    const uint32_t limits[] = {32, 16, 48, 64, 100};
    for (unsigned i = 0; i < 5; i++) memcpy(memory + 16 * i, &limits[i], 4);
    uint64_t address = (uint64_t)(uintptr_t)memory;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:8 height:8 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:descriptor];
    pool_t pool = {.mem = memory, .size = 2 * page};
    tex_t texture = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial(), .pool = &pool, .mem = memory + page};
    texture.d = (nvn_texdesc){.width = 8, .height = 8, .depth = 1, .levels = 1, .samples = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32UI};
    texture.L.size = 256; serial_put(texture.serial, target); mtl_reg_tex(703, &texture, NULL);
    uint32_t source[64], observed[64]; for (unsigned i = 0; i < 64; i++) source[i] = i;
    int old_batch = vis_batch_on(), old_textures = vis_textures_on(); g_nvn_query_batch = 1;
    p_draw triangle = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
    for (unsigned mode = 1; mode <= 2; mode++) {
        [target replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:source bytesPerRow:32];
        state_reset(); memset(&R, 0, sizeof R); S.prog = &guest; S.tex[1][0] = 703;
        S.chmask.mask = 0; S.ds = (ds_st){.df = NVN_DEPTH_FUNC_ALWAYS}; g_nvn_query_textures = mode;
        R.rt = (p_set_rt){.n = 1, .color = {&texture}}; R.has_rt = 1;
        uint64_t passes = g_stat_pass; mtl_sub_begin();
        for (unsigned i = 0; i < 3; i++) {
            if (i) {
                uint8_t commands[sizeof(p_set_rt) + 16] __attribute__((aligned(8)));
                put_hdr(commands, OP_SET_RT, sizeof(p_set_rt) + 8); memcpy(commands + 8, &R.rt, sizeof R.rt);
                put_hdr(commands + sizeof(p_set_rt) + 8, OP_END, 8); mtl_exec(commands);
            }
            visibility_command(OP_RESET_COUNTER, 0);
            p_bind_buf binding = {.stage = 1, .index = 0, .addr = address + 16 * i, .size = 16};
            state_op(OP_BIND_UBO, &binding);
            draw(&triangle, NULL); visibility_command(OP_REPORT_COUNTER, address + 256 + 16 * i);
        }
        EXPECT("self-sampled queries merge only without attachments", mode == 2 ? 1 : 3, g_stat_pass - passes);
        EXPECT("query pass records attachment-free mode", mode == 2, g_enc_mode == 5);
        EXPECT("self-sampled queries complete", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("self-sampled query GPU succeeds", 0, mtl_last_commit_error());
        for (unsigned i = 0; i < 3; i++) EXPECT("attachment-free query preserves fragment discard", limits[i], *(uint64_t *)(memory + 256 + 16 * i));
        [target getBytes:observed bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
        EXPECT("query-only draws preserve their sampled attachment", 0, memcmp(source, observed, sizeof source));
    }
    g_nvn_query_textures = 2; mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0);
    EXPECT("read-only query is eligible", 1, vis_attachmentless_safe(program));
    S.ds.dt = 1; EXPECT("depth-tested query retains attachments", 0, vis_attachmentless_safe(program)); S.ds.dt = 0;
    S.chmask.mask = 15; EXPECT("color-writing query retains attachments", 0, vis_attachmentless_safe(program)); S.chmask.mask = 0;
    R.clear_c[0] = 1; EXPECT("pending clear retains attachments", 0, vis_attachmentless_safe(program)); R.clear_c[0] = 0;
    fragment->rf.writes_smask = 1; EXPECT("sample-mask output retains attachments", 0, vis_attachmentless_safe(program)); fragment->rf.writes_smask = 0;
    S.ubo[1][0] = (p_bind_buf){.addr = address + 48, .size = 16}; draw(&triangle, NULL);
    visibility_command(OP_REPORT_COUNTER, address + 320);
    S.chmask.mask = 15; draw(&triangle, NULL);
    EXPECT("following write restores a normal pass", 0, g_enc_mode == 5);
    EXPECT("query then color write completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("query before a color write stays intact", 64, *(uint64_t *)(memory + 320));
    [target getBytes:observed bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    for (unsigned i = 0; i < 64; i++) EXPECT("following color write reaches the original target", 77, observed[i]);
    S.chmask.mask = 0; S.ubo[1][0] = (p_bind_buf){.addr = address + 64, .size = 16};
    mtl_sub_begin(); visibility_command(OP_RESET_COUNTER, 0); draw(&triangle, NULL);
    visibility_command(OP_REPORT_COUNTER, address + 336);
    uint8_t clear[sizeof(p_clear_color) + 16] __attribute__((aligned(8)));
    p_clear_color value = {.index = 0, .mask = 15};
    put_hdr(clear, OP_CLEAR_COLOR, sizeof(p_clear_color) + 8); memcpy(clear + 8, &value, sizeof value);
    put_hdr(clear + sizeof(p_clear_color) + 8, OP_END, 8); mtl_exec(clear);
    EXPECT("clear ends an attachment-free query pass", 0, g_enc != nil);
    EXPECT("query then clear completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("query before a clear stays intact", 64, *(uint64_t *)(memory + 336));
    [target getBytes:observed bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    for (unsigned i = 0; i < 64; i++) EXPECT("following clear reaches the original target", 0, observed[i]);
    EXPECT("attachment-free query boundary GPU succeeds", 0, mtl_last_commit_error());
    g_nvn_query_batch = old_batch; g_nvn_query_textures = old_textures;
    vis_clear(); g_vis_counting = g_vis_on = 0; state_reset(); memset(&R, 0, sizeof R);
    mtl_reg_tex(703, NULL, NULL); serial_del(texture.serial); serial_del(guest.serial);
    mtl_pool_del(memory); mtl_drain(); free(memory);
}
