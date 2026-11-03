"""Validate lossless snapshot block/plane iteration on the CPU with sanitizers."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "nvn_texture_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned cases;
static void check(nvn_snapshot_layout layout) {
    uint64_t expected = 0;
    for (unsigned p = 0; p < layout.planes; p++) for (unsigned l = 0; l < layout.levels; l++) {
        unsigned w = layout.width >> l, h = layout.height >> l; if (!w) w = 1; if (!h) h = 1;
        nvn_snapshot_plane b = layout.plane[p];
        uint64_t row = ((w + b.block_width - 1) / b.block_width * b.bytes + 255u) / 256u * 256u;
        unsigned depth = layout.depth >> l;
        unsigned slices = layout.depth ? (depth ? depth : 1) : layout.layers;
        expected += row * ((h + b.block_height - 1) / b.block_height) * slices;
    }
    assert(expected == nvn_snapshot_size(&layout));
    nvn_snapshot_cursor cursor = {0}; nvn_snapshot_chunk chunk;
    uint64_t offset = 0; unsigned chunks = 0;
    while (1) {
        nvn_snapshot_cursor before = cursor;
        if (!nvn_snapshot_next(&layout, &cursor, &chunk)) break;
        nvn_snapshot_plane plane = layout.plane[before.plane];
        unsigned width = layout.width >> chunk.level, height = layout.height >> chunk.level;
        if (!width) width = 1; if (!height) height = 1;
        assert(chunk.offset == offset && chunk.bytes > 0 && chunk.bytes <= NVN_SNAPSHOT_CHUNK);
        assert(chunk.width == width && chunk.y + chunk.height <= height);
        assert(chunk.y % plane.block_height == 0);
        assert(chunk.height % plane.block_height == 0 || chunk.y + chunk.height == height);
        assert(chunk.row_bytes % 256 == 0 && chunk.options == plane.options);
        assert(chunk.bytes == (uint64_t)chunk.row_bytes * ((chunk.height + plane.block_height - 1) / plane.block_height));
        assert(chunk.slice < layout.layers && chunk.level < layout.levels);
        if (layout.depth) assert(!chunk.slice && chunk.z < nvn_snapshot_dim(layout.depth, chunk.level));
        else assert(!chunk.z);
        offset += chunk.bytes; assert(cursor.offset == offset); assert(++chunks < 100000);
        // A canceled transfer must leave the published cursor unchanged.
        nvn_snapshot_cursor copy = before; nvn_snapshot_chunk again;
        assert(nvn_snapshot_next(&layout, &copy, &again));
        assert(memcmp(&again, &chunk, sizeof chunk) == 0);
    }
    assert(offset == expected && cursor.plane == layout.planes); cases++;
}
int main(void) {
    unsigned sizes[][2] = {{1,1},{3,7},{17,19},{768,768},{1920,1080},{2048,2048},{16384,1}};
    for (unsigned s=0; s<sizeof sizes/sizeof *sizes; s++) for (unsigned b=1;b<=6;b++) {
        if (b!=1 && b!=4 && b!=6) continue;
        unsigned levels=1, dim=sizes[s][0]>sizes[s][1]?sizes[s][0]:sizes[s][1]; while(dim>>=1) levels++;
        nvn_snapshot_layout layout={sizes[s][0],sizes[s][1],levels,2,1,{{b,b,b==1?4:16,0}},0}; check(layout);
        layout.planes=2;layout.plane[0]=(nvn_snapshot_plane){1,1,4,1};layout.plane[1]=(nvn_snapshot_plane){1,1,1,2};check(layout);
        layout.layers=1;layout.depth=5;check(layout);
    }
    nvn_snapshot_layout too_big={16384,16384,1,1,1,{{1,1,4,0}},0};assert(!nvn_snapshot_size(&too_big));
    nvn_snapshot_layout bad={1,1,1,1,1,{{1,1,4,0}},0};bad.plane[0].block_width=0;assert(!nvn_snapshot_size(&bad));
    bad.plane[0].block_width=1;bad.planes=3;assert(!nvn_snapshot_size(&bad));
    bad.planes=1;bad.layers=2049;assert(!nvn_snapshot_size(&bad));
    assert(!nvn_snapshot_size(NULL));
    printf("snapshot layouts %u passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix="nvn-snapshot-cpu-") as directory:
    root=Path(directory);source=root/"snapshot.c";binary=root/"snapshot";source.write_text(SOURCE)
    subprocess.run(["clang","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-I",str(ROOT/"nvn"),str(source),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"))
