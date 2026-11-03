"""Exercise the production color-attachment lookahead without Metal or a guest."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


PREAMBLE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define nil NULL
typedef struct { unsigned outputs; } NvProg;
typedef NvProg prog_t;
enum { OP_SET_RT, OP_BIND_PROGRAM, OP_DRAW, OP_DRAW_IDX, OP_CLEAR_COLOR, OP_OTHER };
typedef struct { unsigned op; } op_hdr;
typedef struct { unsigned stages; const prog_t *prog; } p_bind_program;
typedef struct { int index; } p_clear_color;
typedef struct { op_hdr op; p_bind_program bind; p_clear_color clear; } command;
typedef struct { const uint8_t *p, *ret[8]; int ret_n, bad; } fz_cursor;
static struct { int has_rt; struct { int n; void *color[8]; } rt; int clear_c[8]; } R;
static struct { const prog_t *prog; } S;
static const uint8_t *g_exec_next, *g_ret[8];
static int g_ret_n, scans, command_count, bad_end;
static command commands[16];
static NvProg programs[] = {{0}, {1}, {2}, {3}, {4}, {0xff}};
static unsigned program_color_outputs(NvProg *program) { return program ? program->outputs : 0xff; }
static NvProg *prog_get(const prog_t *program) { return (NvProg *)program; }
static int fz_next(fz_cursor *cursor, void *unused, op_hdr *op, const void **args) {
    (void)unused; scans++;
    const command *current = (const command *)cursor->p;
    if (current == commands + command_count) { cursor->bad = bad_end; return 0; }
    assert(current >= commands && current < commands + command_count);
    *op = current->op;
    *args = op->op == OP_BIND_PROGRAM ? (const void *)&current->bind : (const void *)&current->clear;
    cursor->p = (const uint8_t *)(current + 1);
    return 1;
}
static void reset(unsigned attached) {
    memset(&R, 0, sizeof R); memset(commands, 0, sizeof commands);
    R.has_rt = 1; R.rt.n = 8;
    for (int i = 0; i < 8; i++) if (attached & (1u << i)) R.rt.color[i] = &programs[0];
    S.prog = &programs[0]; g_exec_next = (const uint8_t *)commands;
    g_ret_n = scans = command_count = bad_end = 0;
}
static void operation(unsigned op) { commands[command_count++].op.op = op; }
static void bind(unsigned program, unsigned stages) {
    commands[command_count++] = (command){.op = {OP_BIND_PROGRAM}, .bind = {stages, &programs[program]}};
}
static void clear(int slot) {
    commands[command_count++] = (command){.op = {OP_CLEAR_COLOR}, .clear = {slot}};
}
"""

CASES = r"""
int main(void) {
    // Every subset of bound slots is already covered: no future command is read.
    for (unsigned slots = 0; slots < 256; slots++) {
        reset(slots); NvProg program = {slots}; operation(OP_OTHER);
        assert(pass_color_need(&program, 0) == 0xff && scans == 0);
    }
    reset(3); R.rt.n = 2; operation(OP_OTHER);
    assert(pass_color_need(&programs[2], 1) == 0xff && scans == 0);
    reset(5); R.clear_c[2] = 1;
    assert(pass_color_need(&programs[1], 0) == 0xff && scans == 0);
    reset(3); operation(OP_SET_RT); bind(2, 3); operation(OP_DRAW);
    assert(pass_color_need(&programs[1], 0) == 1 && scans == 1);
    reset(3); bind(2, 3); operation(OP_DRAW_IDX); operation(OP_OTHER);
    assert(pass_color_need(&programs[1], 0) == 0xff && scans == 2);
    reset(3); clear(1); operation(OP_OTHER);
    assert(pass_color_need(&programs[1], 0) == 0xff && scans == 1);
    reset(7); bind(2, 3); operation(OP_DRAW); operation(OP_SET_RT);
    assert(pass_color_need(&programs[1], 0) == 3 && scans == 3);
    reset(7); bind(2, 0x20); operation(OP_DRAW); operation(OP_SET_RT);
    assert(pass_color_need(&programs[1], 0) == 1);
    reset(7); S.prog = &programs[2]; operation(OP_DRAW); operation(OP_SET_RT);
    assert(pass_color_need(&programs[1], 0) == 3);
    reset(3); clear(7); operation(OP_SET_RT);
    assert(pass_color_need(&programs[1], 0) == 0x81);
    reset(3); clear(-1); assert(pass_color_need(&programs[1], 0) == 0xff);
    reset(3); clear(8); assert(pass_color_need(&programs[1], 0) == 0xff);
    reset(3); bad_end = 1; assert(pass_color_need(&programs[1], 0) == 0xff);
    reset(3); assert(pass_color_need(&programs[1], 0) == 1);
    reset(3); assert(pass_color_need(NULL, 0) == 0xff && scans == 0);
    reset(3); g_exec_next = NULL; assert(pass_color_need(&programs[1], 0) == 0xff);
    reset(3); g_ret_n = 9; assert(pass_color_need(&programs[1], 0) == 0xff && scans == 0);
    reset(3); g_ret_n = 8; operation(OP_SET_RT);
    assert(pass_color_need(&programs[1], 0) == 1);
    reset(3); R.has_rt = 0; assert(pass_color_need(&programs[1], 0) == 0xff);
    reset(3); R.rt.n = 9; assert(pass_color_need(&programs[1], 0) == 0xff);
    reset(3); R.rt.n = -1; assert(pass_color_need(&programs[1], 0) == 0xff);
    puts("Attachment lookahead: 256 covered masks and 20 boundary cases passed");
    return 0;
}
"""


class PassColorNeedTests(unittest.TestCase):
    def test_production_lookahead(self):
        source = Path(__file__).resolve().parents[1] / "nvn" / "nvn_mtl.m"
        text = source.read_text()
        start = text.index("static unsigned pass_color_need(")
        end = text.index("\n}\n", start) + 3
        with tempfile.TemporaryDirectory(prefix="nvn-color-lookahead-") as directory:
            fixture = Path(directory) / "lookahead.c"
            binary = Path(directory) / "lookahead"
            fixture.write_text(PREAMBLE + text[start:end] + CASES)
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-g", "-fsanitize=address,undefined", str(fixture), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
