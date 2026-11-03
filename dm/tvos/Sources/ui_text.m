#import "ui_text.h"

NSString *dm_tvos_text(NSString *message) {
    if (!message.length) return @"";
    static NSBundle *bundle;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *path = [NSBundle.mainBundle pathForResource:@"zh-Hans" ofType:@"lproj"];
        bundle = path ? [NSBundle bundleWithPath:path] : NSBundle.mainBundle;
    });
    return [bundle localizedStringForKey:message value:message table:@"DmTV"];
}
