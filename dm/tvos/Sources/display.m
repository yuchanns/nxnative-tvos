#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "display.h"

static _Atomic unsigned g_options;
static NSString *const preferences_key = @"ControllerDisplayOptions";

unsigned dm_tvos_display_options(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSNumber *saved = [NSUserDefaults.standardUserDefaults objectForKey:preferences_key];
        const char *fps = getenv("DM_SHOW_FPS");
        BOOL show_fps = fps ? atoi(fps) != 0 : [[NSBundle.mainBundle objectForInfoDictionaryKey:@"DMShowFPS"] boolValue];
        unsigned defaults = DM_TV_DISPLAY_CLOUD_SAVE | (show_fps ? DM_TV_DISPLAY_FPS : 0);
        unsigned options = [saved isKindOfClass:NSNumber.class] ? saved.unsignedIntValue : defaults;
        atomic_store(&g_options, options & DM_TV_DISPLAY_MASK);
    });
    return atomic_load(&g_options);
}

void dm_tvos_display_set_options(unsigned options) {
    (void)dm_tvos_display_options();
    options &= DM_TV_DISPLAY_MASK;
    [NSUserDefaults.standardUserDefaults setObject:@(options) forKey:preferences_key];
    atomic_store(&g_options, options);
}
