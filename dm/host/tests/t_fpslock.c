#include "../hle_fpslock.c"
#include <math.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

module_t g_mod[MAX_MOD];
int g_nmod;
static unsigned writes, checks, failures;
void dm_guest_write_prepare(void *address, size_t length) { (void)address; (void)length; writes++; }
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #condition); } } while (0)

static int scenario(int kind) {
    uint8_t *image = mmap(NULL, 0x13000000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (image == MAP_FAILED) return 1;
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = image;
    strcpy(g_mod[0].build_id, "82d187fe9ef9be921b8e1b1cddf7bd28");
    setenv("DM_FPS_TARGET", kind == 1 ? "30" : kind == 2 ? "0" : kind == 6 ? "75" : "60", 1);
    uint32_t *interval = (uint32_t *)(image + 0x1b42db0);
    uint8_t *object = image + 0x1000;
    memcpy(image + 0x1ba1a90, &object, sizeof object);
    float *seconds = (float *)(object + 0x80);
    interval[0] = interval[1] = 2; *seconds = 1.0f / 30;
    if (kind == 3) strcpy(g_mod[0].build_id, "00000000000000000000000000000000");
    if (kind == 4) {
        strcpy(g_mod[0].build_id, "c9a3dd7702075ecd90101f4555389582");
        memcpy(image + 0x1295b3f0, &object, sizeof object);
        *(float *)(object + 0x434) = 30;
        dm_fps_lock_tick();
        CHECK(*(float *)(object + 0x434) == 60);
    } else if (kind == 5) {
        strcpy(g_mod[0].build_id, "9fe60a212cff9b9a3656ab63d4f882e5");
        uint8_t *fps = object + 0x100, *vsync = object + 0x200;
        memcpy(image + 0x7c738e0, &fps, sizeof fps);
        memcpy(image + 0x7b326a0, &vsync, sizeof vsync);
        ((float *)fps)[0] = ((float *)fps)[1] = 30;
        ((uint32_t *)vsync)[0] = ((uint32_t *)vsync)[1] = 2;
        dm_fps_lock_tick();
        CHECK(((float *)fps)[0] == 60 && ((float *)fps)[1] == 60);
        CHECK(((uint32_t *)vsync)[0] == 1 && ((uint32_t *)vsync)[1] == 1);
    } else {
        dm_fps_lock_tick();
        if (kind == 0) {
            CHECK(interval[0] == 1 && interval[1] == 1);
            CHECK(fabsf(*seconds - 1.0f / 60) < 1e-8f);
            CHECK(writes == 3);
            dm_fps_lock_tick(); CHECK(writes == 3);
            image[0x1ba1a98] = 1;
            dm_fps_lock_tick();
            CHECK(interval[0] == 2 && interval[1] == 2);
            CHECK(fabsf(*seconds - 1.0f / 30) < 1e-8f);
            image[0x1ba1a98] = 0;
            dm_fps_lock_tick();
            CHECK(interval[0] == 1 && interval[1] == 1);
            CHECK(fabsf(*seconds - 1.0f / 60) < 1e-8f);
            uintptr_t bad_pointer = 1;
            memcpy(image + 0x1ba1a90, &bad_pointer, sizeof bad_pointer);
            interval[0] = UINT32_MAX; interval[1] = 0;
            unsigned before = writes; dm_fps_lock_tick();
            CHECK(writes == before && interval[0] == UINT32_MAX && interval[1] == 0);
        } else {
            CHECK(interval[0] == 2 && interval[1] == 2);
            CHECK(*seconds == 1.0f / 30 && writes == 0);
        }
    }
    munmap(image, 0x13000000);
    printf("FPS lock scenario %d: %u checks, %u failures\n", kind, checks, failures);
    fflush(stdout);
    return failures != 0;
}

int main(void) {
    unsigned failed = 0;
    for (int kind = 0; kind < 7; kind++) {
        pid_t child = fork();
        if (!child) _exit(scenario(kind));
        int status;
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) failed++;
    }
    return failed != 0;
}
