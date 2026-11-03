// CPU-only verification: access and cold advice preserve shared-file contents.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

static int test_madvise(void *address, size_t bytes, int advice);
#define madvise test_madvise
#include "../hle_guest_file_backing.c"
#undef madvise

static int checks, failed, advice_calls;
static int advice_error;
static void *last_address;
static size_t last_bytes;
static int last_advice;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static int test_madvise(void *address, size_t bytes, int advice) {
    advice_calls++;
    last_address = address; last_bytes = bytes; last_advice = advice;
    if (advice_error) { errno = advice_error; return -1; }
    return madvise(address, bytes, advice);
}

static void check_region(const void *pointer, vm_prot_t protection, vm_behavior_t behavior) {
    mach_vm_address_t address = (mach_vm_address_t)pointer, queried = address;
    mach_vm_size_t bytes = 0;
    vm_region_basic_info_data_64_t info = {0};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t result = mach_vm_region(mach_task_self(), &address, &bytes,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    CHECK(result == KERN_SUCCESS);
    if (result == KERN_SUCCESS) {
        CHECK(address <= queried && queried - address < bytes);
        CHECK(info.protection == protection);
        CHECK(info.behavior == behavior);
    }
    if (object != MACH_PORT_NULL) CHECK(mach_port_deallocate(mach_task_self(), object) == KERN_SUCCESS);
}

typedef struct { uint32_t *data; size_t count; _Atomic int ready; } write_job;
static void *write_pages(void *argument) {
    write_job *job = argument;
    atomic_store(&job->ready, 1);
    for (unsigned pass = 0; pass < 128; pass++)
        for (size_t i = 0; i < job->count; i++) job->data[i] = (uint32_t)i ^ (pass * 0x10101u);
    return NULL;
}

int main(void) {
    size_t page = (size_t)getpagesize(), size = 4 * page;
    uint8_t *base = mmap(NULL, size + 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(base != MAP_FAILED);
    if (base == MAP_FAILED) return 1;
    uint8_t *memory = base + page;
    int fd = dm_guest_file_backing_map(memory, size, "/tmp");
    CHECK(fd >= 0);
    if (fd < 0) return 1;
    check_region(memory, VM_PROT_NONE, VM_BEHAVIOR_DEFAULT);
    CHECK(dm_guest_file_backing_access(NULL, size, 1) == -1 && errno == EINVAL);
    CHECK(dm_guest_file_backing_access(memory, 0, 1) == -1 && errno == EINVAL);
    CHECK(dm_guest_file_backing_access(memory + 1, size, 1) == -1 && errno == EINVAL);
    CHECK(dm_guest_file_backing_access(memory, size - 1, 1) == -1 && errno == EINVAL);
    CHECK(dm_guest_file_backing_access(memory, size, 2) == -1 && errno == EINVAL);
    CHECK(dm_guest_file_backing_access(memory, size, -1) == -1 && errno == EINVAL);
    CHECK(dm_guest_file_backing_access((void *)(UINTPTR_MAX - page + 1), page, 1) == -1 && errno == EINVAL);
    CHECK(advice_calls == 0);
    advice_error = EIO;
    CHECK(dm_guest_file_backing_access(memory, size, 1) == -1 && errno == EIO);
    CHECK(advice_calls == 1);
    check_region(memory, VM_PROT_NONE, VM_BEHAVIOR_DEFAULT);
    advice_error = 0;
    CHECK(!dm_guest_file_backing_access(memory, size, 1));
    CHECK(last_address == memory && last_bytes == size && last_advice == MADV_RANDOM);
    check_region(memory, VM_PROT_NONE, VM_BEHAVIOR_RANDOM);
    check_region(base, VM_PROT_NONE, VM_BEHAVIOR_DEFAULT);
    check_region(memory + size, VM_PROT_NONE, VM_BEHAVIOR_DEFAULT);
    CHECK(!mprotect(memory, size, PROT_READ | PROT_WRITE));
    for (size_t i = 0; i < size; i++) memory[i] = (uint8_t)(i * 73 + 19);
    CHECK(!mprotect(memory + page, page, PROT_READ));
    for (size_t i = 0; i < 4; i++)
        check_region(memory + i * page, i == 1 ? VM_PROT_READ : VM_PROT_READ | VM_PROT_WRITE,
                     VM_BEHAVIOR_RANDOM);
    CHECK(!mprotect(memory + page, page, PROT_READ | PROT_WRITE));
    advice_calls = 0;

    CHECK(!dm_guest_file_backing_cold(NULL, size, memory, size));
    CHECK(!dm_guest_file_backing_cold(memory, size, NULL, size));
    CHECK(!dm_guest_file_backing_cold(memory, size, memory - 1, size));
    CHECK(!dm_guest_file_backing_cold(memory, size, memory, size + 1));
    CHECK(!dm_guest_file_backing_cold(memory, size, memory + size, 1));
    CHECK(!dm_guest_file_backing_cold(memory, size, memory + 1, page - 1));
    CHECK(!dm_guest_file_backing_cold(memory, size, memory, 0));
    CHECK(!dm_guest_file_backing_cold((void *)(UINTPTR_MAX - page), 2 * page,
                                     (void *)(UINTPTR_MAX - page), page));
    CHECK(advice_calls == 0);

    CHECK(dm_guest_file_backing_cold(memory, size, memory + 1, 3 * page + 7) == 2 * page);
    CHECK(advice_calls == 1 && last_address == memory + page && last_bytes == 2 * page);
    CHECK(last_advice == MADV_DONTNEED);
    check_region(memory + page, VM_PROT_READ | VM_PROT_WRITE, VM_BEHAVIOR_RANDOM);
    int intact = 1;
    for (size_t i = 0; i < size; i++) intact &= memory[i] == (uint8_t)(i * 73 + 19);
    CHECK(intact);
    CHECK(!mprotect(memory, size, PROT_READ));
    CHECK(dm_guest_file_backing_cold(memory, size, memory, size) == size);
    uint8_t *disk = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(disk != MAP_FAILED);
    if (disk != MAP_FAILED) CHECK(!memcmp(memory, disk, size));
    if (disk != MAP_FAILED) check_region(disk, VM_PROT_READ, VM_BEHAVIOR_DEFAULT);
    CHECK(!dm_guest_file_backing_access(memory, size, 0));
    CHECK(last_advice == MADV_NORMAL);
    for (size_t i = 0; i < 4; i++) check_region(memory + i * page, VM_PROT_READ, VM_BEHAVIOR_DEFAULT);
    if (disk != MAP_FAILED) CHECK(!memcmp(memory, disk, size));
    CHECK(!dm_guest_file_backing_access(memory, size, 1));
    CHECK(!mprotect(memory, size, PROT_READ | PROT_WRITE));

    write_job job = {.data = (uint32_t *)memory, .count = size / sizeof(uint32_t)};
    pthread_t writer;
    int created = !pthread_create(&writer, NULL, write_pages, &job);
    CHECK(created);
    if (created) {
        while (!atomic_load(&job.ready)) sched_yield();
        for (unsigned i = 0; i < 128; i++)
            CHECK(dm_guest_file_backing_cold(memory, size, memory, size) == size);
        CHECK(!pthread_join(writer, NULL));
        intact = 1;
        for (size_t i = 0; i < job.count; i++) intact &= job.data[i] == ((uint32_t)i ^ (127 * 0x10101u));
        CHECK(intact);
        if (disk != MAP_FAILED) CHECK(!memcmp(memory, disk, size));
    }
    check_region(memory, VM_PROT_READ | VM_PROT_WRITE, VM_BEHAVIOR_RANDOM);
    CHECK(!dm_guest_file_backing_access(memory, size, 0));
    check_region(memory, VM_PROT_READ | VM_PROT_WRITE, VM_BEHAVIOR_DEFAULT);
    if (disk != MAP_FAILED) CHECK(!munmap(disk, size));
    CHECK(!close(fd)); CHECK(!munmap(base, size + 2 * page));
    printf("Guest file advice checks=%d failed=%d\n", checks, failed);
    return failed != 0;
}
