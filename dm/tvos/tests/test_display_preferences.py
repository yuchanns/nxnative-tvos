"""Check display defaults and persistence in an isolated preferences suite."""

import os
from pathlib import Path
import subprocess
import tempfile
import uuid

DM = Path(__file__).resolve().parents[2]
SOURCE = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdlib.h>
static NSUserDefaults *test_defaults;
@interface TestDefaults : NSObject
+ (NSUserDefaults *)standardUserDefaults;
@end
@implementation TestDefaults
+ (NSUserDefaults *)standardUserDefaults { return test_defaults; }
@end
#define NSUserDefaults TestDefaults
#include "display.m"
#undef NSUserDefaults

int main(int argc, char **argv) {
    assert(argc == 5);
    @autoreleasepool {
        NSString *suite = [NSString stringWithUTF8String:argv[1]];
        test_defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
        assert(dm_tvos_display_options() == (unsigned)atoi(argv[2]));
        if (atoi(argv[3]) >= 0) {
            dm_tvos_display_set_options((unsigned)atoi(argv[3]));
            assert(dm_tvos_display_options() == ((unsigned)atoi(argv[3]) & DM_TV_DISPLAY_MASK));
            assert([test_defaults synchronize]);
        }
        if (atoi(argv[4])) {
            [test_defaults removePersistentDomainForName:suite];
            assert([test_defaults synchronize]);
        }
    }
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="nxnative-display-test-") as directory:
        root = Path(directory)
        source, binary = root / "display_test.m", root / "display_test"
        source.write_text(SOURCE)
        subprocess.run([
            "clang", "-std=gnu11", "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-framework", "Foundation",
            "-I", str(DM / "tvos/Sources"), str(source), "-o", str(binary),
        ], check=True)
        suite = "org.nxnative.display-test." + uuid.uuid4().hex
        cases = [("0", 2, 3, 0), ("0", 3, 7, 0), ("0", 7, 11, 0),
                 ("0", 11, 15, 0), ("0", 15, 0, 0), ("1", 0, 255, 0), ("0", 15, -1, 1)]
        for fps, expected, update, cleanup in cases:
            subprocess.run([str(binary), suite, str(expected), str(update), str(cleanup)],
                           env=dict(os.environ, DM_SHOW_FPS=fps, ASAN_OPTIONS="detect_leaks=0"),
                           check=True, timeout=20)
        print("display preferences: defaults, four corners, restart persistence, overrides and option mask passed")


if __name__ == "__main__":
    main()
