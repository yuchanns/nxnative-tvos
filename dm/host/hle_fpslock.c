// Engine frame limits kept in game memory (FPSLocker's evaluate_write entries): some engines cap their own frame rate with a value the host can raise. The target is the profile's
// fps_target (DM_FPS_TARGET overrides; 0 = leave the game's own limit), the same key that drives Tears of the Kingdom's frame step (loader.c install_fps_state). Called on every present
// (hle_nvn.c); the game sets the values after boot and may set them again, so each is written whenever it differs. Fixed addresses: each row runs only on the main build it was found in (AGENTS.md)
#include "hle.h"
#include <mach/mach.h>

// What a field holds: the frame rate as a float, or vsyncs per frame (60 / frame rate) as an int32. Copies: consecutive copies of the value (Unreal keeps one for the game thread, one for the render thread)
enum { FPS_F32, VSYNCS_I32, FRAME_SECONDS_F32 };
static const struct { const char *build, *what; uint64_t ptr_off; uint32_t field; uint8_t kind, copies, direct; uint32_t movie_flag; } k_locks[] = {
    // Monster Hunter Rise 16.0.2: RE Engine's frame limit, a float at [main+0x1295b3f0]+0x434 (the pointer is a global in main's bss). The frame loop (main+0x6b106c8) sleeps until
    // [+0x430] / [+0x434] sixtieths of a second have passed ([+0x430] = 60) and steps the game by the time that really passed (+0x41c), so a higher limit runs the game faster in frames
    // at the same speed. The game sets 30 after boot; it also asks for present interval 2, which the profile's present_interval display ignores.
    // From masagrator's FPSLocker config for this build (FPSLocker-Warehouse, patches/0100B04011742000/C9A3DD7702075ECD.yaml)
    { "c9a3dd7702075ecd90101f4555389582", "RE Engine frame limit", 0x1295b3f0, 0x434, FPS_F32, 1 },
    // Octopath Traveler 0 1.0.5: Unreal Engine 5's console variables t.MaxFPS (the game thread's frame limit: it sleeps until 1 / t.MaxFPS has passed and steps the game by the time that
    // really passed) and rhi.SyncInterval (vsyncs per present: the RHI's present interval). Each is a TAutoConsoleVariable in main's bss (vtable, the console object, then at +0x10 the pointer to
    // its value: the game thread's copy, then the render thread's). The game's config sets t.MaxFPS 30 ([ConsoleVariables] of DefaultEngine.ini; FrameRateLimit=30 of SwitchGameUserSettings.ini
    // sets it again) and rhi.SyncInterval 2 (SwitchDeviceProfiles.ini), so the game asks for present interval 2. Found from the names' references in the static constructors that register them
    // (main+0x2ca6524, main+0x1026e54; RegisterConsoleVariable, then AsVariableFloat / AsVariableInt stored at +0x10)
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Unreal t.MaxFPS", 0x7c738d0 + 0x10, 0, FPS_F32, 2 },
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Unreal rhi.SyncInterval", 0x7b32690 + 0x10, 0, VSYNCS_I32, 2 },
    // Xenoblade Chronicles 3 2.2.0: the engine's two vsync counts and the UI's
    // seconds per frame must change together. Prerendered movies retain 30 fps.
    // FPSLocker-Warehouse, patches/010074F013262000/82D187FE9EF9BE92.yaml;
    // patch_nso.py also fixes the DR target and the ether timer for this build.
    { "82d187fe9ef9be921b8e1b1cddf7bd28", "Monolith vsync interval", 0x1b42db0, 0, VSYNCS_I32, 2, 1, 0x1ba1a98 },
    { "82d187fe9ef9be921b8e1b1cddf7bd28", "Monolith UI frame time", 0x1ba1a90, 0x80, FRAME_SECONDS_F32, 1, 0, 0x1ba1a98 },
};
#define NLOCKS ((int)(sizeof k_locks / sizeof k_locks[0]))

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n; }

void dm_fps_lock_tick(void) {
    static int state = -2, rows[NLOCKS], nrow, nlog[NLOCKS], target; static uint8_t *base; static const char *src;
    if (state == -1) return;
    if (state == -2) {
        state = -1;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
            for (int k = 0; k < NLOCKS; k++) if (!strcmp(g_mod[i].build_id, k_locks[k].build)) { rows[nrow++] = k; base = g_mod[i].base; }
        if (!nrow) return;
        const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET; src = e ? "DM_FPS_TARGET" : "game profile fps_target";
        if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; leaving the game's frame limit", e); t = 0; }
        if (!t) { for (int r = 0; r < nrow; r++) LOG("main: %s left as the game sets it (%s)", k_locks[rows[r]].what, src); return; }
        target = t; state = 0;
    }
    for (int r = 0; r < nrow; r++) {
        int k = rows[r]; uint64_t obj;
        int fps = target;
        if (k_locks[k].movie_flag) {
            uint8_t movie;
            if (!rd((uint64_t)(uintptr_t)(base + k_locks[k].movie_flag), &movie, sizeof movie)) continue;
            if (movie) fps = 30;
        }
        if (k_locks[k].direct) obj = (uint64_t)(uintptr_t)(base + k_locks[k].ptr_off);
        else if (!rd((uint64_t)(uintptr_t)(base + k_locks[k].ptr_off), &obj, sizeof obj) || !obj) continue;   // not created yet
        for (int c = 0; c < k_locks[k].copies; c++) {
            uint64_t a = obj + k_locks[k].field + 4 * (uint64_t)c; uint32_t cur, want; char was[16];
            if (!rd(a, &cur, sizeof cur)) break;
            if (k_locks[k].kind != VSYNCS_I32) {
                int seconds = k_locks[k].kind == FRAME_SECONDS_F32;
                float f, w = seconds ? 1.0f / (float)fps : (float)fps; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(seconds ? f >= 1.0f / 120 && f <= 0.1f : f >= 10 && f <= 120)) continue;   // already set, or not initialized yet
                snprintf(was, sizeof was, "%g", f);
            } else {
                want = 60 / (uint32_t)fps;
                if (cur == want || cur < 1 || cur > 4) continue;
                snprintf(was, sizeof was, "%u", cur);
            }
            uint32_t *p = (uint32_t *)(uintptr_t)a;
            dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, want, __ATOMIC_RELAXED);
            if (nlog[k] < 8) { nlog[k]++; float f; memcpy(&f, &want, sizeof f); char value[24];
                if (k_locks[k].kind == VSYNCS_I32) snprintf(value, sizeof value, "%u vsyncs", want);
                else snprintf(value, sizeof value, "%g %s", f, k_locks[k].kind == FPS_F32 ? "fps" : "seconds");
                LOG("main: %s%s %s → %s (%s)", k_locks[k].what, k_locks[k].copies > 1 ? (c ? " (render thread)" : " (game thread)") : "", was, value, src); }
        }
    }
}
