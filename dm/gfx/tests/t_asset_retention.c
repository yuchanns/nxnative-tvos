#include "../nvn/nvn_asset_retention.h"
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #condition); } } while (0)

static int kept(const nvn_asset_retention_item *items, size_t count, size_t index) {
    for (size_t i = 0; i < count; i++) if (items[i].index == index) return items[i].keep;
    return 0;
}

int main(void) {
    // A recent large texture wins over an older one; the remaining budget still
    // admits a smaller texture, even when an intervening candidate does not fit.
    nvn_asset_retention_item recent[] = {{80, 2, 0, 0}, {90, 4, 1, 0}, {95, 4, 2, 0}};
    CHECK(nvn_asset_retention_select(recent, 3, 100, 120, 6) == 6);
    CHECK(kept(recent, 3, 2) && !kept(recent, 3, 1) && kept(recent, 3, 0));
    CHECK(recent[0].index == 2 && recent[1].index == 1 && recent[2].index == 0);

    // Equal-age candidates use their original index as a deterministic tie-break.
    nvn_asset_retention_item tied[] = {{90, 4, 9, 0}, {90, 4, 2, 0}};
    CHECK(nvn_asset_retention_select(tied, 2, 100, 120, 4) == 4);
    CHECK(kept(tied, 2, 2) && !kept(tied, 2, 9));

    nvn_asset_retention_item ages[] = {{80, 1, 0, 0}, {79, 1, 1, 1}, {201, 1, 2, 1}, {200, 0, 3, 1}};
    CHECK(nvn_asset_retention_select(ages, 4, 200, 120, 10) == 1);
    CHECK(kept(ages, 4, 0) && !kept(ages, 4, 1));
    CHECK(!kept(ages, 4, 2) && !kept(ages, 4, 3));
    CHECK(nvn_asset_retention_select(ages, 4, 201, 120, 10) == 1);
    CHECK(!kept(ages, 4, 0) && kept(ages, 4, 2));

    // Reducing the allowance recomputes protection instead of retaining stale
    // decisions. No selected texture is allocated by the policy itself.
    CHECK(nvn_asset_retention_select(recent, 3, 100, 120, 3) == 2);
    CHECK(kept(recent, 3, 0) && !kept(recent, 3, 1) && !kept(recent, 3, 2));
    CHECK(nvn_asset_retention_select(recent, 3, 100, 120, 0) == 0);
    CHECK(!kept(recent, 3, 0) && !kept(recent, 3, 1) && !kept(recent, 3, 2));
    CHECK(nvn_asset_retention_select(NULL, 0, 0, 120, UINT64_MAX) == 0);

    nvn_asset_retention_item extreme[] = {
        {UINT64_MAX, UINT64_MAX - 5, 0, 0}, {UINT64_MAX - 1, 10, 1, 0}, {0, 1, 2, 0},
    };
    CHECK(nvn_asset_retention_select(extreme, 3, UINT64_MAX, 120, UINT64_MAX) == UINT64_MAX - 5);
    CHECK(kept(extreme, 3, 0) && !kept(extreme, 3, 1) && !kept(extreme, 3, 2));
    CHECK(nvn_asset_retention_select(extreme, 3, 0, UINT64_MAX, UINT64_MAX) == 1);
    CHECK(!kept(extreme, 3, 0) && !kept(extreme, 3, 1) && kept(extreme, 3, 2));

    printf("asset retention: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
