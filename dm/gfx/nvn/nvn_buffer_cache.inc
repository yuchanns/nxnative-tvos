// Authoritative shared buffers need their own backing: discarding a derived
// texture does not release the encoded blocks retained by its owner.
// Only explicitly tracked, directly bound buffers participate. Guest pools and
// buffers containing indirect GPU addresses must never enter this cache.
@interface NvBufferCache : NSObject {
@public
    __weak id<MTLBuffer> buffer;
    uint64_t used_fence, used_frame, file_offset, file_bytes, written;
    int fd, spilled, excluded;
} @end
@implementation NvBufferCache
- (instancetype)init { if ((self = [super init])) fd = -1; return self; }
- (void)dealloc { if (fd >= 0) rt_cache_file_release(file_offset, file_bytes); }
@end

static NSMapTable<id<MTLBuffer>, NvBufferCache *> *g_buffer_cache;
static pthread_mutex_t g_buffer_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_buffer_spills, g_buffer_restores, g_buffer_cancels, g_buffer_errors;
enum { BUFFER_CACHE_IDLE_FRAMES = 120, BUFFER_CACHE_WRITE_LIMIT = 4 << 20 };

static void buffer_cache_track(id<MTLBuffer> buffer) {
    if (!rt_cache_on() || !on_sub_thread() || !buffer || !buffer.length ||
        buffer.storageMode != MTLStorageModeShared || buffer.heap) return;
    pthread_mutex_lock(&g_buffer_cache_mu);
    if (!g_buffer_cache) g_buffer_cache = [NSMapTable weakToStrongObjectsMapTable];
    if (![g_buffer_cache objectForKey:buffer]) {
        NvBufferCache *entry = [NvBufferCache new]; entry->buffer = buffer;
        entry->used_fence = g_fence + 1; entry->used_frame = atomic_load(&g_cur_frame);
        [g_buffer_cache setObject:entry forKey:buffer];
    }
    pthread_mutex_unlock(&g_buffer_cache_mu);
}

static void buffer_cache_cancel(NvBufferCache *entry) {
    if (entry->fd >= 0) rt_cache_file_release(entry->file_offset, entry->file_bytes);
    entry->fd = -1; entry->file_bytes = entry->written = 0;
}

static int buffer_cache_restore(NvBufferCache *entry, id<MTLBuffer> buffer) {
    if (entry->fd < 0 || entry->file_bytes != buffer.length || entry->written != entry->file_bytes) return 0;
    [buffer setPurgeableState:MTLPurgeableStateNonVolatile];
    uint8_t *bytes = buffer.contents;
    if (!bytes) return 0;
    for (uint64_t offset = 0; offset < entry->file_bytes;) {
        size_t size = (size_t)MIN(entry->file_bytes - offset, (uint64_t)BUFFER_CACHE_WRITE_LIMIT);
        if (!rt_cache_io(entry->fd, bytes + offset, size, entry->file_offset + offset, 1)) return 0;
        offset += size;
    }
    return 1;
}

// Called at every encoded-block access, including cached raw views and CPU
// readback. Merely sampling the derived texture does not need these blocks.
static void buffer_cache_use(id<MTLBuffer> buffer) {
    if (!rt_cache_on() || !buffer) return;
    pthread_mutex_lock(&g_buffer_cache_mu);
    NvBufferCache *entry = [g_buffer_cache objectForKey:buffer];
    if (entry) {
        if (entry->spilled) {
            if (!buffer_cache_restore(entry, buffer)) MDIE("encoded buffer snapshot restore failed");
            entry->spilled = 0; g_buffer_restores++;
        } else if (entry->fd >= 0) g_buffer_cancels++;
        // A later GPU/CPU write can change any byte; do not reuse a partial or
        // previously restored snapshot as if it still represented this buffer.
        if (entry->fd >= 0) buffer_cache_cancel(entry);
        if (!on_sub_thread()) entry->excluded = 1;
        entry->used_fence = g_fence + 1; entry->used_frame = atomic_load(&g_cur_frame);
    }
    pthread_mutex_unlock(&g_buffer_cache_mu);
}

static void buffer_cache_trim(void) { @autoreleasepool {
    if (!rt_cache_on() || atomic_load(&g_up_pending)) return;
    uint64_t frame = atomic_load(&g_cur_frame);
    static uint64_t last_frame;
    if (frame == last_frame) return;
    last_frame = frame;
    pthread_mutex_lock(&g_mu); uint64_t done = g_done; pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_buffer_cache_mu);
    size_t budget = BUFFER_CACHE_WRITE_LIMIT;
    for (NvBufferCache *entry in g_buffer_cache.objectEnumerator) {
        if (!budget) break;
        if (entry->excluded || entry->spilled || entry->used_fence > done ||
            frame < entry->used_frame || frame - entry->used_frame < BUFFER_CACHE_IDLE_FRAMES) continue;
        id<MTLBuffer> buffer = entry->buffer;
        if (!buffer) continue;
        if (entry->fd < 0) {
            entry->file_bytes = buffer.length;
            entry->fd = rt_cache_file_allocate(entry->file_bytes, &entry->file_offset);
        }
        size_t size = (size_t)MIN(entry->file_bytes - entry->written, (uint64_t)budget);
        uint8_t *bytes = buffer.contents;
        if (entry->fd < 0 || !bytes || !rt_cache_io(entry->fd, bytes + entry->written, size,
                entry->file_offset + entry->written, 0)) {
            buffer_cache_cancel(entry); entry->excluded = 1; g_buffer_errors++;
            MLOG("encoded buffer snapshot unavailable; retaining original %lu bytes", (unsigned long)buffer.length);
            continue;
        }
        entry->written += size; budget -= size;
        if (entry->written == entry->file_bytes) {
            // The completed fence protects both CPU reads above and physical
            // page disposal. Keep the Metal object stable for existing aliases.
            [buffer setPurgeableState:MTLPurgeableStateEmpty];
            entry->spilled = 1; g_buffer_spills++;
        }
    }
    pthread_mutex_unlock(&g_buffer_cache_mu);
} }

static NSDictionary *buffer_cache_status(void) {
    pthread_mutex_lock(&g_buffer_cache_mu);
    unsigned count = 0, spilled = 0, pending = 0, excluded = 0;
    uint64_t bytes = 0, backed = 0;
    for (NvBufferCache *entry in g_buffer_cache.objectEnumerator) {
        id<MTLBuffer> buffer = entry->buffer;
        if (!buffer) continue;
        count++; excluded += entry->excluded != 0;
        if (entry->spilled) { spilled++; backed += entry->file_bytes; }
        else { bytes += buffer.allocatedSize; pending += entry->fd >= 0; }
    }
    NSDictionary *status = @{@"scope": @"encoded BC buffers; allocation bytes are not physical residency",
        @"tracked": @(count), @"spilled": @(spilled), @"pending": @(pending), @"excluded": @(excluded),
        @"nonpurgeable_allocated_bytes": @(bytes), @"file_bytes": @(backed),
        @"spills": @(g_buffer_spills), @"restores": @(g_buffer_restores),
        @"cancels": @(g_buffer_cancels), @"errors": @(g_buffer_errors),
        @"idle_frames": @(BUFFER_CACHE_IDLE_FRAMES), @"write_limit_bytes": @(BUFFER_CACHE_WRITE_LIMIT)};
    pthread_mutex_unlock(&g_buffer_cache_mu);
    return status;
}
