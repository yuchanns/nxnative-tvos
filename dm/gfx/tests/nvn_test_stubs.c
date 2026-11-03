// Dense-pool fixtures for renderer-only tests. Tests linked with hle_nvn.c use
// its strong definitions, including the real sparse-pool implementation.
#include <mach/mach.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define WEAK __attribute__((weak))
WEAK size_t dm_guest_memory_cold(const void *address, size_t bytes) {
    (void)address; (void)bytes; return 0;
}
WEAK int dm_guest_storage_lease_acquire(const void *base, size_t bytes, void **lease) {
    (void)base; (void)bytes; (void)lease; return 0;
}
WEAK void dm_guest_storage_lease_release(void *lease) { (void)lease; }
WEAK uint64_t nvn_new_serial(void) { static _Atomic uint64_t serial = 1; return atomic_fetch_add(&serial, 1); }
WEAK int nvn_host_readable(uint64_t address, uint64_t length) {
    if (length > UINT64_MAX - address) return 0;
    uint64_t end = address + length;
    while (address < end) {
        vm_address_t region = address; vm_size_t size = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t result = vm_region_64(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS || region > address || !(info.protection & VM_PROT_READ) || size > UINT64_MAX - region || region + size <= address) return 0;
        address = region + size;
    }
    return 1;
}
WEAK int nvn_sparse_pool(const void *pool) { (void)pool; return 0; }
WEAK uint64_t nvn_sparse_gen(void) { return 1; }
static void sparse_unavailable(void) { fputs("sparse-pool tests must link hle_nvn.c\n", stderr); abort(); }
WEAK uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t address, uint64_t length) {
    (void)pool; (void)dst; (void)address; (void)length; sparse_unavailable(); return 0;
}
WEAK int nvn_sparse_since(const void *pool, uint64_t address, uint64_t length, uint64_t generation, uint64_t stamp) {
    (void)pool; (void)address; (void)length; (void)generation; (void)stamp; sparse_unavailable(); return 0;
}
WEAK int nvn_sparse_mapped(const void *pool, uint64_t address, uint64_t length) {
    (void)pool; (void)address; (void)length; sparse_unavailable(); return 0;
}
WEAK void nvn_sparse_protect(const void *pool, uint64_t address, uint64_t length) {
    (void)pool; (void)address; (void)length; sparse_unavailable();
}
