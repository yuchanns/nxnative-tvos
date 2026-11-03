// Idle GPU textures retain their exact contents in unlinked temporary files.
// Snapshot bounded chunks asynchronously; a use cancels an incomplete copy.
// Keep objects and views stable, and discard contents only after a full snapshot.
#include "nvn_texture_snapshot.h"

// One unlinked file avoids keeping a descriptor open for every idle texture.
// Freed ranges are reused; storage failures leave the original texture intact.
typedef struct rt_free_span { uint64_t offset, bytes; struct rt_free_span *next; } rt_free_span;
static pthread_mutex_t g_rt_file_mu = PTHREAD_MUTEX_INITIALIZER;
static rt_free_span *g_rt_free;
static int g_rt_file = -1;
static uint64_t g_rt_file_end;
static uint64_t rt_cache_span(uint64_t bytes) { return (bytes + 4095u) & ~4095ull; }
static int rt_cache_file_allocate(uint64_t bytes, uint64_t *offset) {
    if (!bytes || bytes > ((uint64_t)INT64_MAX & ~4095ull)) return -1;
    uint64_t span = rt_cache_span(bytes);
    pthread_mutex_lock(&g_rt_file_mu);
    if (g_rt_file < 0) {
        NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"nxnative-textures-XXXXXX"];
        char *name = strdup(path.fileSystemRepresentation);
        if (name) {
            g_rt_file = mkstemp(name);
            if (g_rt_file >= 0 && unlink(name)) { close(g_rt_file); g_rt_file = -1; }
            free(name);
        }
    }
    int fd = g_rt_file;
    if (fd >= 0) {
        rt_free_span **link = &g_rt_free;
        while (*link && (*link)->bytes < span) link = &(*link)->next;
        if (*link) {
            rt_free_span *found = *link;
            *offset = found->offset; found->offset += span; found->bytes -= span;
            if (!found->bytes) { *link = found->next; free(found); }
        } else if (span <= (uint64_t)INT64_MAX - g_rt_file_end && !ftruncate(fd, (off_t)(g_rt_file_end + span))) {
            *offset = g_rt_file_end; g_rt_file_end += span;
        } else fd = -1;
    }
    pthread_mutex_unlock(&g_rt_file_mu);
    return fd;
}
static void rt_cache_file_release(uint64_t offset, uint64_t bytes) {
    if (!bytes) return;
    rt_free_span *span = malloc(sizeof *span);
    if (!span) return;   // Losing track of this range must not allow overlapping allocations.
    *span = (rt_free_span){offset, rt_cache_span(bytes), NULL};
    pthread_mutex_lock(&g_rt_file_mu);
    rt_free_span **link = &g_rt_free;
    while (*link && (*link)->offset < offset) link = &(*link)->next;
    span->next = *link; *link = span;
    for (rt_free_span *current = g_rt_free; current && current->next;) {
        rt_free_span *next = current->next;
        if (current->offset + current->bytes == next->offset) {
            current->bytes += next->bytes; current->next = next->next; free(next);
        } else current = next;
    }
    pthread_mutex_unlock(&g_rt_file_mu);
}

// Both the snapshot writer and the GPU callback own this budget reservation.
// Completed or canceled copies remain charged until both have released them.
static atomic_uint_fast64_t g_rt_staging_bytes;
@interface NvRtStaging : NSObject {
@public id<MTLBuffer> buffer; uint64_t bytes;
} @end
@implementation NvRtStaging
- (void)dealloc { atomic_fetch_sub(&g_rt_staging_bytes, bytes); }
@end

@interface NvRtCache : NSObject {
@public
    __weak id<MTLTexture> texture;
    uint64_t serial, used_fence, used_frame, file_bytes, file_offset;
    int fd, spilled, excluded, pending;
    nvn_snapshot_layout layout;
    nvn_snapshot_cursor cursor, next_cursor;
    nvn_snapshot_chunk pending_chunk;
    id<MTLBuffer> staging;
    NvRtStaging *staging_owner;
    id<MTLCommandBuffer> copy;
} @end
@implementation NvRtCache
- (instancetype)init { if ((self = [super init])) fd = -1; return self; }
- (void)dealloc { if (fd >= 0) rt_cache_file_release(file_offset, file_bytes); }
@end

static NSMapTable<id<MTLTexture>, NvRtCache *> *g_rt_cache;
static pthread_mutex_t g_rt_cache_mu = PTHREAD_MUTEX_INITIALIZER;
enum { RT_CACHE_PENDING_LIMIT = 16 };
static NvRtCache *g_rt_pending[RT_CACHE_PENDING_LIMIT];
static unsigned g_rt_pending_n;
static uint64_t g_rt_spilled_bytes, g_rt_restored_bytes, g_rt_spills, g_rt_restores, g_rt_cancels;
static atomic_ulong g_rt_pressure_flags = DISPATCH_MEMORYPRESSURE_NORMAL;
static atomic_uint_fast64_t g_rt_pressure_events;
static dispatch_source_t g_rt_pressure_source;
static int g_rt_reclaiming;
static const uint64_t RT_CACHE_START_HEADROOM = 896ull << 20;
static const uint64_t RT_CACHE_STOP_HEADROOM = 1024ull << 20;
enum { RT_CACHE_IDLE_FRAMES = 300 };

static void rt_cache_memory_pressure(unsigned long flags) {
    atomic_store(&g_rt_pressure_flags, flags);
    if (flags & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL))
        atomic_fetch_add(&g_rt_pressure_events, 1);
}

static int rt_cache_on(void) {
    static int enabled; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("DM_NVN_RT_SPILL"); enabled = e ? atoi(e) != 0 : TARGET_OS_IPHONE;
        if (!enabled) return;
        // The callback only publishes pressure. GPU work stays on the replay
        // thread, where fences and later texture uses can order it safely.
        g_rt_pressure_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
            DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
            dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
        if (g_rt_pressure_source) {
            dispatch_source_set_event_handler(g_rt_pressure_source, ^{
                rt_cache_memory_pressure(dispatch_source_get_data(g_rt_pressure_source));
            });
            dispatch_resume(g_rt_pressure_source);
        }
    });
    return enabled;
}

static NSUInteger rt_cache_pixel_bytes(MTLPixelFormat format) {
    switch (format) {
        case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm:
        case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: return 1;
        case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm:
        case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
        case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm:
        case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
        case MTLPixelFormatR16Float: return 2;
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatRGBA8Uint:
        case MTLPixelFormatRGBA8Sint: case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB: case MTLPixelFormatRG16Unorm:
        case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Uint:
        case MTLPixelFormatRG16Sint: case MTLPixelFormatRG16Float:
        case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
        case MTLPixelFormatR32Float: case MTLPixelFormatRGB10A2Unorm:
        case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG11B10Float:
        case MTLPixelFormatRGB9E5Float: case MTLPixelFormatBGR10A2Unorm: return 4;
        case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm:
        case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
        case MTLPixelFormatRGBA16Float: case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float: return 8;
        case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint:
        case MTLPixelFormatRGBA32Float: return 16;
        case MTLPixelFormatDepth16Unorm: return 2;
        case MTLPixelFormatDepth32Float: return 4;
        case MTLPixelFormatStencil8: return 1;
        default: return 0;
    }
}

static int rt_cache_layout(id<MTLTexture> texture, nvn_snapshot_layout *layout) {
    *layout = (nvn_snapshot_layout){(uint32_t)texture.width, (uint32_t)texture.height,
        (uint32_t)texture.mipmapLevelCount, (uint32_t)texture.arrayLength, 1, {{1, 1, 0, 0}}, 0};
    if (texture.textureType == MTLTextureTypeCube || texture.textureType == MTLTextureTypeCubeArray) layout->layers *= 6;
    if (texture.textureType == MTLTextureType3D) { layout->layers = 1; layout->depth = (uint32_t)texture.depth; }
    MTLPixelFormat format = texture.pixelFormat;
    if (format == MTLPixelFormatDepth32Float_Stencil8) {
        layout->planes = 2;
        layout->plane[0] = (nvn_snapshot_plane){1, 1, 4, MTLBlitOptionDepthFromDepthStencil};
        layout->plane[1] = (nvn_snapshot_plane){1, 1, 1, MTLBlitOptionStencilFromDepthStencil};
    } else if (format == MTLPixelFormatASTC_4x4_LDR || format == MTLPixelFormatASTC_4x4_sRGB) {
        layout->plane[0] = (nvn_snapshot_plane){4, 4, 16, 0};
    } else if (format == MTLPixelFormatASTC_6x6_LDR || format == MTLPixelFormatASTC_6x6_sRGB) {
        layout->plane[0] = (nvn_snapshot_plane){6, 6, 16, 0};
    } else layout->plane[0].bytes = (uint32_t)rt_cache_pixel_bytes(format);
    return nvn_snapshot_size(layout) != 0;
}

static id<MTLTexture> rt_cache_base(id<MTLTexture> texture) {
    id<MTLTexture> base = texture;
    while (base.parentTexture) base = base.parentTexture;
    return base;
}

static void rt_cache_track(id<MTLTexture> texture, const tex_t *source) {
    nvn_snapshot_layout layout;
    if (!rt_cache_on() || !on_sub_thread() || !texture || !source ||
        source->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_VIDEO_DECODE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE) ||
        source->has_layout || texture.parentTexture || texture.buffer || texture.heap || texture.iosurface || texture.isSparse ||
        texture.framebufferOnly || texture.storageMode != MTLStorageModePrivate || texture.sampleCount != 1 ||
        (texture.textureType != MTLTextureType1D && texture.textureType != MTLTextureType1DArray &&
         texture.textureType != MTLTextureType2D && texture.textureType != MTLTextureType2DArray &&
         texture.textureType != MTLTextureTypeCube && texture.textureType != MTLTextureTypeCubeArray &&
         texture.textureType != MTLTextureType3D) ||
        !rt_cache_layout(texture, &layout)) return;
    pthread_mutex_lock(&g_rt_cache_mu);
    if (!g_rt_cache) g_rt_cache = [NSMapTable weakToStrongObjectsMapTable];
    if (![g_rt_cache objectForKey:texture]) {
        NvRtCache *entry = [NvRtCache new];
        entry->texture = texture; entry->serial = source->serial; entry->layout = layout;
        entry->used_fence = g_fence + 1; entry->used_frame = atomic_load(&g_cur_frame);
        [g_rt_cache setObject:entry forKey:texture];
    }
    pthread_mutex_unlock(&g_rt_cache_mu);
}

static int rt_cache_io(int fd, void *bytes, size_t size, uint64_t offset, int restore) {
    size_t done = 0;
    while (done < size) {
        ssize_t count = restore ? pread(fd, (uint8_t *)bytes + done, size - done, (off_t)(offset + done))
                                : pwrite(fd, (uint8_t *)bytes + done, size - done, (off_t)(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        done += (size_t)count;
    }
    return 1;
}

// A canceled asynchronous read can still be on the GPU. Its staging allocation
// remains resident until that command completes, even after the entry is reused.
static void rt_cache_pending_remove(NvRtCache *entry) {
    for (unsigned i = 0; i < g_rt_pending_n; i++) if (g_rt_pending[i] == entry) {
        g_rt_pending[i] = g_rt_pending[--g_rt_pending_n]; g_rt_pending[g_rt_pending_n] = nil;
        break;
    }
    entry->pending = 0;
}
static void rt_cache_cancel(NvRtCache *entry) {
    // A submitted copy owns cleanup through its pre-registered callback.
    // Only a setup failure leaves the staging allocation without that owner.
    if (entry->staging && !entry->copy) res_remove(entry->staging);
    entry->staging = nil; entry->staging_owner = nil; entry->copy = nil;
    if (entry->fd >= 0) rt_cache_file_release(entry->file_offset, entry->file_bytes);
    entry->fd = -1; entry->file_bytes = 0; entry->cursor = (nvn_snapshot_cursor){0};
    rt_cache_pending_remove(entry);
}

static void rt_cache_encode(id<MTLBlitCommandEncoder> blit, id<MTLTexture> texture,
        id<MTLBuffer> staging, nvn_snapshot_chunk chunk, int restore) {
    MTLOrigin origin = MTLOriginMake(0, chunk.y, chunk.z);
    MTLSize size = MTLSizeMake(chunk.width, chunk.height, 1);
    NSUInteger image = texture.textureType == MTLTextureType3D ? (NSUInteger)chunk.bytes : 0;
    if (restore)
        [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:chunk.row_bytes sourceBytesPerImage:image sourceSize:size
                  toTexture:texture destinationSlice:chunk.slice destinationLevel:chunk.level destinationOrigin:origin options:chunk.options];
    else
        [blit copyFromTexture:texture sourceSlice:chunk.slice sourceLevel:chunk.level sourceOrigin:origin sourceSize:size
                    toBuffer:staging destinationOffset:0 destinationBytesPerRow:chunk.row_bytes destinationBytesPerImage:image options:chunk.options];
}

// Residency removal can autorelease the staging buffer. Drain it after this
// synchronous restore, rather than accumulating one buffer per submission use.
static int rt_cache_restore(NvRtCache *entry, id<MTLTexture> texture) { @autoreleasepool {
    if (entry->fd < 0 || entry->file_bytes != nvn_snapshot_size(&entry->layout)) return 0;
    id<MTLBuffer> staging = [g_dev newBufferWithLength:NVN_SNAPSHOT_CHUNK options:MTLResourceStorageModeShared];
    if (!staging) return 0;
    res_add(staging); res_commit();
    int ok = 1;
    nvn_snapshot_cursor cursor = {0}; nvn_snapshot_chunk chunk;
    while (ok && nvn_snapshot_next(&entry->layout, &cursor, &chunk)) { @autoreleasepool {
        if (!rt_cache_io(entry->fd, staging.contents, chunk.bytes, entry->file_offset + chunk.offset, 1)) { ok = 0; break; }
        id<MTLCommandBuffer> cb = command_buffer(g_q);
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        if (!cb || !blit) { ok = 0; break; }
        rt_cache_encode(blit, texture, staging, chunk, 1);
        [blit endEncoding]; gpu_tag(cb); [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { log_gpu_encoders(cb); ok = 0; }
    } }
    res_remove(staging); res_commit();
    return ok && cursor.offset == entry->file_bytes;
} }

// Called even when descriptor/argument caches hit. Partial snapshots are canceled
// before any new read or write; completed snapshots restore every plane first.
static void rt_cache_use(id<MTLTexture> texture) {
    if (!rt_cache_on() || !texture) return;
    id<MTLTexture> base = rt_cache_base(texture);
    pthread_mutex_lock(&g_rt_cache_mu);
    NvRtCache *entry = [g_rt_cache objectForKey:base];
    if (entry) {
        if (entry->spilled) {
            [base setPurgeableState:MTLPurgeableStateNonVolatile];
            res_add(base); res_commit();
            if (!rt_cache_restore(entry, base)) MDIE("texture snapshot restore failed: serial %llu", (unsigned long long)entry->serial);
            entry->spilled = 0; rt_cache_file_release(entry->file_offset, entry->file_bytes); entry->fd = -1; entry->file_bytes = 0;
            g_rt_restored_bytes += base.allocatedSize; g_rt_restores++;
        } else if (entry->fd >= 0) { rt_cache_cancel(entry); g_rt_cancels++; }
        if (on_sub_thread()) { entry->used_fence = g_fence + 1; entry->used_frame = atomic_load(&g_cur_frame); }
        else entry->excluded = 1;
    }
    pthread_mutex_unlock(&g_rt_cache_mu);
}

// Called under g_rt_cache_mu, at a submission boundary. Returning without a GPU
// wait is essential: memory recovery must not add another long transition stall.
static int rt_cache_snapshot_step(NvRtCache *entry, id<MTLTexture> texture) {
    if (entry->copy) {
        MTLCommandBufferStatus status = entry->copy.status;
        if (status < MTLCommandBufferStatusCompleted) return 0;
        if (status != MTLCommandBufferStatusCompleted ||
            !rt_cache_io(entry->fd, entry->staging.contents, entry->pending_chunk.bytes, entry->file_offset + entry->pending_chunk.offset, 0)) return -1;
        entry->cursor = entry->next_cursor;
        entry->staging = nil; entry->staging_owner = nil; entry->copy = nil;
        if (entry->cursor.offset == entry->file_bytes) return 1;
    }
    if (entry->fd < 0) {
        uint64_t size = nvn_snapshot_size(&entry->layout);
        if (!size) return -1;
        int fd = rt_cache_file_allocate(size, &entry->file_offset);
        if (fd < 0) return -1;
        entry->fd = fd; entry->file_bytes = size; entry->cursor = (nvn_snapshot_cursor){0};
    }
    entry->next_cursor = entry->cursor;
    if (!nvn_snapshot_next(&entry->layout, &entry->next_cursor, &entry->pending_chunk)) return -1;
    uint64_t page = (uint64_t)getpagesize();
    uint64_t needed = (entry->pending_chunk.bytes + page - 1) & ~(page - 1);
    if (needed > NVN_SNAPSHOT_CHUNK - atomic_load(&g_rt_staging_bytes)) return 0;
    entry->staging = [g_dev newBufferWithLength:entry->pending_chunk.bytes options:MTLResourceStorageModeShared];
    if (!entry->staging) return -1;
    uint64_t retained = MAX(entry->staging.length, entry->staging.allocatedSize);
    if (retained > NVN_SNAPSHOT_CHUNK) { entry->staging = nil; return -1; }
    if (retained > NVN_SNAPSHOT_CHUNK - atomic_load(&g_rt_staging_bytes)) { entry->staging = nil; return 0; }
    entry->staging_owner = [NvRtStaging new];
    entry->staging_owner->buffer = entry->staging;
    entry->staging_owner->bytes = retained;
    atomic_fetch_add(&g_rt_staging_bytes, retained);
    res_add(entry->staging); res_commit();
    // Same replay queue and commit gates order this read before a later reuse,
    // including reuse through another Metal resource on aliased guest memory.
    id<MTLCommandBuffer> cb = command_buffer(g_replay_q);
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (!cb || !blit) return -1;
    rt_cache_encode(blit, texture, entry->staging, entry->pending_chunk, 0);
    [blit endEncoding]; gpu_tag(cb); entry->copy = cb;
    // Metal requires handlers before commit. Capture this buffer, not the entry:
    // cancellation may reuse the entry before this command's callback runs.
    NvRtStaging *owner = entry->staging_owner;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done; res_remove(owner->buffer);
    }];
    replay_commit(cb, nil, nil, 0);
    return 0;
}

static void rt_cache_trim(void) {
    if (!rt_cache_on() || atomic_load(&g_up_pending)) return;
    uint64_t frame = atomic_load(&g_cur_frame);
    static uint64_t last_frame;
    if (frame == last_frame) return;
    last_frame = frame;
#if TARGET_OS_IPHONE
    size_t headroom = os_proc_available_memory();
#else
    return;
#endif
    pthread_mutex_lock(&g_mu); uint64_t done = g_done; pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_rt_cache_mu);
#if TARGET_OS_IPHONE
    // Process headroom does not describe system-wide pressure. Keep reclaiming
    // throughout a warning, and use separate watermarks to avoid oscillation.
    unsigned long pressure = atomic_load(&g_rt_pressure_flags);
    if (headroom < RT_CACHE_START_HEADROOM ||
        (pressure & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL))) g_rt_reclaiming = 1;
    else if (headroom >= RT_CACHE_STOP_HEADROOM) g_rt_reclaiming = 0;
#endif
    for (unsigned i = 0; i < g_rt_pending_n;) {
        NvRtCache *entry = g_rt_pending[i];
        id<MTLTexture> texture = entry->texture;
        int result = texture ? rt_cache_snapshot_step(entry, texture) : -1;
        if (result < 0) {
            rt_cache_cancel(entry); entry->excluded = 1;
            MLOG("texture snapshot unavailable: serial %llu; keeping original contents", (unsigned long long)entry->serial);
        } else if (result > 0) {
            res_remove(texture); res_commit();
            [texture setPurgeableState:MTLPurgeableStateEmpty]; entry->spilled = 1;
            g_rt_spilled_bytes += texture.allocatedSize; g_rt_spills++; g_ser_gen++;
            rt_cache_pending_remove(entry);
            if (g_rt_spills <= 16 || g_rt_spills % 64 == 0)
                MLOG("texture cache frame %llu: spilled serial %llu, %llu MiB; total %llu MiB, restored %llu MiB, spills %llu restores %llu",
                    (unsigned long long)frame, (unsigned long long)entry->serial, (unsigned long long)(texture.allocatedSize >> 20),
                    (unsigned long long)(g_rt_spilled_bytes >> 20), (unsigned long long)(g_rt_restored_bytes >> 20),
                    (unsigned long long)g_rt_spills, (unsigned long long)g_rt_restores);
        } else i++;
    }
    // Small textures share the same staging budget instead of waiting for one
    // texture's complete mip chain per frame. Never wait for these copies here.
    unsigned admitted = 0;
    // Long-idle storage also leaves memory without a pressure notification.
    // Headroom only accelerates the age threshold; it is not a lifetime policy.
    uint64_t idle_frames = g_rt_reclaiming ? 120 : RT_CACHE_IDLE_FRAMES;
    while (admitted < RT_CACHE_PENDING_LIMIT && g_rt_pending_n < RT_CACHE_PENDING_LIMIT &&
            atomic_load(&g_rt_staging_bytes) < NVN_SNAPSHOT_CHUNK) {
        NvRtCache *oldest = nil;
        id<MTLTexture> oldest_texture = nil;
        for (NvRtCache *entry in g_rt_cache.objectEnumerator) {
            if (entry->pending || entry->spilled || entry->excluded || entry->used_fence > done ||
                frame < entry->used_frame || frame - entry->used_frame < idle_frames) continue;
            id<MTLTexture> texture = entry->texture;
            if (texture && (!oldest || entry->used_frame < oldest->used_frame)) {
                oldest = entry; oldest_texture = texture;
            }
        }
        if (!oldest) break;
        admitted++;
        oldest->pending = 1; g_rt_pending[g_rt_pending_n++] = oldest;
        if (rt_cache_snapshot_step(oldest, oldest_texture) < 0) {
            rt_cache_cancel(oldest); oldest->excluded = 1;
            MLOG("texture snapshot unavailable: serial %llu; keeping original contents", (unsigned long long)oldest->serial);
        }
    }
    pthread_mutex_unlock(&g_rt_cache_mu);
}

static NSDictionary *rt_cache_status(void) {
    pthread_mutex_lock(&g_rt_cache_mu);
    uint64_t bytes = 0; unsigned spilled = 0, excluded = 0;
    for (NvRtCache *entry in g_rt_cache.objectEnumerator) {
        if (entry->spilled) { spilled++; bytes += entry->file_bytes; }
        excluded += entry->excluded != 0;
    }
    NSDictionary *status = @{@"enabled": @(rt_cache_on()), @"tracked": @(g_rt_cache.count),
        @"pressure_flags": @(atomic_load(&g_rt_pressure_flags)), @"pressure_events": @(atomic_load(&g_rt_pressure_events)),
        @"reclaiming": @(g_rt_reclaiming), @"start_headroom_bytes": @(RT_CACHE_START_HEADROOM),
        @"stop_headroom_bytes": @(RT_CACHE_STOP_HEADROOM),
        @"idle_frames": @(RT_CACHE_IDLE_FRAMES),
        @"spilled": @(spilled), @"excluded": @(excluded), @"file_bytes": @(bytes),
        @"pending": @(g_rt_pending_n != 0), @"pending_count": @(g_rt_pending_n),
        @"staging_bytes": @(atomic_load(&g_rt_staging_bytes)), @"staging_limit_bytes": @(NVN_SNAPSHOT_CHUNK),
        @"spills": @(g_rt_spills), @"restores": @(g_rt_restores),
        @"cancels": @(g_rt_cancels), @"spilled_bytes_total": @(g_rt_spilled_bytes), @"restored_bytes_total": @(g_rt_restored_bytes)};
    pthread_mutex_unlock(&g_rt_cache_mu);
    return status;
}
