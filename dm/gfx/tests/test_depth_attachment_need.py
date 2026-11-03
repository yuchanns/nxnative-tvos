"""Exercise production depth/stencil lookahead using CPU command records."""

import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "nvn/nvn_mtl.m").read_text()
OBJECTS = (ROOT / "nvn/nvn_obj.h").read_text()
API = (ROOT / "nvn/include/nvn.h").read_text()


def function(name):
    match = re.search(r"^static [^\n]+\b" + name + r"\([^\n]*\) \{", SOURCE, re.M)
    assert match, name
    start = SOURCE.index("{", match.start())
    depth = 0
    for end in range(start, len(SOURCE)):
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        if not depth:
            return SOURCE[match.start():end + 1] + "\n"
    raise AssertionError(name)


types = []
for name in ("ds_st", "op_hdr", "p_bind_state"):
    match = re.search(r"typedef struct \{[^{}]+\} " + name + r";", OBJECTS)
    assert match, name
    types.append(match[0])
opcodes = re.search(r"enum \{\s*OP_END,[^}]+\};", OBJECTS)
assert opcodes
depth_functions = re.search(r"typedef enum \{[^{}]+\} NVNdepthFunc;", API)
assert depth_functions

PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PD(...) ((void)0)
'''

GLOBALS = r'''
enum { DS_FREE = 0, DS_USED = 1, DS_RET = 2 };
typedef struct { op_hdr header; union { p_bind_state bind; uint64_t target; } payload; } command;
static command commands[64], child[12][3], caller[3], next_handle[3];
static unsigned command_count;
static struct { struct { void *depth; } rt; int clear_d, clear_s; } R;
static struct { ds_st ds; } S;
static const uint8_t *g_exec_next, *g_ret[17];
static uint64_t g_rest[3];
static int g_ret_n, g_nrest, g_hoist_n, g_depth_attachments;
static int g_ds_why, g_ds_scanned, g_ds_need;
static int hoist_skipped(const uint8_t *p) { (void)p; return 0; }
static command operation(unsigned op) {
    return (command){.header = {op, sizeof(command)}};
}
static command comparison_binding(int depth, int write, int stencil, int compare) {
    command c = operation(OP_BIND_STATE);
    c.payload.bind.kind = 5;
    ds_st state = {.dt = depth, .dw = write, .st = stencil, .df = compare};
    memcpy(c.payload.bind.bytes, &state, sizeof state);
    return c;
}
static command binding(int depth, int write, int stencil) {
    return comparison_binding(depth, write, stencil, NVN_DEPTH_FUNC_LESS);
}
static command transfer(unsigned op, command *target) {
    command c = operation(op); c.payload.target = (uintptr_t)target; return c;
}
static void append(command c) {
    assert(command_count + 1 < sizeof commands / sizeof *commands);
    commands[command_count++] = c;
    commands[command_count] = operation(OP_END);
}
static void reset(int depth, int write, int stencil) {
    memset(&R, 0, sizeof R); memset(commands, 0, sizeof commands);
    memset(child, 0, sizeof child); memset(caller, 0, sizeof caller);
    memset(next_handle, 0, sizeof next_handle);
    R.rt.depth = &R;
    S.ds = (ds_st){.dt = depth, .dw = write, .st = stencil, .df = NVN_DEPTH_FUNC_LESS};
    command_count = 0; commands[0] = operation(OP_END);
    g_exec_next = (const uint8_t *)commands;
    g_ret_n = g_nrest = g_hoist_n = 0; g_depth_attachments = 1;
}
'''

CASES = r'''
int main(void) {
    // The write mask has no effect while the depth test is disabled.
    for (unsigned state = 0; state < 32; ++state) for (unsigned compare = 1; compare <= 8; ++compare) {
        int depth = state & 1, write = (state >> 1) & 1, stencil = (state >> 2) & 1;
        reset(depth, write, stencil); R.clear_d = (state >> 3) & 1; R.clear_s = (state >> 4) & 1;
        S.ds.df = compare;
        int reads_or_writes = depth && (write || compare != NVN_DEPTH_FUNC_ALWAYS);
        unsigned expected = (reads_or_writes || R.clear_d ? 1u : 0u) | (stencil || R.clear_s ? 2u : 0u);
        assert(ds_attachment_need(&S.ds) == ((unsigned)reads_or_writes | ((unsigned)stencil << 1)));
        assert(pass_ds_need() == (int)expected);
        g_depth_attachments = 0;
        assert(pass_ds_need() == (int)(expected | (depth || write ? 1u : 0u)));
    }
    reset(1, 0, 0); S.ds.df = NVN_DEPTH_FUNC_ALWAYS;
    append(comparison_binding(1, 0, 0, NVN_DEPTH_FUNC_ALWAYS)); append(operation(OP_DRAW));
    assert(pass_ds_need() == 0);
    append(binding(1, 0, 0)); append(operation(OP_DRAW)); assert(pass_ds_need() == 1);
    reset(1, 0, 1); S.ds.df = NVN_DEPTH_FUNC_ALWAYS; assert(pass_ds_need() == 2);
    append(comparison_binding(1, 1, 0, NVN_DEPTH_FUNC_ALWAYS)); append(operation(OP_DRAW_IDX));
    assert(pass_ds_need() == 3);
    reset(1, 0, 0); S.ds.df = NVN_DEPTH_FUNC_ALWAYS;
    R.clear_d = 1; assert(pass_ds_need() == 1);
    reset(0, 1, 0); append(binding(0, 1, 0)); append(operation(OP_DRAW_IDX));
    assert(pass_ds_need() == 0);
    reset(0, 1, 0); append(binding(1, 0, 0)); append(operation(OP_DRAW));
    assert(pass_ds_need() == 1);
    reset(0, 1, 0); append(binding(0, 0, 1)); append(operation(OP_DRAW_IDX));
    assert(pass_ds_need() == 2);
    reset(1, 0, 0); append(binding(0, 1, 0)); append(operation(OP_DRAW));
    assert(pass_ds_need() == 1);
    reset(0, 0, 1); append(binding(1, 0, 0)); append(operation(OP_DRAW));
    assert(pass_ds_need() == 3);
    // A state change without a draw does not access a depth attachment.
    reset(0, 1, 0); append(binding(1, 1, 1)); append(operation(OP_SET_RT));
    assert(pass_ds_need() == 0);
    unsigned boundaries[] = {OP_SET_RT, OP_COPY_B2B, OP_CLEAR_BUF, OP_UPDATE_UB,
        OP_COPY_B2T, OP_COPY_T2B, OP_COPY_T2T, OP_CLEAR_TEX, OP_DISPATCH};
    for (unsigned i = 0; i < sizeof boundaries / sizeof *boundaries; ++i) {
        reset(0, 1, 0); append(operation(boundaries[i]));
        append(binding(1, 1, 1)); append(operation(OP_DRAW));
        assert(pass_ds_need() == 0);
    }
    reset(0, 1, 0); append(operation(OP_CLEAR_DS)); assert(pass_ds_need() == 3);
    reset(0, 1, 0); append(operation(OP_CLEAR_COLOR)); assert(pass_ds_need() == 3);
    reset(0, 1, 0);
    child[0][0] = binding(1, 0, 0); child[0][1] = operation(OP_DRAW);
    child[0][2] = operation(OP_END); append(transfer(OP_CALL, child[0]));
    assert(pass_ds_need() == 1);
    // State from a sub-list persists after its return.
    reset(0, 1, 0);
    child[0][0] = binding(0, 0, 1); child[0][1] = operation(OP_END);
    append(transfer(OP_CALL, child[0])); append(operation(OP_DRAW));
    assert(pass_ds_need() == 2);
    reset(0, 1, 0);
    child[0][0] = binding(1, 0, 0); child[0][1] = operation(OP_DRAW_IDX);
    child[0][2] = operation(OP_END); append(transfer(OP_JUMP, child[0]));
    assert(pass_ds_need() == 1);
    reset(0, 1, 0);
    caller[0] = binding(1, 0, 0); caller[1] = operation(OP_DRAW); caller[2] = operation(OP_END);
    g_ret[0] = (const uint8_t *)caller; g_ret_n = 1;
    assert(pass_ds_need() == 1);
    reset(0, 1, 0);
    next_handle[0] = binding(0, 1, 1); next_handle[1] = operation(OP_DRAW_IDX);
    next_handle[2] = operation(OP_END); g_rest[0] = (uintptr_t)next_handle; g_nrest = 1;
    assert(pass_ds_need() == 2);
    reset(0, 1, 0);
    for (unsigned i = 0; i < 11; ++i) {
        child[i][0] = transfer(OP_CALL, child[i + 1]); child[i][1] = operation(OP_END);
    }
    child[11][0] = operation(OP_END); append(transfer(OP_CALL, child[0]));
    assert(pass_ds_need() == 3);
    reset(0, 1, 0); g_exec_next = NULL; assert(pass_ds_need() == 3);
    reset(0, 1, 0); g_ret_n = 17; assert(pass_ds_need() == 3);
    reset(0, 1, 0); R.rt.depth = NULL; assert(pass_ds_need() == 3);
    puts("DEPTH_ATTACHMENTS OK: state masks, clears, later draws, boundaries, jumps, calls, conservative fallback");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="nvn-depth-attachments-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(PREAMBLE + "\n".join(types) + opcodes[0] + depth_functions[0] + GLOBALS +
                      "".join(function(name) for name in ("ds_attachment_need", "ds_scan", "pass_ds_need")) + CASES)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-g", "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
