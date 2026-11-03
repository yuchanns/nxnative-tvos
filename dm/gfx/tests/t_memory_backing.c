#include "../nvn/nvn_memory_backing.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks, failed;
#define CHECK(label, value) do { checks++; if (!(value)) { failed++; fprintf(stderr, "FAIL %s\n", label); } } while (0)
static nvn_backing_cache cache;
static nvn_backing_range range(uint64_t address, uint64_t bytes) {
    nvn_backing_range result = {0}; uint64_t consumed = 0;
    CHECK("backing query covers its bounded range", nvn_backing_prefix(&cache, 1, address, bytes, &result, &consumed) && consumed == bytes);
    return result;
}
int main(void) {
    uint64_t page = (uint64_t)getpagesize(); mach_vm_address_t memory = 0, alias = 0;
    vm_prot_t current, maximum;
    if (mach_vm_allocate(mach_task_self(), &memory, 2 * page, VM_FLAGS_ANYWHERE) ||
        mach_vm_remap(mach_task_self(), &alias, page, 0, VM_FLAGS_ANYWHERE, mach_task_self(), memory, FALSE,
            &current, &maximum, VM_INHERIT_NONE)) abort();
    memset((void *)(uintptr_t)memory, 0, 2 * page);
    nvn_backing_range first = range(memory + 32, 32), same = range(alias + 32, 32), later = range(memory + page + 32, 32);
    CHECK("distinct addresses preserve anonymous aliases", nvn_backing_key_equal(first.key, same.key) && first.lo == same.lo);
    CHECK("anonymous aliases are not disjoint", !nvn_backing_disjoint(first, same));
    CHECK("different offsets in one object are disjoint", nvn_backing_disjoint(first, later));
    CHECK("unknown ranges are not disjoint", !nvn_backing_disjoint(first, (nvn_backing_range){0}));
    nvn_backing_range unknown = first; unknown.key.second++;
    CHECK("different anonymous objects remain conservative", !nvn_backing_disjoint(first, unknown));
    char path[] = "/tmp/nxnative-backing-XXXXXX";
    int file = mkstemp(path); if (file < 0 || ftruncate(file, 2 * page)) abort();
    uint8_t *shared = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
    uint8_t *other = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, file, (off_t)page);
    uint8_t *private = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, file, 0);
    if (shared == MAP_FAILED || other == MAP_FAILED || private == MAP_FAILED) abort();
    unlink(path); close(file); memset(shared, 0, 2 * page);
    nvn_backing_range file_first = range((uintptr_t)shared + 32, 32);
    nvn_backing_range file_second = range((uintptr_t)shared + page + 32, 32);
    nvn_backing_range file_alias = range((uintptr_t)other + 32, 32);
    CHECK("unlinked mapped files retain stable identity", file_first.key.kind == NVN_BACKING_FILE);
    CHECK("file offsets match a separate offset mapping", nvn_backing_key_equal(file_second.key, file_alias.key) && file_second.lo == file_alias.lo);
    CHECK("file aliases are not disjoint", !nvn_backing_disjoint(file_second, file_alias));
    CHECK("separate file offsets are disjoint", nvn_backing_disjoint(file_first, file_second));
    nvn_backing_region region;
    CHECK("copy-on-write mappings are not proved", !nvn_backing_query((uintptr_t)private, &region));
    private[0] = 1;
    CHECK("a private copy is never treated as shared file storage", !nvn_backing_query((uintptr_t)private, &region) || region.key.kind != NVN_BACKING_FILE);
    char other_path[] = "/tmp/nxnative-backing-other-XXXXXX";
    int other_file = mkstemp(other_path); if (other_file < 0 || ftruncate(other_file, page)) abort();
    uint8_t *separate = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, other_file, 0);
    if (separate == MAP_FAILED) abort();
    unlink(other_path); close(other_file); separate[0] = 1;
    nvn_backing_range disjoint_file = range((uintptr_t)separate + 32, 32);
    CHECK("a single shared file mapping is identified", disjoint_file.key.kind == NVN_BACKING_FILE);
    CHECK("different regular file identities are disjoint", nvn_backing_disjoint(file_first, disjoint_file));
    munmap(separate, page);
    CHECK("mixed backing types remain conservative", !nvn_backing_disjoint(first, file_first));
    uint64_t consumed;
    CHECK("overflowing ranges fail shut", !nvn_backing_prefix(&cache, 1, UINT64_MAX - 2, 8, &same, &consumed));
    CHECK("zero-sized ranges fail shut", !nvn_backing_prefix(&cache, 1, memory, 0, &same, &consumed));
    cache.entries[0].key.kind = 0;
    CHECK("mapping generations invalidate cached identities", nvn_backing_cached(&cache, 2, memory + 32, &region) && region.key.kind != 0);
    munmap(shared, 2 * page); munmap(other, page); munmap(private, page);
    mach_vm_deallocate(mach_task_self(), alias, page); mach_vm_deallocate(mach_task_self(), memory, 2 * page);
    fprintf(stderr, "t_memory_backing: %u checks, %u failed\n", checks, failed); return failed != 0;
}
