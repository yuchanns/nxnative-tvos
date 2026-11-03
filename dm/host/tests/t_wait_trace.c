// Verify that diagnostic recording preserves timeout and event-wakeup results.
#include "../dm.h"
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

int svc_thread(int num, uint64_t *regs);
uint32_t svc_thread_main_init(void);
static uint32_t event_write;
static uint64_t signal_tid;

static void *signal_event(void *arg) {
    (void)arg;
    pthread_threadid_np(NULL, &signal_tid);
    usleep(30000);
    uint64_t regs[32] = {event_write};
    assert(svc_thread(0x11, regs) == 1 && regs[0] == 0);
    return NULL;
}

int main(void) {
    char path[] = "/tmp/nx-wait-trace-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    setenv("DM_WAIT_TRACE", path, 1);
    setenv("DM_WAIT_TRACE_MIN_US", "2000", 1);
    setenv("DM_PREEMPT", "0", 1);
    g_t0_ns = now_ns();
    uint32_t main_handle = svc_thread_main_init();
    uint64_t main_tid; pthread_threadid_np(NULL, &main_tid);
    uint32_t address = 7;
    uint64_t timeout[32] = {(uint64_t)&address, 2, 7, 10000000};
    timeout[30] = 0x12345678;
    assert(svc_thread(0x34, timeout) == 1 && timeout[0] == 0xea01);
    uint64_t create[32] = {0};
    assert(svc_thread(0x45, create) == 1 && create[0] == 0);
    event_write = (uint32_t)create[1];
    uint32_t event_read = (uint32_t)create[2];
    pthread_t thread; assert(!pthread_create(&thread, NULL, signal_event, NULL));
    uint64_t wait[32] = {0, (uint64_t)&event_read, 1, 1000000000};
    wait[30] = 0x23456789;
    assert(svc_thread(0x18, wait) == 1 && wait[0] == 0 && wait[1] == 0);
    pthread_join(thread, NULL);

    int matched = 0;
    for (int attempt = 0; attempt < 30 && matched != 3; attempt++) {
        usleep(10000);
        FILE *file = fopen(path, "r"); assert(file);
        char line[1024];
        while (fgets(line, sizeof line, file)) {
            if (line[0] < '0' || line[0] > '9') continue;
            uint64_t values[19]; char *cursor = line;
            for (unsigned i = 0; i < 19; i++) {
                char *field = strsep(&cursor, "\t"); assert(field);
                values[i] = strtoull(field, NULL, 0);
            }
            assert(cursor && strchr(cursor, '\n'));
            assert(values[1] >= values[0] && values[2] <= values[1] - values[0]);
            assert(values[3] <= values[1] - values[0]);
            assert(values[5] == main_tid && values[6] == main_handle);
            if (values[9] == 0x12345678) {
                assert(values[7] == 0x34 && values[8] == 0xea01);
                assert(values[10] == (uint64_t)&address && values[14] == 1);
                assert(values[3] >= 9000000 && values[4] == 0 && values[18] == 0);
                matched |= 1;
            } else if (values[9] == 0x23456789) {
                assert(values[7] == 0x18 && values[8] == 0 && values[14] == 1);
                assert(values[15] == 0 && values[16] == signal_tid);
                assert(values[18] >= values[0] && values[18] <= values[1]);
                assert(values[4] <= values[3]);
                matched |= 2;
            }
        }
        fclose(file);
    }
    assert(matched == 3);
    unlink(path);
    puts("wait trace: timeout and host event-wakeup attribution passed");
    return 0;
}
