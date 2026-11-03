#import "cloud_save.h"
#import "save_archive.h"
#import "ui_text.h"
#import <UIKit/UIKit.h>
#import <Security/Security.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/clonefile.h>
#include <sys/stat.h>

int dm_fs_clone_save(const char *destination);
static NSString *g_store, *g_save, *g_endpoint, *g_token, *g_base, *g_flight;
static dispatch_queue_t g_queue;
static dispatch_source_t g_timer;
static _Atomic bool g_ready, g_scheduled, g_capture_failed, g_capturing;
static _Atomic uint64_t g_capture_generation;
static pthread_mutex_t g_capture_lock = PTHREAD_MUTEX_INITIALIZER, g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static NSDictionary *g_status;
static BOOL g_verify_remote;

static NSDictionary *credential_query(void) {
    return @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
             (__bridge id)kSecAttrService: NSBundle.mainBundle.bundleIdentifier,
             (__bridge id)kSecAttrAccount: @"nxnative-cloud-save"};
}
static NSString *load_token(NSString *endpoint) {
    NSMutableDictionary *query = [credential_query() mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    CFTypeRef result = NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess) return nil;
    NSData *data = CFBridgingRelease(result);
    id credential = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return [credential isKindOfClass:NSDictionary.class] && [credential[@"endpoint"] isEqual:endpoint] ? credential[@"token"] : nil;
}
static BOOL store_token(NSString *endpoint, NSString *token) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:@{@"endpoint": endpoint, @"token": token} options:0 error:nil];
    NSDictionary *values = @{(__bridge id)kSecValueData: data,
                            (__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    OSStatus result = SecItemUpdate((__bridge CFDictionaryRef)credential_query(), (__bridge CFDictionaryRef)values);
    if (result == errSecItemNotFound) {
        NSMutableDictionary *item = [credential_query() mutableCopy]; [item addEntriesFromDictionary:values];
        result = SecItemAdd((__bridge CFDictionaryRef)item, NULL);
    }
    return result == errSecSuccess;
}

static NSError *save_error(NSString *message) {
    return [NSError errorWithDomain:@"NXCloudSave" code:1 userInfo:@{NSLocalizedDescriptionKey: message}];
}
static NSString *child(NSString *root, NSString *name) { return [root stringByAppendingPathComponent:name]; }
static BOOL is_hash(id value) {
    return [value isKindOfClass:NSString.class] && [value length] == 64 &&
        [value rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location == NSNotFound;
}
static id read_json(NSString *path, NSUInteger limit) {
    struct stat st;
    if (lstat(path.fileSystemRepresentation, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > limit) return nil;
    NSData *data = [NSData dataWithContentsOfFile:path];
    return data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
}
static BOOL write_json(id value, NSString *path, NSError **error) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingSortedKeys error:error];
    return data && [data writeToFile:path options:NSDataWritingAtomic error:error];
}
static BOOL replace_directory(NSString *staging, NSString *destination, NSError **error) {
    struct stat st; BOOL exists = !lstat(destination.fileSystemRepresentation, &st);
    int result = exists ? renamex_np(staging.fileSystemRepresentation, destination.fileSystemRepresentation, RENAME_SWAP)
                        : rename(staging.fileSystemRepresentation, destination.fileSystemRepresentation);
    if (result) { if (error) *error = save_error(@"Cannot publish the complete save directory."); return NO; }
    if (exists) [NSFileManager.defaultManager removeItemAtPath:staging error:nil];
    return YES;
}
NSDictionary *dm_tvos_cloud_save_status(void) {
    pthread_mutex_lock(&g_status_lock); NSDictionary *result = g_status ?: @{@"state": @"disabled", @"message": @"Cloud save is not configured."};
    pthread_mutex_unlock(&g_status_lock);
    NSMutableDictionary *display = [result mutableCopy];
    display[@"message"] = dm_tvos_text(result[@"message"]);
    return display;
}
static void status(NSString *state, NSString *message) {
    pthread_mutex_lock(&g_status_lock);
    NSMutableDictionary *next = [g_status mutableCopy] ?: [NSMutableDictionary dictionary];
    next[@"state"] = state; next[@"message"] = message; g_status = next;
    pthread_mutex_unlock(&g_status_lock);
    fprintf(stderr, "cloud save: %s: %s\n", state.UTF8String, message.UTF8String);
}
static void remember(void) {
    [NSUserDefaults.standardUserDefaults setObject:@{@"endpoint": g_endpoint, @"base": g_base ?: @"none", @"flight": g_flight ?: @""} forKey:@"NXCloudSave"];
}
static void acknowledged(NSDictionary *save) {
    g_base = save[@"revision"]; g_flight = nil; remember();
    pthread_mutex_lock(&g_status_lock);
    NSMutableDictionary *next = [g_status mutableCopy] ?: [NSMutableDictionary dictionary];
    next[@"revision"] = g_base; next[@"saved_at"] = save[@"savedAt"] ?: @""; g_status = next;
    pthread_mutex_unlock(&g_status_lock);
}

// Each request owns its buffers and completion signal. Redirects are refused so
// the narrowly scoped device token cannot follow a response to another origin.
@interface DmSaveRequest : NSObject <NSURLSessionDownloadDelegate, NSURLSessionDataDelegate>
@property (nonatomic, strong) dispatch_semaphore_t done;
@property (nonatomic, strong) NSMutableData *data;
@property (nonatomic, strong) NSError *error;
@property (nonatomic, strong) NSHTTPURLResponse *response;
@property (nonatomic, copy) NSString *destination;
@property (nonatomic) uint64_t limit;
@end
@implementation DmSaveRequest
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))completion {
    self.error = save_error(@"Cloud save redirects are not accepted."); completion(nil);
}
- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task didWriteData:(int64_t)bytes totalBytesWritten:(int64_t)total totalBytesExpectedToWrite:(int64_t)expected {
    if (total > (int64_t)self.limit || expected > (int64_t)self.limit) { self.error = save_error(@"Cloud save response exceeds the size limit."); [task cancel]; }
}
- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task didFinishDownloadingToURL:(NSURL *)location {
    struct stat st;
    if (stat(location.fileSystemRepresentation, &st) || st.st_size < 0 || (uint64_t)st.st_size > self.limit) { self.error = save_error(@"Cloud save response has an invalid size."); return; }
    if (![NSFileManager.defaultManager moveItemAtURL:location toURL:[NSURL fileURLWithPath:self.destination] error:NULL]) self.error = save_error(@"Cannot store the downloaded save.");
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    if (self.data.length + data.length > 8192) { self.error = save_error(@"Cloud save response exceeds the size limit."); [task cancel]; }
    else [self.data appendData:data];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    self.error = self.error ?: error; self.response = (NSHTTPURLResponse *)task.response; dispatch_semaphore_signal(self.done);
}
@end

static NSDictionary *exchange(NSString *method, NSString *route, NSString *upload, NSString *parent, NSString *download, NSInteger *code, NSError **error) {
    DmSaveRequest *job = [DmSaveRequest new]; job.done = dispatch_semaphore_create(0); job.data = [NSMutableData data];
    NSString *temporary = download ?: child(g_store, [@"response-" stringByAppendingString:NSUUID.UUID.UUIDString]);
    job.destination = temporary; job.limit = download ? DM_SAVE_ARCHIVE_LIMIT : 8192;
    NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    configuration.timeoutIntervalForRequest = 20; configuration.timeoutIntervalForResource = 90;
    configuration.URLCache = nil; configuration.HTTPShouldSetCookies = NO;
    NSOperationQueue *callbacks = [NSOperationQueue new]; callbacks.maxConcurrentOperationCount = 1;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration delegate:job delegateQueue:callbacks];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:[g_endpoint stringByAppendingString:route]]];
    request.HTTPMethod = method; request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    [request setValue:[@"Bearer " stringByAppendingString:g_token] forHTTPHeaderField:@"Authorization"];
    NSURLSessionTask *task;
    if (upload) {
        struct stat st;
        if (stat(upload.fileSystemRepresentation, &st)) { [session invalidateAndCancel]; if (error) *error = save_error(@"Pending save archive is missing."); return nil; }
        [request setValue:@"application/octet-stream" forHTTPHeaderField:@"Content-Type"];
        [request setValue:[NSString stringWithFormat:@"%lld", (long long)st.st_size] forHTTPHeaderField:@"Content-Length"];
        [request setValue:parent forHTTPHeaderField:@"X-Save-Parent"];
        task = [session uploadTaskWithRequest:request fromFile:[NSURL fileURLWithPath:upload]];
    } else task = [session downloadTaskWithRequest:request];
    __block UIBackgroundTaskIdentifier background = UIBackgroundTaskInvalid;
    dispatch_sync(dispatch_get_main_queue(), ^{
        background = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Cloud save" expirationHandler:^{ [task cancel]; }];
    });
    [task resume];
    if (dispatch_semaphore_wait(job.done, dispatch_time(DISPATCH_TIME_NOW, 95 * NSEC_PER_SEC))) {
        [task cancel];
        if (error) *error = save_error(@"Cloud save timed out.");
        [session invalidateAndCancel];
        dispatch_async(dispatch_get_main_queue(), ^{ if (background != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:background]; });
        return nil;
    }
    [session finishTasksAndInvalidate];
    dispatch_async(dispatch_get_main_queue(), ^{ if (background != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:background]; });
    if (code) *code = job.response.statusCode;
    if (job.error && error) *error = job.error;
    id result = upload ? [NSJSONSerialization JSONObjectWithData:job.data options:0 error:nil] : (download ? nil : read_json(temporary, 8192));
    if (!download) [NSFileManager.defaultManager removeItemAtPath:temporary error:nil];
    return [result isKindOfClass:NSDictionary.class] ? result : nil;
}

static BOOL valid_receipt(id value) {
    if (![value isKindOfClass:NSDictionary.class] || !is_hash(value[@"sha256"]) || ![value[@"bytes"] isKindOfClass:NSNumber.class]) return NO;
    double size = [value[@"bytes"] doubleValue];
    return size >= 12 && size <= DM_SAVE_ARCHIVE_LIMIT && size == (double)[value[@"bytes"] unsignedLongLongValue];
}
static NSDictionary *pending(void) {
    NSString *directory = child(g_store, @"pending"), *archive = child(directory, @"archive.nxsave");
    id receipt = read_json(child(directory, @"receipt.json"), 4096);
    if (!valid_receipt(receipt)) return nil;
    struct stat st;
    if (stat(archive.fileSystemRepresentation, &st) || st.st_size != [receipt[@"bytes"] longLongValue] ||
        ![dm_save_archive_hash(archive, nil) isEqual:receipt[@"sha256"]]) return nil;
    return receipt;
}
static BOOL capture(const char *source, NSError **error) {
    uint64_t generation = atomic_load(&g_capture_generation);
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *staging = child(g_store, [@"capture-" stringByAppendingString:NSUUID.UUID.UUIDString]);
    if (![fm createDirectoryAtPath:staging withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    NSString *tree = child(staging, @"tree");
    int result = source ? clonefile(source, tree.fileSystemRepresentation, 0) : dm_fs_clone_save(tree.fileSystemRepresentation);
    NSArray *index = result ? nil : dm_save_tree_index(tree, error);
    NSData *inventory = index ? [NSJSONSerialization dataWithJSONObject:@{@"id": NSUUID.UUID.UUIDString, @"entries": index} options:NSJSONWritingSortedKeys error:error] : nil;
    BOOL ok = inventory && inventory.length <= (1u << 20) && [inventory writeToFile:child(staging, @"index.json") options:NSDataWritingAtomic error:error];
    if (ok) {
        pthread_mutex_lock(&g_capture_lock);
        // A retry clone must not replace a newer game commit captured meanwhile.
        if (!source && generation != atomic_load(&g_capture_generation)) [fm removeItemAtPath:staging error:nil];
        else ok = replace_directory(staging, child(g_store, @"capture"), error);
        pthread_mutex_unlock(&g_capture_lock);
    }
    if (!ok) { [fm removeItemAtPath:staging error:nil]; if (error && !*error) *error = save_error(@"Cannot capture the complete local save."); }
    return ok;
}
static BOOL pack_capture(NSError **error) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *processing = child(g_store, @"processing");
    // One processing slot makes a crash recoverable without guessing the order
    // of orphan directories. Drain an older slot and the newest queued capture.
    for (int attempt = 0; attempt < 2; attempt++) {
        if (![fm fileExistsAtPath:processing]) {
            pthread_mutex_lock(&g_capture_lock);
            int taken = rename(child(g_store, @"capture").fileSystemRepresentation, processing.fileSystemRepresentation), saved_errno = errno;
            pthread_mutex_unlock(&g_capture_lock);
            if (taken) { if (saved_errno == ENOENT) return YES; if (error) *error = save_error(@"Cannot open the pending save capture."); return NO; }
        }
        id inventory = read_json(child(processing, @"index.json"), 1 << 20);
        BOOL valid = [inventory isKindOfClass:NSDictionary.class] && [inventory[@"id"] isKindOfClass:NSString.class] && [inventory[@"entries"] isKindOfClass:NSArray.class];
        id previous = read_json(child(child(g_store, @"pending"), @"receipt.json"), 4096);
        BOOL published = valid && [previous isKindOfClass:NSDictionary.class] && [previous[@"capture_id"] isEqual:inventory[@"id"]] && pending();
        if (!published) {
            NSString *tree = child(processing, @"tree");
            NSArray *actual = valid ? dm_save_tree_index(tree, error) : nil;
            if (!actual || ![inventory[@"entries"] isEqual:actual]) {
                if (error && !*error) *error = save_error(@"Local save capture is incomplete; the cloud save was not replaced."); return NO;
            }
            NSString *staging = child(g_store, [@"pending-" stringByAppendingString:NSUUID.UUID.UUIDString]);
            if (![fm createDirectoryAtPath:staging withIntermediateDirectories:NO attributes:nil error:error]) return NO;
            NSMutableDictionary *receipt = [dm_save_archive_create(tree, child(staging, @"archive.nxsave"), error) mutableCopy];
            if (receipt) receipt[@"capture_id"] = inventory[@"id"];
            BOOL ok = receipt && write_json(receipt, child(staging, @"receipt.json"), error) && replace_directory(staging, child(g_store, @"pending"), error);
            if (!ok) {
                [fm removeItemAtPath:staging error:nil];
                if (error && !*error) *error = save_error(@"Cannot finish the pending save archive."); return NO;
            }
        }
        if (![fm removeItemAtPath:processing error:error]) return NO;
    }
    return YES;
}
static NSDictionary *remote_head(NSInteger *code, NSError **error) {
    NSDictionary *head = exchange(@"GET", @"/v1/latest", nil, nil, nil, code, error);
    if (*code == 404 && [head[@"error"] isEqual:@"no_save"]) return nil;
    if (*code != 200 || !valid_receipt(head) || ![head[@"version"] isEqual:@1] || ![head[@"revision"] isEqual:head[@"sha256"]]) {
        if (error && !*error) *error = save_error(*code == 401 ? @"Cloud save authorization failed." : @"Cannot verify the cloud save.");
        return nil;
    }
    return head;
}
static BOOL publish(NSDictionary *receipt, NSDictionary *head, NSError **error) {
    NSString *revision = head[@"revision"] ?: @"none", *sha = receipt[@"sha256"];
    if ([revision isEqual:sha]) { acknowledged(head); return YES; }
    if (![revision isEqual:g_base] && ![revision isEqual:g_flight]) {
        if (error) *error = save_error(@"Cloud save changed elsewhere. Both versions are preserved.");
        status(@"conflict", @"Cloud save conflict; local progress is retained."); return NO;
    }
    g_base = revision; g_flight = sha; remember();
    status(@"syncing", @"Uploading save...");
    NSInteger code = 0;
    NSDictionary *saved = exchange(@"PUT", [@"/v1/snapshots/" stringByAppendingString:sha], child(child(g_store, @"pending"), @"archive.nxsave"), revision, nil, &code, error);
    if ((code == 200 || code == 201) && valid_receipt(saved) && [saved[@"revision"] isEqual:sha] && [saved[@"bytes"] isEqual:receipt[@"bytes"]]) {
        acknowledged(saved); return YES;
    }
    if (error && !*error) *error = save_error(code == 409 ? @"Cloud save changed during upload. Both versions are preserved." : @"Cloud upload is pending; it will retry automatically.");
    if (code == 409) status(@"conflict", @"Cloud save conflict; local progress is retained.");
    if (code == 401) status(@"error", @"Cloud authorization failed; new progress is local only.");
    return NO;
}
static BOOL restore(NSString *archive, NSError **error) {
    NSString *staging = child([g_save stringByDeletingLastPathComponent], [@"save-restore-" stringByAppendingString:NSUUID.UUID.UUIDString]);
    if (!dm_save_archive_restore(archive, staging, error)) return NO;
    BOOL ok = replace_directory(staging, g_save, error);
    if (!ok) [NSFileManager.defaultManager removeItemAtPath:staging error:nil];
    return ok;
}
static BOOL cache_remote(NSDictionary *head, NSError **error) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *staging = child(g_store, [@"download-" stringByAppendingString:NSUUID.UUID.UUIDString]);
    if (![fm createDirectoryAtPath:staging withIntermediateDirectories:NO attributes:nil error:error]) return NO;
    NSString *archive = child(staging, @"archive.nxsave"); NSInteger code = 0;
    exchange(@"GET", [@"/v1/snapshots/" stringByAppendingString:head[@"revision"]], nil, nil, archive, &code, error);
    struct stat st;
    BOOL ok = code == 200 && !stat(archive.fileSystemRepresentation, &st) && st.st_size == [head[@"bytes"] longLongValue] &&
        [dm_save_archive_hash(archive, error) isEqual:head[@"sha256"]];
    if (ok) ok = write_json(head, child(staging, @"receipt.json"), error) && replace_directory(staging, child(g_store, @"pending"), error);
    if (!ok) { [fm removeItemAtPath:staging error:nil]; if (error && !*error) *error = save_error(@"Downloaded save did not pass integrity checks."); }
    return ok;
}
static void synced_status(void) {
    pthread_mutex_lock(&g_capture_lock);
    BOOL waiting = atomic_load(&g_capturing) || atomic_load(&g_capture_failed) || [NSFileManager.defaultManager fileExistsAtPath:child(g_store, @"capture")];
    status(waiting ? @"pending" : @"synced", waiting ? @"New save waiting to upload..." : @"Cloud save up to date");
    pthread_mutex_unlock(&g_capture_lock);
}
static void sync_pending(void) {
    @autoreleasepool {
        pthread_mutex_lock(&g_capture_lock); BOOL captured = [NSFileManager.defaultManager fileExistsAtPath:child(g_store, @"capture")] || [NSFileManager.defaultManager fileExistsAtPath:child(g_store, @"processing")]; pthread_mutex_unlock(&g_capture_lock);
        NSString *phase = dm_tvos_cloud_save_status()[@"state"];
        if (!captured && !atomic_load(&g_capture_failed) && !g_verify_remote && ([phase isEqual:@"synced"] || [phase isEqual:@"ready"])) return;
        NSError *error = nil;
        if (atomic_exchange(&g_capture_failed, false) && !capture(NULL, &error)) atomic_store(&g_capture_failed, true);
        if (error || !pack_capture(&error)) { status(@"pending", error.localizedDescription); return; }
        NSDictionary *receipt = pending();
        if (!receipt) { status(@"pending", @"Local save snapshot is unavailable. Save again to retry."); return; }
        if ([g_base isEqual:@"none"] && [receipt[@"files"] isEqual:@0]) { status(@"ready", @"Cloud save ready"); return; }
        if (!g_verify_remote && [receipt[@"sha256"] isEqual:g_base]) { synced_status(); return; }
        NSInteger code = 0; NSDictionary *head = remote_head(&code, &error);
        if (error || !publish(receipt, head, &error)) {
            NSString *phase = dm_tvos_cloud_save_status()[@"state"];
            if (code == 401) status(@"error", @"Cloud authorization failed; new progress is local only.");
            else if (![phase isEqual:@"conflict"] && ![phase isEqual:@"error"]) status(@"pending", @"Cloud save pending; retrying automatically.");
            return;
        }
        g_verify_remote = NO;
        synced_status();
    }
}
void dm_tvos_cloud_save_resume(void) {
    if (!atomic_load(&g_ready) || atomic_exchange(&g_scheduled, true)) return;
    dispatch_async(g_queue, ^{ atomic_store(&g_scheduled, false); sync_pending(); });
}
// Called with the filesystem transaction locks held. Only the copy-on-write
// capture and its file inventory are made here; hashing and networking run later.
void front_save_committed(const char *directory) {
    if (!atomic_load(&g_ready)) return;
    @autoreleasepool {
        atomic_store(&g_capturing, true);
        atomic_fetch_add(&g_capture_generation, 1);
        status(@"pending", @"Save waiting to upload...");
        NSError *error = nil;
        BOOL ok = capture(directory, &error); atomic_store(&g_capture_failed, !ok);
        atomic_store(&g_capturing, false);
        status(@"pending", ok ? @"Save waiting to upload..." : @"Local save captured incompletely; retrying.");
        dm_tvos_cloud_save_resume();
    }
}

static BOOL prepare_save(void (^progress)(NSString *), NSError **error) {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm createDirectoryAtPath:g_store withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    NSDictionary *record = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"NXCloudSave"];
    BOOL matching = [record[@"endpoint"] isEqual:g_endpoint];
    g_base = matching && is_hash(record[@"base"]) ? record[@"base"] : @"none";
    g_flight = matching && is_hash(record[@"flight"]) ? record[@"flight"] : nil;
    NSError *capture_error = nil;
    BOOL capture_ok = pack_capture(&capture_error);
    NSDictionary *local = pending();
    // Preserve an unexpected, nonempty local tree instead of silently rolling it
    // back. This can happen if capture failed after a successful local commit.
    if (local) {
        NSArray *entries = dm_save_tree_index(g_save, error);
        if (!entries) return NO;
        BOOL has_files = NO;
        for (NSDictionary *entry in entries) if ([entry[@"type"] isEqual:@"file"]) { has_files = YES; break; }
        if (has_files) {
            NSString *probe = child(g_store, [@"probe-" stringByAppendingString:NSUUID.UUID.UUIDString]);
            NSDictionary *current = dm_save_archive_create(g_save, probe, error);
            [fm removeItemAtPath:probe error:nil];
            if (!current) return NO;
            if (![current[@"sha256"] isEqual:local[@"sha256"]]) {
                if (error) *error = save_error(@"Local save differs from the last complete snapshot. Both versions were kept; recovery needs review.");
                return NO;
            }
        }
    }
    NSInteger code = 0; NSError *network_error = nil;
    progress(@"Checking cloud save...");
    NSDictionary *head = remote_head(&code, &network_error);
    if (network_error) {
        if (!local) { if (error) *error = network_error; return NO; }
        if (!restore(child(child(g_store, @"pending"), @"archive.nxsave"), error)) return NO;
        g_verify_remote = YES;
        status(code == 401 ? @"error" : @"pending", code == 401 ? @"Cloud authorization failed; new progress is local only." : @"Playing from a verified local save; cloud sync is pending."); return YES;
    }
    if (!head && ![g_base isEqual:@"none"]) { if (error) *error = save_error(@"The previous cloud save is missing. Local progress was kept."); return NO; }
    if (head && (!local || ([local[@"sha256"] isEqual:g_base] && ![head[@"revision"] isEqual:g_base]))) {
        progress(@"Restoring cloud save...");
        if (!cache_remote(head, error)) return NO;
        local = pending();
    }
    if (!head && !local) {
        if (!capture_ok) { if (error) *error = capture_error; return NO; }
        if (!capture(g_save.fileSystemRepresentation, error) || !pack_capture(error)) return NO;
        local = pending();
        if (!local) { if (error) *error = save_error(@"Cannot verify the initial local save."); return NO; }
        if (![local[@"files"] unsignedIntegerValue]) { status(@"ready", @"Cloud save ready"); return YES; }
    }
    if (head && ![head[@"revision"] isEqual:local[@"sha256"]] && ![head[@"revision"] isEqual:g_base] && ![head[@"revision"] isEqual:g_flight]) {
        if (error) *error = save_error(@"Local and cloud saves have different unsynced changes. Both versions are preserved."); return NO;
    }
    if (!restore(child(child(g_store, @"pending"), @"archive.nxsave"), error)) return NO;
    if (!capture_ok) { [fm removeItemAtPath:child(g_store, @"capture") error:nil]; [fm removeItemAtPath:child(g_store, @"processing") error:nil]; }
    NSError *upload_error = nil;
    if (!publish(local, head, &upload_error)) {
        g_verify_remote = YES;
        if ([dm_tvos_cloud_save_status()[@"state"] isEqual:@"conflict"]) { if (error) *error = upload_error; return NO; }
        if (![dm_tvos_cloud_save_status()[@"state"] isEqual:@"error"]) status(@"pending", @"Verified local save ready; cloud upload is pending.");
        return YES;
    }
    status(@"synced", @"Cloud save up to date"); return YES;
}
void dm_tvos_cloud_save_prepare(NSString *state, void (^progress)(NSString *), void (^completion)(BOOL)) {
    const char *url = getenv("DM_CLOUD_SAVE_URL"), *token = getenv("DM_CLOUD_SAVE_TOKEN");
    if ((!url || !*url) && (!token || !*token)) { completion(YES); return; }
    g_endpoint = url ? @(url) : @"";
    while ([g_endpoint hasSuffix:@"/"]) g_endpoint = [g_endpoint substringToIndex:g_endpoint.length - 1];
    g_token = token && *token ? @(token) : load_token(g_endpoint);
    NSURLComponents *parts = [NSURLComponents componentsWithString:g_endpoint];
    if (![parts.scheme isEqual:@"https"] || !parts.host.length || parts.user || parts.password || parts.query || parts.fragment || parts.path.length || !is_hash(g_token)) {
        status(@"error", @"Cloud save configuration is invalid."); progress(@"Cloud save configuration is invalid."); completion(NO); return;
    }
    // Provision through the device configuration once. The distributable IPA
    // contains only the public endpoint, never the private cloud credential.
    if (token && *token) {
        if (!store_token(g_endpoint, g_token)) {
            status(@"error", @"Cannot persist the cloud save credential."); progress(@"Cannot persist the cloud save credential."); completion(NO); return;
        }
        unsetenv("DM_CLOUD_SAVE_TOKEN");
        NSString *settings = child(state, @"env.txt");
        NSString *text = [NSString stringWithContentsOfFile:settings encoding:NSUTF8StringEncoding error:nil];
        if (text) {
            NSMutableArray *lines = [NSMutableArray array];
            for (NSString *line in [text componentsSeparatedByString:@"\n"])
                if (![[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] hasPrefix:@"DM_CLOUD_SAVE_TOKEN="]) [lines addObject:line];
            [[lines componentsJoinedByString:@"\n"] writeToFile:settings atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }
    }
    g_store = child(state, @"cloud-save"); g_save = @(getenv("DM_SAVE"));
    g_queue = dispatch_queue_create("org.nxnative.cloud-save", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    status(@"checking", @"Checking cloud save...");
    dispatch_async(g_queue, ^{
        @autoreleasepool {
            NSError *error = nil;
            BOOL ok = prepare_save(^(NSString *message) { dispatch_async(dispatch_get_main_queue(), ^{ progress(message); }); }, &error);
            if (ok) {
                atomic_store(&g_ready, true);
                g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_queue);
                dispatch_source_set_timer(g_timer, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), 15 * NSEC_PER_SEC, NSEC_PER_SEC);
                dispatch_source_set_event_handler(g_timer, ^{ sync_pending(); }); dispatch_resume(g_timer);
            } else status(@"error", error.localizedDescription ?: @"Cloud save is not ready.");
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!ok) progress([dm_tvos_text(error.localizedDescription ?: @"Cloud save is not ready.")
                                   stringByAppendingString:dm_tvos_text(@" Reopen the game to retry.")]);
                completion(ok);
            });
        }
    });
}
