// iOS App entry: set the environment (game files, saves, shader libraries), write the host log to Documents/log.txt, create the window (front_ios.m), then run the host's dm_main on a big-stack thread.
// The game files (RomFS) ship inside the App at <App>/romfs; Documents/romfs placed through the Files app is also accepted (lets you swap them without reinstalling).
#include <TargetConditionals.h>
#import <UIKit/UIKit.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "process_signals.h"
#include "game_profile.h"   // game profile constants (the RomFS marker path)
#if TARGET_OS_TV
#import "loading.h"
#import "gamepad.h"
#import "cloud_save.h"
#endif

int dm_main(int argc, char **argv);   // dm/host/main.c compiled in with -Dmain=dm_main
void dm_host_preload_start(int argc, char **argv);
UIViewController *front_ios_root(void);
void front_ios_focus_event(int ev);

static char *g_argv[4]; static int g_argc;

static NSString *state_directory(void) {
#if TARGET_OS_TV
    // tvOS keeps downloadable resources in Caches; verify them before boot.
    NSString *cache = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES)[0];
    return [cache stringByAppendingPathComponent:@"nxnative"];
#else
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
#endif
}

static NSString *romfs_locate(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = state_directory();
    for (NSString *d in @[[doc stringByAppendingPathComponent:@"romfs"], [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs"]])
        if ([fm fileExistsAtPath:[d stringByAppendingPathComponent:@DM_GAME_ROMFS_MARKER]]) return d;   // only a directory containing the game profile's romfs_marker path counts (Mario: System/Resource, the resource size table)
    return nil;
}

// Boot prerequisites (done before UIApplicationMain): directories / environment variables / log -- the host's early load reads these
static int boot_env(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = state_directory();
    NSString *res = NSBundle.mainBundle.resourcePath;
    for (NSString *d in @[@"save", @"temp"]) [fm createDirectoryAtPath:[doc stringByAppendingPathComponent:d] withIntermediateDirectories:YES attributes:nil error:nil];
    // Keep game files and scratch data out of device backups (Documents is backed up by default; a RomFS placed there is many GB). Saves, logs and the pipeline list stay in the backup
    for (NSString *d in @[@"romfs", @"temp"]) {
        NSURL *u = [NSURL fileURLWithPath:[doc stringByAppendingPathComponent:d] isDirectory:YES]; NSError *e = nil;
        if ([fm fileExistsAtPath:u.path] && ![u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&e]) fprintf(stderr, "boot: %s could not be excluded from backup: %s\n", d.UTF8String, e.localizedDescription.UTF8String);
    }
    // Log: the host only writes to stderr; redirect it to Documents/log.txt (the previous one is kept as log.prev.txt), retrieve it from the Files app when something goes wrong
    NSString *log = [doc stringByAppendingPathComponent:@"log.txt"], *prev = [doc stringByAppendingPathComponent:@"log.prev.txt"];
    [fm removeItemAtPath:prev error:nil]; [fm moveItemAtPath:log toPath:prev error:nil];
    if (freopen(log.fileSystemRepresentation, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    // Environment switch files (for on-device diagnostics: a sideloaded app cannot be given environment variables from outside): one KEY=VALUE per line, # starts a comment; Documents/env.txt takes precedence over dm/env.txt in the bundle (put there by build_ipa.sh's DM_ENV_FILE)
    for (NSString *ef in @[[doc stringByAppendingPathComponent:@"env.txt"], [[res stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"env.txt"]]) {
        NSString *txt = [NSString stringWithContentsOfFile:ef encoding:NSUTF8StringEncoding error:nil];
        for (NSString *raw in [txt componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
            NSString *ln = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; NSRange eq = [ln rangeOfString:@"="];
            if (!ln.length || [ln hasPrefix:@"#"] || eq.location == NSNotFound || !eq.location) continue;
            NSString *k = [ln substringToIndex:eq.location], *v = [ln substringFromIndex:eq.location + 1];
            if (!getenv(k.UTF8String)) { setenv(k.UTF8String, v.UTF8String, 0); fprintf(stderr, "boot: env %s=%s (%s)\n", k.UTF8String, [k hasSuffix:@"TOKEN"] ? "<redacted>" : v.UTF8String, ef.lastPathComponent.UTF8String); }
        }
    }
    setenv("DM_SAVE", [doc stringByAppendingPathComponent:@"save"].fileSystemRepresentation, 0);
    setenv("DM_TEMP", [doc stringByAppendingPathComponent:@"temp"].fileSystemRepresentation, 0);
    NSString *cache = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES)[0];
    setenv("DM_NVN_ASTC_CACHE", [cache stringByAppendingPathComponent:@"nxnative/astc"].fileSystemRepresentation, 0);
    setenv("DM_NVN_ASTC_CACHE_RO", [res stringByAppendingPathComponent:@"astc"].fileSystemRepresentation, 0);
#if TARGET_OS_TV
    void dm_tvos_diagnostics_start(const char *directory);
    dm_tvos_diagnostics_start(doc.fileSystemRepresentation);
    dm_tvos_gamepad_start();
#endif
    NSString *dm = [res stringByAppendingPathComponent:@"dm"];
    setenv("DM_NVN_SHMAP", [dm stringByAppendingPathComponent:@"shader_map.tsv"].fileSystemRepresentation, 0);
    setenv("DM_MSL_DIR", [res stringByAppendingPathComponent:@"msl"].fileSystemRepresentation, 0);
    setenv("DM_FONT_DIR", [dm stringByAppendingPathComponent:@"font"].fileSystemRepresentation, 0);   // Shared-font stand-ins (standard.ttf / nintendo_ext.ttf): iOS lacks the macOS font path
    NSString *aoc = [res stringByAppendingPathComponent:@"aoc"];   // add-on content: build_ipa.sh puts the RomFS extracted from the player's DLC NSPs at <App>/aoc/<index>/
    if ([fm fileExistsAtPath:aoc]) setenv("DM_AOC_DIR", aoc.fileSystemRepresentation, 0);
    chdir(doc.fileSystemRepresentation);
    g_argv[0] = strdup("dmhost"); g_argv[1] = strdup(dm.fileSystemRepresentation); g_argc = 2;
#if TARGET_OS_TV
    if ([fm fileExistsAtPath:[dm stringByAppendingPathComponent:@"assets-manifest.json"]] || getenv("DM_CLOUD_SAVE_URL")) return 0;
#endif
    NSString *romfs = romfs_locate();
    if (!romfs) { fprintf(stderr, "boot: game files not found\n"); return 0; }
    fprintf(stderr, "boot: game files %s\n", romfs.fileSystemRepresentation);
    setenv("DM_ROMFS", romfs.fileSystemRepresentation, 0);
    dm_host_preload_start(g_argc, g_argv);   // Module tables / relocation / takeover run early in the background, in parallel with UIKit startup
    return 1;
}
static int g_boot_ok;
#if TARGET_OS_TV
void front_tvos_prepare(UIView *loading) {
    void dm_tvos_assets_prepare(NSString *, void (^)(NSString *), void (^)(BOOL));
    NSDictionary *dm_tvos_asset_status(void);
    BOOL downloadable = [NSFileManager.defaultManager fileExistsAtPath:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm/assets-manifest.json"]];
    void (^resources_ready)(BOOL) = ^(BOOL ok) {
        NSString *romfs = ok ? romfs_locate() : nil;
        if (!romfs) {
            if (ok) dm_tvos_loading_update(loading, @{@"stage": @"failed", @"message": @"Game resources are incomplete. Reopen the game to try again."});
            return;
        }
        setenv("DM_ROMFS", romfs.fileSystemRepresentation, 1);
        if (downloadable) {
            setenv("DM_MSL_DIR", [state_directory() stringByAppendingPathComponent:@"msl"].fileSystemRepresentation, 1);
            setenv("DM_NVN_ASTC_CACHE_RO", [state_directory() stringByAppendingPathComponent:@"astc"].fileSystemRepresentation, 1);
        }
        dm_tvos_cloud_save_prepare(state_directory(), ^(NSString *message) {
            dm_tvos_loading_update(loading, @{@"stage": @"cloud-save", @"message": message});
        }, ^(BOOL save_ready) {
            if (!save_ready) return;
            dm_host_preload_start(g_argc, g_argv); g_boot_ok = 1;
            UIWindow *window = loading.window;
            window.rootViewController = front_ios_root();
        });
    };
    if (downloadable) dm_tvos_assets_prepare(state_directory(), ^(NSString *status) {
        (void)status; dm_tvos_loading_update(loading, dm_tvos_asset_status());
    }, resources_ready);
    else resources_ready(YES);
}
#endif

static void *boot_thread(void *p) {
    (void)p;
#if !TARGET_OS_TV
    static char alt[65536]; stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);   // crash printing must work on the host thread too
#endif
    int mtl_bc_selftest(void);
    const char *bc_selftest = getenv("DM_NVN_BC_SELFTEST");
    if (bc_selftest && !strcmp(bc_selftest, "1") && mtl_bc_selftest()) {
        fprintf(stderr, "boot: BC device self-test failed; guest not started\n");
        return NULL;
    }
    int rc = dm_main(g_argc, g_argv);
    fprintf(stderr, "boot: dm_main returned %d\n", rc);
    return NULL;
}
void front_ios_boot(void) {   // Called by front_ios.m once the window and Metal view exist
    static int once;
    if (!g_boot_ok || UIApplication.sharedApplication.applicationState != UIApplicationStateActive || once++) return;
    void front_ios_audio_session(void); front_ios_audio_session();   // set a short I/O buffer before the game opens audio output (front_ios.m)
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);   // the Switch main thread stack is set by npdm; the host gives plenty
    pthread_t t; pthread_create(&t, &a, boot_thread, NULL); pthread_detach(t);
}
int front_ios_boot_ok(void) { return g_boot_ok; }

@interface App : UIResponder <UIApplicationDelegate>
@end
@implementation App
- (UISceneConfiguration *)application:(UIApplication *)a configurationForConnectingSceneSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    return [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:s.role];
}
@end

// Scene lifecycle: create the window + hand focus events to front_ios.m
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)o {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = front_ios_root();
    [self.window makeKeyAndVisible];
}
- (void)sceneWillResignActive:(UIScene *)s {
#if TARGET_OS_TV
    dm_tvos_gamepad_active(0);
#endif
    front_ios_focus_event(0);
}
- (void)sceneDidEnterBackground:(UIScene *)s { front_ios_focus_event(1); }
- (void)sceneWillEnterForeground:(UIScene *)s { front_ios_focus_event(2); }
- (void)sceneDidBecomeActive:(UIScene *)s {
#if TARGET_OS_TV
    dm_tvos_gamepad_active(1);
    dm_tvos_cloud_save_resume();
#endif
    front_ios_focus_event(3); front_ios_boot();
}
@end

int main(int argc, char **argv) {
    if (dm_process_signals_init()) { perror("boot: cannot configure process signals"); return 1; }
    @autoreleasepool {
        g_boot_ok = boot_env();
        // Device diagnostic switches are loaded by boot_env, after constructors.
        void dm_memstat_start(void); dm_memstat_start();
    }
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(App.class)); }
}
