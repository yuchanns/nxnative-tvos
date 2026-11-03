"""Check production ASTC restore eligibility without creating a Metal device."""

import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "nvn/nvn_mtl.m").read_text()


def function(name):
    match = re.search(r"^static [^\n]+\b" + name + r"\([^\n]*\) \{", SOURCE, re.M)
    if not match:
        raise AssertionError("Missing production function " + name)
    start = SOURCE.index("{", match.start())
    depth = 0
    for end in range(start, len(SOURCE)):
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        if not depth:
            return SOURCE[match.start():end + 1]
    raise AssertionError("Unterminated production function " + name)


PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NVN_TEXTURE_FLAGS_IMAGE = 4 };
typedef struct {
    struct { unsigned flags, width, height, levels; } d;
    struct { uint64_t size; } L;
    const uint8_t *mem;
    const void *pool;
} tex_t;
typedef struct {
    const void *astc_keys, *span;
    int reloadable, sparse;
    tex_t source;
    uint64_t seen, stamp, hash;
} NvPkg;
static unsigned checks, failures, dirty_calls;
static int dirty_result, verify_hash;
static const void *expected_span;
static const uint8_t *expected_memory;
static uint64_t expected_size, expected_stamp;
#define CHECK(value) do { checks++; if (!(value)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)
static int dirtycheck(void) { return verify_hash; }
static int nvn_dirty_since_span(const void *span, const uint8_t *memory, uint64_t size, uint64_t stamp) {
    dirty_calls++;
    CHECK(span == expected_span && memory == expected_memory);
    CHECK(size == expected_size && stamp == expected_stamp);
    return dirty_result;
}
'''


TEST = r'''
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "unset")) unsetenv("DM_NVN_ASSET_PURGE_IMAGES");
    else setenv("DM_NVN_ASSET_PURGE_IMAGES", argv[1], 1);
    const int images = strcmp(argv[1], "0") != 0;
    uint8_t memory[64] = {0}, replacement[64] = {0};
    int pool, other_pool, span, key;
    tex_t source = {.d = {0, 8, 8, 1}, .L.size = sizeof memory, .mem = memory, .pool = &pool};
    CHECK(pkg_cache_initial_reloadable(&source, 0));
    CHECK(!pkg_cache_initial_reloadable(&source, 1));
    source.d.flags = NVN_TEXTURE_FLAGS_IMAGE;
    CHECK(pkg_cache_initial_reloadable(&source, 0) == images);
    CHECK(!pkg_cache_initial_reloadable(&source, 1));
    NvPkg original = {.astc_keys = &key, .span = &span, .reloadable = 1,
                      .source = source, .seen = 23, .stamp = 11};
    original.hash = full_hash(memory, sizeof memory);
    expected_span = &span; expected_memory = memory;
    expected_size = sizeof memory; expected_stamp = 11;
    CHECK(pkg_astc_can_restore(&original, &source, 23));
    CHECK(!dirty_calls);
    CHECK(pkg_astc_can_restore(&original, &source, 24));
    CHECK(dirty_calls == 1);
    dirty_result = 1;
    CHECK(!pkg_astc_can_restore(&original, &source, 25));
    CHECK(original.stamp == 11 && original.seen == 23);
    dirty_result = 0;
    NvPkg candidate = original;
    candidate.astc_keys = NULL;
    CHECK(!pkg_astc_can_restore(&candidate, &source, 23));
    candidate = original; candidate.reloadable = 0;
    CHECK(!pkg_astc_can_restore(&candidate, &source, 23));
    candidate = original; candidate.sparse = 1;
    CHECK(!pkg_astc_can_restore(&candidate, &source, 23));
    candidate = original; candidate.span = NULL;
    CHECK(!pkg_astc_can_restore(&candidate, &source, 23));
    tex_t changed = source;
    changed.mem = replacement;
    CHECK(!pkg_astc_can_restore(&original, &changed, 23));
    changed = source; changed.pool = &other_pool;
    CHECK(!pkg_astc_can_restore(&original, &changed, 23));
    changed = source; changed.L.size--;
    CHECK(!pkg_astc_can_restore(&original, &changed, 23));
    changed = source; changed.d.width++;
    CHECK(!pkg_astc_can_restore(&original, &changed, 23));
    changed = source; changed.d.levels++;
    CHECK(!pkg_astc_can_restore(&original, &changed, 23));
    changed = source; changed.d.flags = 0;
    CHECK(!pkg_astc_can_restore(&original, &changed, 23));
    CHECK(dirty_calls == 2);
    verify_hash = 1;
    CHECK(pkg_astc_can_restore(&original, &source, 23));
    memory[31] = 0x7f;
    CHECK(!pkg_astc_can_restore(&original, &source, 23));
    memory[31] = 0;
    CHECK(pkg_astc_can_restore(&original, &source, 26));
    CHECK(original.stamp == 11 && original.seen == 23);
    printf("%s: %u checks, %u failures\n", argv[1], checks, failures);
    return failures != 0;
}
'''


with tempfile.TemporaryDirectory(prefix="nvn-asset-restore-") as directory:
    root = Path(directory)
    source = root / "restore.c"
    binary = root / "restore"
    source.write_text(PREFIX + "\n" + "\n".join(function(name) for name in (
        "pkg_cache_initial_reloadable", "full_hash", "pkg_astc_can_restore"
    )) + "\n" + TEST)
    subprocess.run([
        "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", str(source), "-o", str(binary)
    ], check=True)
    environment = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
    for value in ("0", "1", "unset"):
        subprocess.run([str(binary), value], env=environment, check=True)
