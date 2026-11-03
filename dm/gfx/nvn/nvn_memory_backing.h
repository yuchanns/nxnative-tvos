#pragma once
#include <TargetConditionals.h>
#include <mach/mach.h>
#if !TARGET_OS_IPHONE
#include <mach/mach_vm.h>
#endif
#include <mach/vm_region.h>
#include <stdint.h>
#include <string.h>
#if !TARGET_OS_IPHONE
#include <libproc.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// A cached region is valid only while the caller keeps its memory mapping alive.
// NVN pools hold their storage lease through GPU retirement; pool generation
// changes invalidate these entries. Copy-on-write and unknown mappings fail shut.
enum { NVN_BACKING_OBJECT = 1, NVN_BACKING_FILE = 2, NVN_BACKING_CACHE = 64 };
typedef struct { uint64_t first, second; unsigned kind; } nvn_backing_key;
typedef struct { nvn_backing_key key; uint64_t start, end, offset; } nvn_backing_region;
typedef struct { nvn_backing_key key; uint64_t lo, hi; } nvn_backing_range;
typedef struct { nvn_backing_region entries[NVN_BACKING_CACHE]; uint64_t generation; unsigned count, next; } nvn_backing_cache;

static inline int nvn_backing_key_equal(nvn_backing_key a, nvn_backing_key b) {
    return a.kind == b.kind && a.first == b.first && a.second == b.second;
}
static inline int nvn_backing_file_mode(unsigned mode) {
    // SM_PRIVATE is an object reference classification, not MAP_PRIVATE.
    // COW entries and shadow chains are rejected before using file offsets.
    return mode == SM_PRIVATE || mode == SM_PRIVATE_ALIASED || mode == SM_SHARED ||
        mode == SM_TRUESHARED || mode == SM_SHARED_ALIASED;
}
static inline int nvn_backing_query(uint64_t address, nvn_backing_region *result) {
    if (!address || !result) return 0;
#if TARGET_OS_IPHONE
    vm_address_t base; vm_size_t bytes = 0;
#else
    mach_vm_address_t base; mach_vm_size_t bytes = 0;
#endif
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    for (;;) {
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        base = address; memset(&info, 0, sizeof info);
#if TARGET_OS_IPHONE
        kern_return_t status = vm_region_recurse_64(mach_task_self(), &base, &bytes, &depth,
                (vm_region_recurse_info_t)&info, &count);
#else
        kern_return_t status = mach_vm_region_recurse(mach_task_self(), &base, &bytes, &depth,
                (vm_region_recurse_info_t)&info, &count);
#endif
        if (status != KERN_SUCCESS ||
            count < VM_REGION_SUBMAP_INFO_V2_COUNT_64 || depth > 16) return 0;
        if (!info.is_submap) break;
        depth++;
    }
    if (!bytes || base > address || address - base >= bytes || base > UINT64_MAX - bytes ||
        info.offset > UINT64_MAX - bytes || !info.object_id_full || info.shadow_depth || info.share_mode == SM_COW) return 0;
    nvn_backing_region region = {.start = base, .end = base + bytes, .offset = info.offset};
    if (!info.external_pager) {
        if (info.share_mode == SM_EMPTY) return 0;
        region.key = (nvn_backing_key){.kind = NVN_BACKING_OBJECT, .second = info.object_id_full};
    } else {
#if !TARGET_OS_IPHONE
        if (!nvn_backing_file_mode(info.share_mode)) return 0;
        struct proc_regionwithpathinfo path; memset(&path, 0, sizeof path);
        if (proc_pidinfo(getpid(), PROC_PIDREGIONPATHINFO, address, &path, sizeof path) != sizeof path) return 0;
        const struct proc_regioninfo *file = &path.prp_prinfo;
        const struct vinfo_stat *status = &path.prp_vip.vip_vi.vi_stat;
        if (!S_ISREG(status->vst_mode) || !status->vst_ino || file->pri_shadow_depth ||
            !nvn_backing_file_mode(file->pri_share_mode) || file->pri_address > address ||
            address - file->pri_address >= file->pri_size || file->pri_offset > UINT64_MAX - (address - file->pri_address) ||
            file->pri_offset + address - file->pri_address != info.offset + address - base) return 0;
        region.key = (nvn_backing_key){.kind = NVN_BACKING_FILE,
            .first = (uint64_t)status->vst_dev << 32 | status->vst_gen, .second = status->vst_ino};
#else
        return 0;
#endif
    }
    *result = region; return 1;
}
static inline int nvn_backing_cached(nvn_backing_cache *cache, uint64_t generation, uint64_t address, nvn_backing_region *result) {
    if (cache->generation != generation) { cache->generation = generation; cache->count = cache->next = 0; }
    for (unsigned i = 0; i < cache->count; i++) {
        const nvn_backing_region *entry = &cache->entries[i];
        if (address >= entry->start && address < entry->end) { *result = *entry; return 1; }
    }
    if (!nvn_backing_query(address, result)) return 0;
    cache->entries[cache->count < NVN_BACKING_CACHE ? cache->count++ : cache->next++ % NVN_BACKING_CACHE] = *result;
    return 1;
}
static inline int nvn_backing_prefix(nvn_backing_cache *cache, uint64_t generation, uint64_t address,
                                    uint64_t length, nvn_backing_range *range, uint64_t *consumed) {
    nvn_backing_region region;
    if (!length || address > UINT64_MAX - length || !nvn_backing_cached(cache, generation, address, &region)) return 0;
    uint64_t bytes = region.end - address; if (bytes > length) bytes = length;
    uint64_t offset = region.offset + address - region.start;
    if (offset > UINT64_MAX - bytes) return 0;
    *range = (nvn_backing_range){region.key, offset, offset + bytes}; *consumed = bytes; return 1;
}
static inline int nvn_backing_disjoint(nvn_backing_range a, nvn_backing_range b) {
    if (!a.key.kind || !b.key.kind || a.lo >= a.hi || b.lo >= b.hi) return 0;
    if (!nvn_backing_key_equal(a.key, b.key)) {
        // Different regular files cannot alias writes. Different VM objects
        // may still share backing through a pager, so retain that boundary.
        return a.key.kind == NVN_BACKING_FILE && b.key.kind == NVN_BACKING_FILE;
    }
    return a.lo >= b.hi || b.lo >= a.hi;
}
