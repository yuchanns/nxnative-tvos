"""Check binding dependencies against scalar argument observations, without Metal."""

import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
renderer = (ROOT / "nvn/nvn_mtl.m").read_text()
objects = (ROOT / "nvn/nvn_obj.h").read_text()


def function(name):
    match = re.search(r"^static [^\n]*\b" + re.escape(name) + r"\([^;]*?\)\s*\{", renderer, re.M)
    start, opening = match.start(), match.end() - 1
    depth = 1
    end = opening + 1
    while depth:
        depth += (renderer[end] == "{") - (renderer[end] == "}")
        end += 1
    return renderer[start:end]


SOURCE = r'''
#include "nvn_argument_dependencies.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures;
#define CHECK(value) do { checks++; if (!(value)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)
static const int limits[5] = {16, 16, 256, 128, 32};

// Observe scalar resolver inputs, independently of dependency bitmaps. Both
// sides of optional combined/separate handles are exercised with zero inputs.
typedef struct { uint64_t slots[5][256]; } bindings;
static uint64_t fold(uint64_t hash, uint64_t value) {
    return (hash ^ value) * UINT64_C(1099511628211);
}
static uint64_t observe(const nvn_refl *reflection, int stage, const bindings *state) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (int i = 0; i < reflection->nb; i++) {
        const nvn_rbind *b = &reflection->b[i];
        unsigned reg = b->reg;
        uint64_t value = 0;
        switch (b->kind) {
        case NVN_RB_CBUF: case NVN_RB_UBOP: case NVN_RB_TEXU: case NVN_RB_SAMPU: case NVN_RB_SBUFU:
            value = reg < 16 ? state->slots[0][reg] : 0; break;
        case NVN_RB_SBUFP:
            value = reg < 16 ? state->slots[1][reg] : 0; break;
        case NVN_RB_TEX: case NVN_RB_TBUF: case NVN_RB_SAMPT:
            value = reg < 256 ? state->slots[2][reg] : 0; break;
        case NVN_RB_SAMP:
            value = reg < 128 ? state->slots[3][reg] : 0; break;
        case NVN_RB_IMG: case NVN_RB_IMGA:
            value = reg < 32 ? state->slots[4][reg] : 0; break;
        case NVN_RB_TEXD:
            for (unsigned k = 0; k < b->n; k++) {
                uint64_t handle = k < 128 ? (state->slots[2][k] ? state->slots[2][k] : state->slots[2][128 + k]) : 0;
                hash = fold(hash, handle);
            }
            break;
        case NVN_RB_SAMPD:
            for (unsigned k = 0; k < b->n; k++) {
                uint64_t sampler = k < 128 ? state->slots[3][k] >> 20 : 0;
                if (!sampler && k < 256) sampler = state->slots[2][k] >> 20;
                hash = fold(hash, sampler);
            }
            break;
        case NVN_RB_DRV0:
            if (reflection->drv_read[0]) {
                if (stage == 0 || stage == 1 || stage == 5)
                    for (int k = 0; k < 16; k++) hash = fold(hash, state->slots[1][k]);
                if (stage == 5)
                    for (int k = 0; k < 16; k++) hash = fold(hash, state->slots[0][k]);
            }
            break;
        case NVN_RB_DRV2:
            if (reflection->drv_read[2]) {
                // The exact driver table contents are more selective than
                // the deliberately conservative dependency mask for this kind.
                for (int k = 0; k < 8; k++) hash = fold(hash, state->slots[4][k]);
                for (int k = 0; k < 41; k++) {
                    if (k >= 32 && (!state->slots[2][k] || (k - 32 < 8 && state->slots[4][k - 32]))) continue;
                    hash = fold(hash, state->slots[2][k]);
                }
                for (int k = 0; k < 128; k++) {
                    hash = fold(hash, state->slots[2][128 + k]);
                    hash = fold(hash, state->slots[3][k]);
                }
            }
            break;
        case NVN_RB_SAMPI: value = reg; break;
        case NVN_RB_C1: case NVN_RB_SUPPORT: value = 123; break;
        default: abort();
        }
        hash = fold(hash, value);
    }
    return hash;
}

static void perturb(const nvn_refl *reflection, int stage, int mode) {
    bindings state = {0};
    for (int group = 0; group < 5; group++)
        for (int slot = 0; slot < limits[group]; slot++)
            state.slots[group][slot] = ((uint64_t)(group + 1) << 40) | ((uint64_t)(slot + 1) << 20) | (unsigned)(slot + 1);
    if (mode == 1) memset(state.slots[2], 0, 128 * sizeof(uint64_t));
    if (mode == 2) memset(state.slots[3], 0, sizeof state.slots[3]);
    nvn_argument_dependencies dependencies = nvn_argument_dependencies_make(reflection, stage);
    uint64_t original = observe(reflection, stage, &state);
    for (int group = 0; group < 5; group++) {
        for (int slot = 0; slot < limits[group]; slot++) {
            state.slots[group][slot] ^= UINT64_C(0x3510025);
            int changed = observe(reflection, stage, &state) != original;
            int dependent = nvn_argument_binding_used(&dependencies, group, slot, 1);
            CHECK(!changed || dependent);
            state.slots[group][slot] ^= UINT64_C(0x3510025);
        }
    }
}

static uint8_t g_args_dirty[6];
static nvn_argument_dependencies g_args_dependencies[6];
static uint64_t g_stat_args_bind_skips, g_reg_gen, g_ser_gen, dirty_epoch;
static uint64_t g_args_reg_seen, g_args_dirty_seen, g_args_ser_seen;
static int filter_enabled = 1;
static int args_binding_filter_on(void) { return filter_enabled; }
static uint64_t nvn_dirty_now(void) { return dirty_epoch; }
typedef struct { nvn_refl rf; } NvShader;
static NvShader *g_args_shader[6];
'''

SOURCE += "\n" + function("args_dirty_all")
SOURCE += "\n" + function("args_binding_changed")
SOURCE += "\n" + function("args_sync_dirty")
start = renderer.index("    args_sync_dirty();", renderer.index("static int encode_args_(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) {"))
end = renderer.index("    if (!passdump()", start)
SOURCE += "\nstatic void prepare_shader(NvShader *sh, int nvn_stage) {\n" + renderer[start:end] + "}\n"
for name in ("p_bind_buf", "p_bind_handles"):
    SOURCE += "\n" + re.search(r"typedef struct \{[^\n]*\} " + name + r";", objects)[0]
SOURCE += r'''
#define NTEX 256
#define RED_ONCE(...) ((void)0)
static void bh_fresh(const p_bind_handles *binding) { (void)binding; }
static struct { p_bind_buf ubo[6][16], ssbo[6][16]; uint64_t tex[6][256], samp[6][128], img[6][32]; } S;
enum { OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP, OP_BIND_IMG };
static void state_bind(unsigned op, const void *a) { switch (op) {
'''
start = renderer.index("        case OP_BIND_UBO:", renderer.index("static int state_op("))
end = renderer.index("        case OP_STENCIL:", start)
SOURCE += renderer[start:end] + "    }\n}\n"
SOURCE += r'''
int main(void) {
    const unsigned registers[] = {0, 15, 16, 31, 32, 127, 128, 255, 256, 65535};
    const unsigned lengths[] = {0, 1, 63, 128, 129, 192};
    const int stages[] = {0, 1, 5};
    for (unsigned kind = 1; kind <= NVN_RB_IMGA; kind++) {
        for (unsigned r = 0; r < sizeof registers / sizeof *registers; r++) {
            nvn_refl reflection = {.nb = 1, .b = {{.kind = kind, .reg = registers[r]}}};
            for (unsigned variant = 0; variant < 6; variant++) {
                reflection.b[0].n = lengths[variant];
                reflection.drv_read[0] = reflection.drv_read[2] = variant & 1;
                for (unsigned st = 0; st < 3; st++) perturb(&reflection, stages[st], variant % 3);
            }
        }
    }
    uint32_t random = 0x6354;
    for (unsigned sample = 0; sample < 250; sample++) {
        nvn_refl reflection = {0};
        reflection.nb = 1 + (int)(sample % NVN_REFL_MAXB);
        reflection.drv_read[0] = sample & 1; reflection.drv_read[2] = sample >> 1 & 1;
        for (int i = 0; i < reflection.nb; i++) {
            random = random * 1664525 + 1013904223;
            reflection.b[i] = (nvn_rbind){.kind = 1 + random % NVN_RB_IMGA,
                .reg = registers[(random >> 8) % 10], .n = lengths[(random >> 16) % 6]};
        }
        perturb(&reflection, stages[sample % 3], sample % 3);
    }
    // Compare range queries against a scalar bit-by-bit oracle at word edges.
    nvn_argument_dependencies d = {.texture = {UINT64_C(0x8000000000000001), 0, 1, UINT64_MAX}};
    for (int first = 0; first <= 256; first++) {
        for (int count = 0; count <= 256 - first; count++) {
            int expected = 0;
            for (int i = first; i < first + count; i++) expected |= (int)(d.texture[i / 64] >> (i % 64) & 1);
            CHECK(nvn_argument_binding_used(&d, NVN_ARGUMENT_TEXTURE, first, count) == expected);
        }
    }
    CHECK(nvn_argument_binding_used(&d, NVN_ARGUMENT_TEXTURE, -1, 1));
    CHECK(nvn_argument_binding_used(&d, NVN_ARGUMENT_TEXTURE, 0, -1));
    CHECK(nvn_argument_binding_used(&d, NVN_ARGUMENT_TEXTURE, INT_MAX, 1));
    CHECK(nvn_argument_binding_used(&d, NVN_ARGUMENT_TEXTURE, 255, INT_MAX));
    CHECK(nvn_argument_binding_used(&d, 99, 0, 1));
    nvn_refl invalid = {.nb = 1, .b = {{.kind = 255}}};
    for (int variant = 0; variant < 4; variant++) {
        if (variant == 1) invalid.nb = -1;
        if (variant == 2) invalid.nb = NVN_REFL_MAXB + 1;
        d = nvn_argument_dependencies_make(variant == 3 ? NULL : &invalid, 0);
        for (int group = 0; group < 5; group++)
            for (int slot = 0; slot < limits[group]; slot++) CHECK(nvn_argument_binding_used(&d, group, slot, 1));
    }
    // Exercise the production state updates and dirty-epoch helpers. An unused
    // bind must still update state for a later shader that reads that slot.
    NvShader one = {.rf = {.nb = 1, .b = {{.kind = NVN_RB_CBUF, .reg = 2}}}};
    NvShader two = {.rf = {.nb = 1, .b = {{.kind = NVN_RB_CBUF, .reg = 3}}}};
    prepare_shader(&one, 0); CHECK(g_args_dirty[0]); g_args_dirty[0] = 0;
    p_bind_buf buffer = {.stage = 0, .index = 3, .addr = 4096, .size = 256};
    state_bind(OP_BIND_UBO, &buffer); CHECK(!g_args_dirty[0]); CHECK(S.ubo[0][3].addr == 4096);
    CHECK(g_stat_args_bind_skips == 1);
    prepare_shader(&two, 0); CHECK(g_args_dirty[0]); g_args_dirty[0] = 0;
    buffer.addr += 256; state_bind(OP_BIND_UBO, &buffer); CHECK(g_args_dirty[0]);
    g_args_dirty[0] = 0; state_bind(OP_BIND_UBO, &buffer); CHECK(!g_args_dirty[0]);
    state_bind(OP_BIND_SSBO, &buffer); CHECK(!g_args_dirty[0]); CHECK(S.ssbo[0][3].addr == buffer.addr);
    p_bind_handles handles = {.stage = 0, .first = 1, .n = 2, .h = {31, 32}};
    state_bind(OP_BIND_TEX, &handles); CHECK(!g_args_dirty[0] && S.tex[0][2] == 32);
    state_bind(OP_BIND_SAMP, &handles); CHECK(!g_args_dirty[0] && S.samp[0][2] == 32);
    state_bind(OP_BIND_IMG, &handles); CHECK(!g_args_dirty[0] && S.img[0][2] == 32);
    filter_enabled = 0; buffer.index = 4; state_bind(OP_BIND_UBO, &buffer); CHECK(g_args_dirty[0]);
    filter_enabled = 1; memset(g_args_dirty, 0, sizeof g_args_dirty); g_reg_gen++;
    args_sync_dirty(); for (int st = 0; st < 6; st++) CHECK(g_args_dirty[st]);
    memset(g_args_dirty, 0, sizeof g_args_dirty); dirty_epoch++;
    args_sync_dirty(); for (int st = 0; st < 6; st++) CHECK(g_args_dirty[st]);
    memset(g_args_dirty, 0, sizeof g_args_dirty); g_ser_gen++;
    args_sync_dirty(); for (int st = 0; st < 6; st++) CHECK(g_args_dirty[st]);
    memset(g_args_dirty, 0, sizeof g_args_dirty); args_sync_dirty();
    for (int st = 0; st < 6; st++) CHECK(!g_args_dirty[st]);
    args_dirty_all(); for (int st = 0; st < 6; st++) CHECK(g_args_dirty[st]);
    printf("%u checks, %u failures; scalar dependency oracle and production state helpers, CPU only\n", checks, failures);
    return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix="nvn-argument-dependencies-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(SOURCE)
    subprocess.run([
        "xcrun", "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I", str(ROOT / "nvn"),
        str(source), "-o", str(binary),
    ], check=True)
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
    subprocess.run([str(binary)], env=environment, check=True)
