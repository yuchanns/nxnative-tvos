#pragma once

enum {
    DM_TV_DISPLAY_FPS = 1,
    DM_TV_DISPLAY_CLOUD_SAVE = 2,
    DM_TV_DISPLAY_POSITION_SHIFT = 2,
    DM_TV_DISPLAY_POSITION_MASK = 3 << DM_TV_DISPLAY_POSITION_SHIFT,
    DM_TV_DISPLAY_MASK = DM_TV_DISPLAY_FPS | DM_TV_DISPLAY_CLOUD_SAVE | DM_TV_DISPLAY_POSITION_MASK,
};
enum { DM_TV_TOP_LEFT, DM_TV_TOP_RIGHT, DM_TV_BOTTOM_LEFT, DM_TV_BOTTOM_RIGHT };

unsigned dm_tvos_display_options(void);
void dm_tvos_display_set_options(unsigned options);
