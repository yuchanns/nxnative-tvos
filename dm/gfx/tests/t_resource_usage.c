#include "../nvn/nvn_resource_usage.h"
#include <stdio.h>

static nvn_resource_usage cache;
static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #condition); } } while (0)

int main(void) {
    nvn_resource_usage_reset(&cache);
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 1, 0));
    CHECK(!nvn_resource_usage_require(&cache, 0x1000, 1, 0));
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 1, 1));
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 3, 1));
    CHECK(!nvn_resource_usage_require(&cache, 0x1000, 1, 1));
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 2, 0));
    CHECK(!nvn_resource_usage_require(&cache, 0x1000, 3, 0));
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 1, 2));
    CHECK(nvn_resource_usage_require(&cache, 0x2000, 1, 0));
    nvn_resource_usage_reset(&cache);
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 1, 0));
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 3, 1));
    // Collisions may overdeclare; they must never lose a first declaration
    // or an upgrade of access permissions.
    for (uintptr_t i = 1; i <= 4096; i++) CHECK(nvn_resource_usage_require(&cache, i * 32 + 0x10000, 1, 0));
    for (uintptr_t i = 1; i <= 4096; i++) CHECK(nvn_resource_usage_require(&cache, i * 32 + 0x10000, 3, 0));
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 8, 0));
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 8, 0));
    cache.epoch = UINT64_MAX;
    nvn_resource_usage_reset(&cache);
    CHECK(nvn_resource_usage_require(&cache, 0x1000, 1, 0));
    CHECK(!nvn_resource_usage_require(&cache, 0x1000, 1, 0));
    printf("resource usage: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
