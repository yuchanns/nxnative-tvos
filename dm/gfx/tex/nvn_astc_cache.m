#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CommonCrypto/CommonDigest.h>
#include "nvn_astc_cache.h"
#include "nvn_astc.h"
#include "nvn_bcdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>

int nvn_bc_astc_format(uint32_t format, unsigned block, uint32_t *pixel_format) {
    nvn_bc_format_info info;
    if ((block != 4 && block != 6) || !nvn_bc_format(format, &info) || info.pixel_bytes != 4) return 0;
    int srgb = info.pixel_format == MTLPixelFormatRGBA8Unorm_sRGB;
    if (pixel_format) *pixel_format = block == 4 ? (srgb ? MTLPixelFormatASTC_4x4_sRGB : MTLPixelFormatASTC_4x4_LDR)
                                                : (srgb ? MTLPixelFormatASTC_6x6_sRGB : MTLPixelFormatASTC_6x6_LDR);
    return 1;
}

// This namespace changes with encoder settings, version, or the stripe layout.
// Fixed-width little-endian parameters keep Mac-generated caches usable on tvOS.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static void cache_key(uint32_t format, uint32_t width, uint32_t height, unsigned block,
                      const void *source, size_t size, char key[65]) {
    static const char version[] = "nxnative-astc-arm-5.7.0-fastest-stripe4m-v1";
    uint32_t fields[] = {format, width, height, block};
    uint8_t parameters[16], digest[CC_SHA256_DIGEST_LENGTH];
    for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 4; j++) parameters[i * 4 + j] = fields[i] >> (j * 8);
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    CC_SHA256_Update(&hash, version, sizeof version);
    CC_SHA256_Update(&hash, parameters, sizeof parameters);
    CC_SHA256_Update(&hash, source, (CC_LONG)size);
    CC_SHA256_Final(digest, &hash);
    for (unsigned i = 0; i < sizeof digest; i++) snprintf(key + i * 2, 3, "%02x", digest[i]);
}
static void payload_hash(const void *data, size_t size, uint8_t digest[CC_SHA256_DIGEST_LENGTH]) {
    CC_SHA256(data, (CC_LONG)size, digest);
}
#pragma clang diagnostic pop

static NSString *cache_path(const char *root, const char *key) {
    if (!root || !*root) return nil;
    return [[NSString stringWithUTF8String:root] stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.astc", key]];
}
static const uint8_t cache_magic[8] = {'N', 'X', 'A', 'S', 'T', 'C', '0', '1'};
static NSDictionary<NSString *, NSData *> *g_cache_small;
static NSDictionary<NSString *, NSData *> *g_cache_mapped;
static void *g_cache_mapped_base;
static size_t g_cache_mapped_size;
static int g_cache_snapshot_fd = -1;
static _Atomic int g_cache_small_ready;
static _Atomic uint64_t g_cache_memory_hits, g_cache_disk_hits, g_cache_mapped_hits;
const char *nvn_bc_astc_cache_status_name(nvn_astc_cache_status status) {
    static const char *names[] = {"unchecked", "disabled", "hit", "missing", "header", "size", "hash", "io"};
    return (unsigned)status < sizeof names / sizeof *names ? names[status] : "unknown";
}
static nvn_astc_cache_status cache_read_disk_status(NSString *path, void *output, size_t size) {
    if (!path) return NVN_ASTC_CACHE_DISABLED;
    FILE *file = fopen(path.fileSystemRepresentation, "rb");
    if (!file) return errno == ENOENT ? NVN_ASTC_CACHE_MISSING : NVN_ASTC_CACHE_IO;
    uint8_t header[40], digest[CC_SHA256_DIGEST_LENGTH];
    nvn_astc_cache_status status = NVN_ASTC_CACHE_HIT;
    if (fread(header, 1, sizeof header, file) != sizeof header) status = NVN_ASTC_CACHE_SIZE;
    else if (memcmp(header, cache_magic, sizeof cache_magic)) status = NVN_ASTC_CACHE_HEADER;
    else if (fread(output, 1, size, file) != size || fgetc(file) != EOF) status = NVN_ASTC_CACHE_SIZE;
    if (ferror(file)) status = NVN_ASTC_CACHE_IO;
    if (fclose(file)) status = NVN_ASTC_CACHE_IO;
    if (status != NVN_ASTC_CACHE_HIT) return status;
    payload_hash(output, size, digest);
    return memcmp(header + sizeof cache_magic, digest, sizeof digest) ? NVN_ASTC_CACHE_HASH : NVN_ASTC_CACHE_HIT;
}
static int cache_read_disk(NSString *path, void *output, size_t size) {
    return cache_read_disk_status(path, output, size) == NVN_ASTC_CACHE_HIT;
}

// Copy and validate with bounded scratch. An invalid source is a cache miss;
// a destination write failure stops snapshot growth. Neither is published.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static int cache_snapshot_copy(NSString *path, int fd, off_t offset, size_t size,
                                void *scratch, size_t scratch_size) {
    FILE *file = fopen(path.fileSystemRepresentation, "rb");
    if (!file) return 0;
    uint8_t header[40], digest[CC_SHA256_DIGEST_LENGTH];
    int valid = fread(header, 1, sizeof header, file) == sizeof header &&
                !memcmp(header, cache_magic, sizeof cache_magic);
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    for (size_t done = 0; valid == 1 && done < size;) {
        size_t count = MIN(size - done, scratch_size);
        if (fread(scratch, 1, count, file) != count) { valid = 0; break; }
        CC_SHA256_Update(&hash, scratch, (CC_LONG)count);
        size_t written = 0;
        while (written < count) {
            ssize_t n = pwrite(fd, (uint8_t *)scratch + written, count - written,
                               offset + (off_t)(done + written));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { valid = -1; break; }
            written += (size_t)n;
        }
        done += count;
    }
    if (valid == 1) {
        CC_SHA256_Final(digest, &hash);
        valid = fgetc(file) == EOF && !ferror(file) &&
                !memcmp(header + sizeof cache_magic, digest, sizeof digest);
    }
    fclose(file);
    return valid;
}
#pragma clang diagnostic pop

static NSDictionary<NSString *, NSData *> *cache_prepare_mapped(NSArray<NSDictionary *> *entries,
        NSDictionary<NSString *, NSData *> *small, size_t budget) {
    if (!budget || budget > INT64_MAX) return @{};
    char path[PATH_MAX];
    int length = snprintf(path, sizeof path, "%s/dm-astc-snapshot-XXXXXX", NSTemporaryDirectory().fileSystemRepresentation);
    if (length < 0 || (size_t)length >= sizeof path) return @{};
    int fd = mkstemp(path);
    if (fd < 0) return @{};
    if (unlink(path)) { close(fd); return @{}; }
    const size_t scratch_size = 256u << 10;
    void *scratch = malloc(scratch_size);
    if (!scratch) { close(fd); return @{}; }
    NSMutableDictionary<NSString *, NSArray<NSNumber *> *> *ranges = [NSMutableDictionary dictionary];
    size_t page = (size_t)getpagesize(), used = 0;
    for (NSDictionary *entry in entries) {
        NSString *name = entry[@"path"];
        if (small[name] || ranges[name]) continue;
        size_t size = [entry[@"bytes"] unsignedLongLongValue];
        if (!size || size > SIZE_MAX - (page - 1)) continue;
        size_t span = (size + page - 1) / page * page;
        if (span > budget - used) continue;
        int valid = cache_snapshot_copy(name, fd, (off_t)used, size, scratch, scratch_size);
        if (valid < 0) break;
        if (!valid) continue;
        ranges[name] = @[@(used), @(size)];
        used += span;
    }
    free(scratch);
    // Truncate any rejected trailing entry, then finish writes before gameplay.
    // The unlinked file cannot later be truncated or replaced through a cache
    // pathname, which would otherwise invalidate a verified mapping or SIGBUS.
    if (!used || ftruncate(fd, (off_t)used) || fsync(fd)) { close(fd); return @{}; }
    void *mapped = mmap(NULL, used, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) { close(fd); return @{}; }
    NSMutableDictionary<NSString *, NSData *> *prepared = [NSMutableDictionary dictionary];
    for (NSString *name in ranges) {
        NSArray<NSNumber *> *range = ranges[name];
        prepared[name] = [NSData dataWithBytesNoCopy:(uint8_t *)mapped + range[0].unsignedLongLongValue
            length:range[1].unsignedLongLongValue freeWhenDone:NO];
    }
    // The process-lifetime dictionary owns this immutable file mapping. Its
    // pages may be reclaimed independently of the small retained heap cache.
    g_cache_mapped_base = mapped; g_cache_mapped_size = used;
    // Keep one descriptor for positional bulk reads. This lets the filesystem
    // service a range instead of serializing demand faults on the replay thread.
    // It neither pins mapped pages nor retains another copy of the payload.
    const char *bulk = getenv("DM_NVN_ASTC_SNAPSHOT_PREAD");
    if ((!bulk || strcmp(bulk, "0")) && !fcntl(fd, F_SETFD, FD_CLOEXEC)) g_cache_snapshot_fd = fd;
    else close(fd);
    (void)madvise(mapped, used, MADV_DONTNEED);
    return [prepared copy];
}

static int cache_snapshot_read(void *output, const void *source, size_t size) {
    uintptr_t base = (uintptr_t)g_cache_mapped_base, address = (uintptr_t)source;
    if (g_cache_snapshot_fd < 0 || !output || !size || address < base ||
        size > g_cache_mapped_size || address - base > g_cache_mapped_size - size) return 0;
    size_t offset = address - base;
    for (size_t done = 0; done < size;) {
        size_t count = MIN(size - done, 1u << 20);
        ssize_t n = pread(g_cache_snapshot_fd, (uint8_t *)output + done, count, (off_t)(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}

static nvn_astc_cache_status cache_read_status(NSString *path, void *output, size_t size) {
    if (!path) return NVN_ASTC_CACHE_DISABLED;
    if (atomic_load_explicit(&g_cache_small_ready, memory_order_acquire)) {
        NSData *data = g_cache_small[path];
        if (data && data.length == size) {
            memcpy(output, data.bytes, size);
            atomic_fetch_add_explicit(&g_cache_memory_hits, 1, memory_order_relaxed);
            return NVN_ASTC_CACHE_HIT;
        }
        data = g_cache_mapped[path];
        if (data && data.length == size) {
            // The immutable mapping also supplies a complete fallback after a
            // failed or partial read; never publish a partially filled plane.
            if (!cache_snapshot_read(output, data.bytes, size)) memcpy(output, data.bytes, size);
            atomic_fetch_add_explicit(&g_cache_mapped_hits, 1, memory_order_relaxed);
            return NVN_ASTC_CACHE_HIT;
        }
    }
    nvn_astc_cache_status status = cache_read_disk_status(path, output, size);
    if (status == NVN_ASTC_CACHE_HIT) atomic_fetch_add_explicit(&g_cache_disk_hits, 1, memory_order_relaxed);
    return status;
}
static int cache_read(NSString *path, void *output, size_t size) {
    return cache_read_status(path, output, size) == NVN_ASTC_CACHE_HIT;
}
void nvn_bc_astc_cache_stats(uint64_t *memory_hits, uint64_t *disk_hits, uint64_t *mapped_hits) {
    *memory_hits = atomic_load_explicit(&g_cache_memory_hits, memory_order_relaxed);
    *disk_hits = atomic_load_explicit(&g_cache_disk_hits, memory_order_relaxed);
    *mapped_hits = atomic_load_explicit(&g_cache_mapped_hits, memory_order_relaxed);
}
void nvn_bc_astc_cache_prepare(const char *readonly_cache, const char *cache,
                                size_t budget, size_t mapped_budget) {
    if (!budget && !mapped_budget) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ @autoreleasepool {
        uint64_t started = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        NSMutableSet<NSString *> *roots = [NSMutableSet set];
        if (readonly_cache && *readonly_cache) [roots addObject:@(readonly_cache)];
        if (cache && *cache) [roots addObject:@(cache)];
        NSMutableArray<NSDictionary *> *entries = [NSMutableArray array];
        NSFileManager *manager = NSFileManager.defaultManager;
        for (NSString *root in roots) {
            NSArray<NSURL *> *files = [manager contentsOfDirectoryAtURL:[NSURL fileURLWithPath:root isDirectory:YES]
                includingPropertiesForKeys:@[NSURLIsRegularFileKey, NSURLFileSizeKey]
                options:NSDirectoryEnumerationSkipsHiddenFiles error:NULL];
            for (NSURL *url in files) {
                if (url.lastPathComponent.length != 69 || ![url.pathExtension isEqualToString:@"astc"]) continue;
                NSNumber *regular = nil, *length = nil;
                if (![url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:NULL] || !regular.boolValue ||
                    ![url getResourceValue:&length forKey:NSURLFileSizeKey error:NULL]) continue;
                uint64_t bytes = length.unsignedLongLongValue;
                if (bytes <= 40 || bytes - 40 > SIZE_MAX) continue;
                // NSURL can expand aliases such as /var to /private/var. Keep
                // the caller's spelling so cache_read builds the same key.
                NSString *path = [root stringByAppendingPathComponent:url.lastPathComponent];
                [entries addObject:@{@"path": path, @"bytes": @(bytes - 40)}];
            }
        }
        [entries sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
            NSComparisonResult order = [a[@"bytes"] compare:b[@"bytes"]];
            return order == NSOrderedSame ? [a[@"path"] compare:b[@"path"]] : order;
        }];
        NSMutableDictionary<NSString *, NSData *> *prepared = [NSMutableDictionary dictionary];
        size_t used = 0;
        for (NSDictionary *entry in entries) {
            NSString *path = entry[@"path"];
            if (prepared[path]) continue;
            size_t size = [entry[@"bytes"] unsignedLongLongValue];
            if (size > 64u * 1024u || size > budget - used) break;
            void *bytes = malloc(size);
            if (!bytes) break;
            // Validate once before publication. Content-addressed entries never
            // change their meaning, so later reads use this verified snapshot.
            if (!cache_read_disk(path, bytes, size)) { free(bytes); continue; }
            prepared[path] = [NSData dataWithBytesNoCopy:bytes length:size freeWhenDone:YES];
            used += size;
        }
        g_cache_small = [prepared copy];
        g_cache_mapped = cache_prepare_mapped(entries, prepared, mapped_budget);
        atomic_store_explicit(&g_cache_small_ready, 1, memory_order_release);
        fprintf(stderr, "[dm] ASTC cache preparation: %lu small entries, %zu heap bytes / %zu budget, %lu mapped entries, %zu file bytes / %zu budget, %.1f ms\n",
                (unsigned long)g_cache_small.count, used, budget,
                (unsigned long)g_cache_mapped.count, g_cache_mapped_size, mapped_budget,
                (clock_gettime_nsec_np(CLOCK_MONOTONIC) - started) / 1e6);
        if (g_cache_mapped_base)
            fprintf(stderr, "[dm] ASTC snapshot mapping: %p, %zu bytes; reads %s\n", g_cache_mapped_base, g_cache_mapped_size,
                    g_cache_snapshot_fd >= 0 ? "pread (at most 1 MiB per call)" : "mapped memcpy");
    } });
}
static int cache_write(NSString *path, const void *output, size_t size) {
    if (!path) return 0;
    [[NSFileManager defaultManager] createDirectoryAtPath:path.stringByDeletingLastPathComponent
                            withIntermediateDirectories:YES attributes:nil error:NULL];
    char *temporary = strdup([[path stringByAppendingString:@".XXXXXX"] fileSystemRepresentation]);
    if (!temporary) return ENOMEM;
    int fd = mkstemp(temporary);
    if (fd < 0) { int error = errno; free(temporary); return error; }
    FILE *file = fdopen(fd, "wb");
    int error = file ? 0 : errno;
    if (file) {
        uint8_t header[40]; memcpy(header, cache_magic, sizeof cache_magic);
        payload_hash(output, size, header + sizeof cache_magic);
        if (fwrite(header, 1, sizeof header, file) != sizeof header || fwrite(output, 1, size, file) != size) error = errno ? errno : EIO;
        if (fclose(file) && !error) error = errno ? errno : EIO;
    } else close(fd);
    if (!error && rename(temporary, path.fileSystemRepresentation)) error = errno;
    unlink(temporary);
    free(temporary);
    return error;
}

int nvn_bc_astc_cache_read(const nvn_astc_cache_key *key, void *output, size_t size,
                           const char *cache, const char *readonly_cache) {
    if (!key || !output || !size || key->hex[64]) return 0;
    for (unsigned i = 0; i < 64; i++)
        if (!((key->hex[i] >= '0' && key->hex[i] <= '9') || (key->hex[i] >= 'a' && key->hex[i] <= 'f'))) return 0;
    @autoreleasepool {
        return cache_read(cache_path(readonly_cache, key->hex), output, size) ||
               cache_read(cache_path(cache, key->hex), output, size);
    }
}

int nvn_bc_astc_encode_report(uint32_t format, const void *source, size_t source_size,
                       uint32_t width, uint32_t height, unsigned block,
                       void *output, size_t output_size, const char *cache,
                       const char *readonly_cache, int *cache_hit,
                       nvn_astc_cache_key *saved_key, nvn_astc_cache_report *report) {
    if (cache_hit) *cache_hit = 0;
    if (saved_key) memset(saved_key, 0, sizeof *saved_key);
    if (report) memset(report, 0, sizeof *report);
    if (!source || !output || !width || !height || width > 16384 || height > 16384 || !nvn_bc_astc_format(format, block, NULL)) return -1;
    nvn_bc_format_info info; nvn_bc_format(format, &info);
    size_t bc_pitch = ((size_t)width + 3) / 4 * info.block_bytes;
    size_t astc_pitch = ((size_t)width + block - 1) / block * 16;
    if (source_size != bc_pitch * (((size_t)height + 3) / 4) || output_size != astc_pitch * (((size_t)height + block - 1) / block)) return -1;
    uintptr_t src = (uintptr_t)source, dst = (uintptr_t)output;
    if (dst >= src ? dst - src < source_size : src - dst < output_size) return -1;
    @autoreleasepool {
        char key[65]; cache_key(format, width, height, block, source, source_size, key);
        if (report) memcpy(report->key.hex, key, sizeof key);
        NSString *path = cache_path(cache, key), *readonly_path = cache_path(readonly_cache, key);
        nvn_astc_cache_status ro = cache_read_status(readonly_path, output, output_size), rw = NVN_ASTC_CACHE_UNCHECKED;
        if (ro != NVN_ASTC_CACHE_HIT)
            rw = [path isEqualToString:readonly_path] ? ro : cache_read_status(path, output, output_size);
        if (report) { report->readonly_status = ro; report->writable_status = rw; }
        if (ro == NVN_ASTC_CACHE_HIT || rw == NVN_ASTC_CACHE_HIT) {
            if (cache_hit) *cache_hit = 1;
            if (saved_key) memcpy(saved_key->hex, key, sizeof key);
            return 0;
        }
        // Every stripe begins on both a BC and an ASTC block boundary. This bounds
        // decoded scratch without adding seams or retaining a second full texture.
        size_t row = (size_t)width * 4, alignment = block == 6 ? 12 : 4;
        size_t rows = ((4u << 20) / row / alignment) * alignment;
        if (!rows) rows = alignment;
        if (rows > height) rows = height;
        uint8_t *pixels = malloc(rows * row);
        if (!pixels) return -1;
        int result = 0;
        for (uint32_t y = 0; y < height && !result; y += rows) {
            uint32_t count = MIN(rows, height - y);
            size_t offset = (size_t)y / 4 * bc_pitch;
            result = nvn_bc_decode(format, (const uint8_t *)source + offset, source_size - offset, bc_pitch,
                                   width, count, pixels, rows * row, row);
            if (!result) result = nvn_astc_encode_rgba8(pixels, width, count, info.pixel_format == MTLPixelFormatRGBA8Unorm_sRGB,
                block, (uint8_t *)output + (size_t)y / block * astc_pitch, astc_pitch * ((count + block - 1) / block));
        }
        free(pixels);
        if (!result) {
            int error = cache_write(path, output, output_size);
            if (report) { report->write_attempted = path != nil; report->write_error = error; }
            if (saved_key) memcpy(saved_key->hex, key, sizeof key);
        }
        return result;
    }
}

int nvn_bc_astc_encode_key(uint32_t format, const void *source, size_t source_size,
                       uint32_t width, uint32_t height, unsigned block,
                       void *output, size_t output_size, const char *cache,
                       const char *readonly_cache, int *cache_hit,
                       nvn_astc_cache_key *saved_key) {
    return nvn_bc_astc_encode_report(format, source, source_size, width, height, block,
        output, output_size, cache, readonly_cache, cache_hit, saved_key, NULL);
}

int nvn_bc_astc_encode(uint32_t format, const void *source, size_t source_size,
                       uint32_t width, uint32_t height, unsigned block,
                       void *output, size_t output_size, const char *cache,
                       const char *readonly_cache, int *cache_hit) {
    return nvn_bc_astc_encode_key(format, source, source_size, width, height, block,
                                  output, output_size, cache, readonly_cache, cache_hit, NULL);
}
