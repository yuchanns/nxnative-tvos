#include "xc3_heap_pages.h"
#include "xc3_sparse_arena.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define UNIT ((size_t)65536)
#define OK(call) assert((call) == XC3_ALLOCATOR_OK)

static xc3_heap_page_stats stats(xc3_heap_pages *pages) {
    xc3_heap_page_stats value;
    OK(xc3_heap_pages_stats(pages, &value));
    assert(!value.poisoned);
    return value;
}

int main(void) {
    size_t page = (size_t)getpagesize(), heap_bytes = 16 * UNIT;
    void *mapping = mmap(NULL, heap_bytes + UNIT, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void *alias = mmap(NULL, page * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(mapping != MAP_FAILED && alias != MAP_FAILED);
    uintptr_t heap = ((uintptr_t)mapping + UNIT - 1) & ~(uintptr_t)(UNIT - 1);
    const char *directory = getenv("TMPDIR");
    assert(directory && directory[0] == '/');
    xc3_heap_pages_config config = {
        .heap_base = (void *)heap, .heap_bytes = heap_bytes,
        .alias_base = alias, .alias_bytes = page * 2,
        .host_page_bytes = page, .commit_limit = 4 * UNIT,
        .file_backed_heap = true, .file_backing_directory = directory,
    };
    xc3_heap_pages *pages;
    OK(xc3_heap_pages_create(&config, &pages));
    xc3_sparse_arena_config arena_config = {
        .base = (void *)heap, .span_bytes = heap_bytes,
        .commit_limit = config.commit_limit, .page_size = page,
        .vm = {pages, xc3_heap_pages_commit, xc3_heap_pages_decommit},
    };
    xc3_sparse_arena *arena;
    OK(xc3_sparse_arena_create(&arena_config, &arena));
    xc3_allocator_backend backend = xc3_sparse_arena_backend(arena);
    void *first, *second;
    OK(backend.allocate(backend.context, 2 * UNIT, UNIT, UNIT, &first));
    memset(first, 0xa7, UNIT);
    xc3_heap_page_span span;
    OK(xc3_heap_pages_query(pages, (uintptr_t)first + UNIT, &span));
    assert(!span.committed);
    xc3_sparse_lease lease;
    OK(xc3_sparse_arena_acquire_lease(arena, (uintptr_t)first, UNIT, &lease));
    OK(backend.free(backend.context, first));
    assert(stats(pages).heap_committed_bytes == UNIT);
    assert(*(unsigned char *)first == 0xa7);
    assert(xc3_sparse_arena_shutdown(arena) == XC3_ALLOCATOR_BUSY);
    OK(backend.allocate(backend.context, 2 * UNIT, UNIT, UNIT, &second));
    assert(first != second);
    OK(backend.free(backend.context, second));
    void *unchanged = (void *)(uintptr_t)0x1234;
    assert(backend.allocate(backend.context, 4 * UNIT, 4 * UNIT, UNIT, &unchanged) == XC3_ALLOCATOR_OUT_OF_MEMORY);
    assert(unchanged == (void *)(uintptr_t)0x1234);
    assert(stats(pages).heap_committed_bytes == UNIT);
    OK(xc3_sparse_arena_release_lease(arena, lease));
    assert(xc3_sparse_arena_release_lease(arena, lease) == XC3_ALLOCATOR_NOT_FOUND);
    assert(!stats(pages).heap_committed_bytes);
    OK(xc3_heap_pages_query(pages, (uintptr_t)first, &span));
    assert(!span.committed);
    OK(backend.allocate(backend.context, 2 * UNIT, UNIT, UNIT, &second));
    assert(first == second);
    for (size_t offset = 0; offset < UNIT; offset++) assert(!((unsigned char *)second)[offset]);
    xc3_sparse_lease current;
    OK(xc3_sparse_arena_acquire_lease(arena, (uintptr_t)second, UNIT, &current));
    assert(current.slot == lease.slot && current.generation != lease.generation);
    assert(xc3_sparse_arena_release_lease(arena, lease) == XC3_ALLOCATOR_NOT_FOUND);
    OK(xc3_sparse_arena_release_lease(arena, current));
    OK(backend.free(backend.context, second));

    // Two Horizon mappings share a host page; unmapping one retains the other.
    OK(xc3_heap_pages_map_alias(pages, (uintptr_t)alias, 4096, true));
    OK(xc3_heap_pages_map_alias(pages, (uintptr_t)alias + 4096, 4096, true));
    assert(stats(pages).alias_committed_bytes == (page > 4096 ? page : 8192));
    ((unsigned char *)alias)[4096] = 0x5b;
    OK(xc3_heap_pages_map_alias(pages, (uintptr_t)alias, 4096, false));
    assert(((unsigned char *)alias)[4096] == 0x5b);
    OK(xc3_heap_pages_map_alias(pages, (uintptr_t)alias + 4096, 4096, false));
    assert(!stats(pages).alias_committed_bytes);
    OK(xc3_sparse_arena_shutdown(arena));
    OK(xc3_sparse_arena_destroy(arena));
    OK(xc3_heap_pages_destroy(pages));

    // A storage error must not become OOM or publish any mapping/accounting.
    char missing[4096];
    int length = snprintf(missing, sizeof missing, "%s/absent-%ld/backing", directory, (long)getpid());
    assert(length > 0 && (size_t)length < sizeof missing);
    config.file_backing_directory = missing;
    OK(xc3_heap_pages_create(&config, &pages));
    assert(xc3_heap_pages_commit(pages, (void *)heap, UNIT) == XC3_ALLOCATOR_BACKING_IO_ERROR);
    assert(!stats(pages).heap_committed_bytes && !stats(pages).file_live_mappings);
    OK(xc3_heap_pages_query(pages, heap, &span));
    assert(!span.committed);
    OK(xc3_heap_pages_destroy(pages));
    assert(!munmap(mapping, heap_bytes + UNIT));
    assert(!munmap(alias, page * 2));
    puts("sparse heap: lease retirement/ABA, quota rollback, zero reuse, alias union and file failure passed");
    return 0;
}
