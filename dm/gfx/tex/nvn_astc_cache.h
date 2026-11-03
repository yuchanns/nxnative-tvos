#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// BC1/2/3/7 can use LDR ASTC. BC4/5 and HDR BC6 retain their decoded formats.
int nvn_bc_astc_format(uint32_t format, unsigned block, uint32_t *pixel_format);
// Prepare verified immutable snapshots before gameplay. The first budget bounds
// heap payload bytes; the second bounds a private file snapshot whose read-only
// mapped pages remain reclaimable. Entries that do not fit keep the disk path.
// Snapshot reads use one retained descriptor and bounded pread calls unless
// DM_NVN_ASTC_SNAPSHOT_PREAD=0; failures fall back to the immutable mapping.
void nvn_bc_astc_cache_prepare(const char *readonly_cache, const char *cache,
                                size_t budget, size_t mapped_budget);
void nvn_bc_astc_cache_stats(uint64_t *memory_hits, uint64_t *disk_hits, uint64_t *mapped_hits);
typedef struct { char hex[65]; } nvn_astc_cache_key;
typedef enum {
    NVN_ASTC_CACHE_UNCHECKED, NVN_ASTC_CACHE_DISABLED, NVN_ASTC_CACHE_HIT,
    NVN_ASTC_CACHE_MISSING, NVN_ASTC_CACHE_HEADER, NVN_ASTC_CACHE_SIZE,
    NVN_ASTC_CACHE_HASH, NVN_ASTC_CACHE_IO
} nvn_astc_cache_status;
typedef struct {
    nvn_astc_cache_key key;
    nvn_astc_cache_status readonly_status, writable_status;
    int write_attempted, write_error;
} nvn_astc_cache_report;
const char *nvn_bc_astc_cache_status_name(nvn_astc_cache_status status);
// Restore a previously encoded plane without touching its BC source. The caller
// must first prove that the source and its layout have not changed. A miss or a
// corrupt entry returns zero and requires the normal source upload.
int nvn_bc_astc_cache_read(const nvn_astc_cache_key *key, void *output, size_t size,
                           const char *cache, const char *readonly_cache);
// Tightly packed BC input and ASTC output. Optional caches contain only encoded
// blocks; callers retain no decoded pixels after this call. Zero means success.
int nvn_bc_astc_encode(uint32_t format, const void *source, size_t source_size,
                       uint32_t width, uint32_t height, unsigned block,
                       void *output, size_t output_size, const char *cache,
                       const char *readonly_cache, int *cache_hit);
int nvn_bc_astc_encode_key(uint32_t format, const void *source, size_t source_size,
                           uint32_t width, uint32_t height, unsigned block,
                           void *output, size_t output_size, const char *cache,
                           const char *readonly_cache, int *cache_hit,
                           nvn_astc_cache_key *saved_key);
// Per-call diagnostics never retain guest memory and do not change cache keys.
int nvn_bc_astc_encode_report(uint32_t format, const void *source, size_t source_size,
                              uint32_t width, uint32_t height, unsigned block,
                              void *output, size_t output_size, const char *cache,
                              const char *readonly_cache, int *cache_hit,
                              nvn_astc_cache_key *saved_key, nvn_astc_cache_report *report);
#ifdef __cplusplus
}
#endif
