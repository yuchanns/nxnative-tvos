#include <mach/mach_vm.h>

static void *discard_foreign_thread(void *command_buffer) {
    discard_end_for((__bridge id<MTLCommandBuffer>)command_buffer);
    return NULL;
}

static void discard_batch_dependencies(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { device uint *output [[id(0)]]; const device uint *input [[id(1)]]; };\n"
        "vertex float4 ordinary(constant Args &a [[buffer(0)]], uint v [[vertex_id]]) {"
        "a.output[v] = a.input[v] + 1; return float4(0,0,0,1); }\n"
        "vertex void discarded(constant Args &a [[buffer(0)]], uint v [[vertex_id]]) { a.output[v] = a.input[v] + 1; }\n"
        "struct Read { const device uint *input [[id(0)]]; };\n"
        "vertex float4 consume(constant Read &a [[buffer(0)]], uint v [[vertex_id]]) {"
        "return *a.input == 160 ? float4(v == 1 ? 3.0 : -1.0, v == 2 ? 3.0 : -1.0, 0.5, 1.0) : float4(4,4,4,1); }\n"
        "fragment float4 green() { return float4(0,1,0,1); }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvShader *vertex = [NvShader new]; vertex->fn = [library newFunctionWithName:@"ordinary"];
    vertex->discard_fn = [library newFunctionWithName:@"discarded"];
    vertex->rf.nb = 2;
    vertex->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 0, .rw = 1};
    vertex->rf.b[1] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 1, .aid = 1};
    vertex->ae = [vertex->fn newArgumentEncoderWithBufferIndex:0]; vertex->arg_len = vertex->ae.encodedLength;
    NvProg *producer = fixture_program(); producer->sh[0] = vertex;
    NvProg *consumer = fixture_program(); consumer->sh[0] = [NvShader new]; consumer->sh[1] = [NvShader new];
    consumer->sh[0]->fn = [library newFunctionWithName:@"consume"];
    consumer->sh[0]->rf.nb = 1;
    consumer->sh[0]->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 0};
    consumer->sh[0]->ae = [consumer->sh[0]->fn newArgumentEncoderWithBufferIndex:0];
    consumer->sh[0]->arg_len = consumer->sh[0]->ae.encodedLength;
    consumer->sh[1]->fn = [library newFunctionWithName:@"green"]; consumer->sh[1]->stage = 1;
    consumer->sh[1]->rf.color_known = consumer->sh[1]->rf.color_mask = 1;
    static prog_t guests[2]; guests[0].serial = nvn_new_serial(); guests[1].serial = nvn_new_serial();
    serial_put(guests[0].serial, producer); serial_put(guests[1].serial, consumer);

    mach_vm_size_t page = (mach_vm_size_t)getpagesize(); mach_vm_address_t memory = 0, alias = 0;
    vm_prot_t current, maximum;
    if (mach_vm_allocate(mach_task_self(), &memory, 2 * page, VM_FLAGS_ANYWHERE) ||
        mach_vm_remap(mach_task_self(), &alias, page, 0, VM_FLAGS_ANYWHERE, mach_task_self(), memory, FALSE,
                      &current, &maximum, VM_INHERIT_NONE)) abort();
    mtl_pool_add((void *)(uintptr_t)memory, 2 * page); mtl_pool_add((void *)(uintptr_t)alias, page);
    void *first_buffer, *alias_buffer; uint64_t offset;
    mtl_addr(memory, page, &first_buffer, &offset); mtl_addr(alias, page, &alias_buffer, &offset);
    EXPECT("discard dependency test uses distinct Metal alias wrappers", 1, first_buffer != alias_buffer);
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:descriptor];
    tex_t texture = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial()};
    texture.d = (nvn_texdesc){.width = 8, .height = 8, .depth = 1, .levels = 1, .samples = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8};
    texture.pool = calloc(1, sizeof *texture.pool); serial_put(texture.serial, target);
    discard_batch_on(); int previous = g_nvn_discard_batch;
    for (unsigned mode = 0; mode < 2; mode++) {
        g_nvn_discard_batch = (int)mode; memset((void *)(uintptr_t)memory, 0, 2 * page);
        state_reset(); memset(&R, 0, sizeof R); S.prog = &guests[0]; S.rast_discard = 1;
        p_draw points = {.prim = 0, .count = (int)(page / 4), .instances = 1};
        mtl_sub_begin(); uint64_t passes = g_discard_passes, reuses = g_discard_reuses;
        for (unsigned i = 0; i < 128; i++) {
            S.ssbo[0][0] = (p_bind_buf){.addr = i & 1 ? alias : memory, .size = page};
            S.ssbo[0][1] = (p_bind_buf){.addr = i & 1 ? memory : alias, .size = page};
            draw(&points, NULL);
            if (mode && i == 63) {
                id<MTLRenderCommandEncoder> owned = g_discard_encoder; pthread_t thread;
                if (pthread_create(&thread, NULL, discard_foreign_thread, (__bridge void *)g_cb)) abort();
                pthread_join(thread, NULL);
                EXPECT("another thread cannot end the owned discard encoder", 1, owned == g_discard_encoder);
            }
        }
        EXPECT("discard batch uses one encoder when enabled", mode ? 1 : 128, g_discard_passes - passes);
        EXPECT("discard batch retains all dependent draws", mode ? 127 : 0, g_discard_reuses - reuses);
        mtl_sub_copy(alias, memory + page, page);
        EXPECT("GPU copy closes the preceding discard encoder", 1, g_discard_encoder == nil);
        for (unsigned i = 0; i < 32; i++) draw(&points, NULL);
        S.prog = &guests[1]; S.rast_discard = 0;
        S.ssbo[0][0] = (p_bind_buf){.addr = memory, .size = page}; S.ds = (ds_st){.df = NVN_DEPTH_FUNC_ALWAYS};
        R.rt = (p_set_rt){.n = 1, .color = {&texture}}; R.has_rt = R.clear_c[0] = 1;
        R.cc[0] = MTLClearColorMake(1, 0, 0, 1);
        p_draw triangle = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1};
        draw(&triangle, NULL);
        EXPECT("normal rendering closes the preceding discard encoder", 1, g_discard_encoder == nil);
        S.prog = &guests[0]; S.rast_discard = 1;
        S.ssbo[0][0] = (p_bind_buf){.addr = memory, .size = page}; S.ssbo[0][1] = (p_bind_buf){.addr = alias, .size = page};
        for (unsigned i = 0; i < 16; i++) draw(&points, NULL);
        uint64_t fence = mtl_sub_commit();
        EXPECT("submission closes its final discard encoder", 1, g_discard_encoder == nil);
        EXPECT("discard dependency GPU completes", 1, mtl_wait(fence, 5000000000ull));
        EXPECT("discard dependency GPU succeeds", 0, mtl_last_commit_error());
        for (unsigned i = 0; i < page / 4; i++) {
            EXPECT("each dependent draw observes the previous alias write", 176, ((uint32_t *)(uintptr_t)memory)[i]);
            EXPECT("copy observes all preceding vertex writes", 128, ((uint32_t *)(uintptr_t)(memory + page))[i]);
        }
        uint8_t pixels[256]; [target getBytes:pixels bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
        const uint8_t green[4] = {0, 255, 0, 255};
        for (unsigned i = 0; i < 64; i++) EXPECT("next render pass reads the latest discard output", 0, memcmp(pixels + 4 * i, green, 4));
    }
    g_nvn_discard_batch = previous; mtl_pool_del((void *)(uintptr_t)alias); mtl_pool_del((void *)(uintptr_t)memory);
    mtl_drain(); mach_vm_deallocate(mach_task_self(), alias, page); mach_vm_deallocate(mach_task_self(), memory, 2 * page);
    serial_del(guests[0].serial); serial_del(guests[1].serial); serial_del(texture.serial); free(texture.pool);
    state_reset(); memset(&R, 0, sizeof R);
}
