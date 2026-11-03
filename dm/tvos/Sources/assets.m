// Install immutable game resources in bounded chunks before starting the guest.
#import <Foundation/Foundation.h>
#import "gofile.h"
#import "asset_source.h"
#import "ui_text.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static NSDictionary *g_asset_status;
static NSObject *status_lock(void) {
    static NSObject *lock; static dispatch_once_t once;
    dispatch_once(&once, ^{ lock = [NSObject new]; }); return lock;
}
NSDictionary *dm_tvos_asset_status(void) {
    @synchronized(status_lock()) { return g_asset_status ?: @{@"stage": @"idle"}; }
}
static void report_details(NSString *stage, NSUInteger done, NSUInteger total, NSString *message,
                           double fraction, NSDictionary *details, void (^progress)(NSString *)) {
    @synchronized(status_lock()) {
        NSMutableDictionary *status = [@{@"stage": stage, @"completed_chunks": @(done), @"total_chunks": @(total),
            @"message": message, @"fraction": @(MAX(0, MIN(1, fraction))), @"active_downloads": @0} mutableCopy];
        [status addEntriesFromDictionary:details ?: @{}];
        g_asset_status = [status copy];
    }
    fprintf(stderr, "assets: %s (%lu / %lu chunks, %.1f%%)\n", stage.UTF8String,
            (unsigned long)done, (unsigned long)total, fraction * 100);
    if (progress) dispatch_async(dispatch_get_main_queue(), ^{ progress(message); });
}
static void report(NSString *stage, NSUInteger done, NSUInteger total, NSString *message,
                   double fraction, void (^progress)(NSString *)) {
    report_details(stage, done, total, message, fraction, nil, progress);
}
static BOOL relative_path(NSString *name) {
    if (!name.length || [name hasPrefix:@"/"] ||
        [name rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound) return NO;
    for (NSString *part in [name componentsSeparatedByString:@"/"])
        if (!part.length || [part isEqualToString:@"."] || [part isEqualToString:@".."]) return NO;
    return YES;
}
static BOOL digest_file(NSString *path, uint8_t digest[CC_SHA256_DIGEST_LENGTH]) {
    FILE *file = fopen(path.fileSystemRepresentation, "rb"); if (!file) return NO;
    CC_SHA256_CTX context; CC_SHA256_Init(&context);
    uint8_t bytes[65536]; size_t count;
    while ((count = fread(bytes, 1, sizeof bytes, file))) CC_SHA256_Update(&context, bytes, (CC_LONG)count);
    BOOL ok = !ferror(file); fclose(file); CC_SHA256_Final(digest, &context); return ok;
}
static NSString *digest_hex(const uint8_t *digest) {
    char hex[65]; for (unsigned i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return [NSString stringWithUTF8String:hex];
}
static BOOL parents(NSString *root, NSString *name, BOOL create, NSMutableSet<NSString *> *verified) {
    NSString *path = root;
    NSArray *parts = [name componentsSeparatedByString:@"/"];
    for (NSUInteger i = 0; i + 1 < parts.count; i++) {
        path = [path stringByAppendingPathComponent:parts[i]];
        if ([verified containsObject:path]) continue;
        if (create && mkdir(path.fileSystemRepresentation, 0755) && errno != EEXIST) return NO;
        struct stat info;
        if (lstat(path.fileSystemRepresentation, &info) || !S_ISDIR(info.st_mode)) return NO;
        [verified addObject:path];
    }
    return YES;
}
static NSArray *unpack_fragment(FILE *input, NSString *root, NSString *name,
                                uint64_t total, uint64_t offset, uint64_t size, const uint8_t checksum[32]) {
    // Verify before modifying an existing file: a bad retry must not corrupt a
    // fragment that a previous receipt already accepted.
    off_t start = ftello(input);
    if (start < 0) return nil;
    CC_SHA256_CTX context; CC_SHA256_Init(&context);
    uint8_t buffer[65536], digest[32];
    for (uint64_t copied = 0; copied < size;) {
        size_t count = (size_t)MIN((uint64_t)sizeof buffer, size - copied);
        if (fread(buffer, 1, count, input) != count) return nil;
        CC_SHA256_Update(&context, buffer, (CC_LONG)count); copied += count;
    }
    CC_SHA256_Final(digest, &context);
    if (memcmp(digest, checksum, 32) || fseeko(input, start, SEEK_SET)) return nil;
    NSString *destination = [root stringByAppendingPathComponent:name];
    int fd = open(destination.fileSystemRepresentation, O_RDWR | O_NOFOLLOW);
    struct stat info;
    if (fd < 0 && errno != ENOENT) return nil;
    if (fd >= 0 && (fstat(fd, &info) || !S_ISREG(info.st_mode))) { close(fd); return nil; }
    if (fd < 0 || (uint64_t)info.st_size != total) {
        if (fd >= 0) close(fd);
        NSString *template = [root stringByAppendingPathComponent:@".nxasset-XXXXXX"];
        char *temporary = strdup(template.fileSystemRepresentation);
        fd = mkstemp(temporary);
        BOOL ok = fd >= 0 && !ftruncate(fd, (off_t)total) && !rename(temporary, destination.fileSystemRepresentation);
        if (!ok) { if (fd >= 0) close(fd); unlink(temporary); free(temporary); return nil; }
        free(temporary);
    }
    BOOL ok = YES;
    for (uint64_t copied = 0; ok && copied < size;) {
        size_t count = (size_t)MIN((uint64_t)sizeof buffer, size - copied);
        if (fread(buffer, 1, count, input) != count) { ok = NO; break; }
        for (size_t written = 0; written < count;) {
            ssize_t n = pwrite(fd, buffer + written, count - written, (off_t)(offset + copied + written));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = NO; break; }
            written += (size_t)n;
        }
        copied += count;
    }
    ok = ok && !fsync(fd) && !fstat(fd, &info);
    if (close(fd)) ok = NO;
    if (!ok) return nil;
    // File length alone cannot prove a fragment survived cache eviction. A new
    // sparse file has the same length while its other fragments are missing.
    // Bind every receipt to this file instance, including inode reuse protection.
    return @[name, @(total), @(offset), @(size), @((uint64_t)info.st_dev), @((uint64_t)info.st_ino),
             @(info.st_birthtimespec.tv_sec), @(info.st_birthtimespec.tv_nsec)];
}
NSArray *dm_tvos_assets_unpack(NSString *archive, NSString *root, NSUInteger expected) {
    FILE *input = fopen(archive.fileSystemRepresentation, "rb"); if (!input) return nil;
    struct stat info; uint8_t magic[8]; BOOL ok = !fstat(fileno(input), &info) && info.st_size >= 8;
    ok = ok && fread(magic, 1, 8, input) == 8;
    BOOL fragments = ok && !memcmp(magic, "NXROMFS2", 8);
    ok = ok && (fragments || !memcmp(magic, "NXROMFS1", 8));
    NSMutableArray *index = [NSMutableArray array]; NSMutableSet *names = [NSMutableSet set];
    uint8_t buffer[65536];
    while (ok && ftello(input) < info.st_size) { @autoreleasepool {
        uint8_t header[60]; size_t header_size = fragments ? 60 : 44;
        if (index.count >= expected || fread(header, 1, header_size, input) != header_size) { ok = NO; break; }
        uint32_t length; uint64_t size, total, offset = 0;
        memcpy(&length, header, 4); length = CFSwapInt32LittleToHost(length);
        memcpy(&total, header + 4, 8); total = CFSwapInt64LittleToHost(total); size = total;
        if (fragments) {
            memcpy(&offset, header + 12, 8); offset = CFSwapInt64LittleToHost(offset);
            memcpy(&size, header + 20, 8); size = CFSwapInt64LittleToHost(size);
            if (total > LLONG_MAX || offset > total || size > total - offset || (!size && total)) { ok = NO; break; }
        }
        uint64_t remaining = (uint64_t)info.st_size - (uint64_t)ftello(input);
        if (!length || length > 4096 || length > remaining || size > remaining - length) { ok = NO; break; }
        char path[4097];
        if (fread(path, 1, length, input) != length || memchr(path, 0, length)) { ok = NO; break; }
        path[length] = 0; NSString *name = [NSString stringWithUTF8String:path];
        if (!relative_path(name) || [names containsObject:name] || !parents(root, name, YES, nil)) { ok = NO; break; }
        [names addObject:name];
        if (fragments) {
            NSArray *receipt = unpack_fragment(input, root, name, total, offset, size, header + 28);
            if (!receipt) { ok = NO; break; }
            [index addObject:receipt]; continue;
        }
        NSString *destination = [root stringByAppendingPathComponent:name];
        NSString *template = [root stringByAppendingPathComponent:@".nxasset-XXXXXX"];
        char *temporary = strdup(template.fileSystemRepresentation); int fd = mkstemp(temporary);
        FILE *output = fd >= 0 ? fdopen(fd, "wb") : NULL;
        if (!output) { if (fd >= 0) { close(fd); unlink(temporary); } free(temporary); ok = NO; break; }
        CC_SHA256_CTX context; CC_SHA256_Init(&context);
        for (uint64_t copied = 0; ok && copied < size;) {
            size_t count = (size_t)MIN((uint64_t)sizeof buffer, size - copied);
            ok = fread(buffer, 1, count, input) == count && fwrite(buffer, 1, count, output) == count;
            if (ok) { CC_SHA256_Update(&context, buffer, (CC_LONG)count); copied += count; }
        }
        uint8_t digest[32]; CC_SHA256_Final(digest, &context);
        if (fclose(output)) ok = NO;
        ok = ok && !memcmp(digest, header + 12, 32);
        if (ok && rename(temporary, destination.fileSystemRepresentation)) ok = NO;
        if (!ok) unlink(temporary);
        free(temporary);
        if (ok) [index addObject:@[name, @(size)]];
    } }
    fclose(input);
    return ok && index.count == expected ? index : nil;
}
static BOOL installed(NSString *index_path, NSString *root, NSUInteger count) {
    NSData *data = [NSData dataWithContentsOfFile:index_path]; if (!data) return NO;
    id index = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (![index isKindOfClass:NSArray.class] || [index count] != count) return NO;
    NSMutableSet<NSString *> *directories = [NSMutableSet set];
    for (id row in index) { @autoreleasepool {
        if (![row isKindOfClass:NSArray.class] || ([row count] != 2 && [row count] != 8) || ![row[0] isKindOfClass:NSString.class] ||
            ![row[1] isKindOfClass:NSNumber.class] || !relative_path(row[0])) return NO;
        struct stat info; NSString *path = [root stringByAppendingPathComponent:row[0]];
        if (!parents(root, row[0], NO, directories) || lstat(path.fileSystemRepresentation, &info) || !S_ISREG(info.st_mode) ||
            (uint64_t)info.st_size != [row[1] unsignedLongLongValue]) return NO;
        if ([row count] == 8) {
            for (NSUInteger i = 2; i < 8; i++) if (![row[i] isKindOfClass:NSNumber.class]) return NO;
            uint64_t total = [row[1] unsignedLongLongValue], offset = [row[2] unsignedLongLongValue], size = [row[3] unsignedLongLongValue];
            if (offset > total || size > total - offset || (!size && total) ||
                (uint64_t)info.st_dev != [row[4] unsignedLongLongValue] || (uint64_t)info.st_ino != [row[5] unsignedLongLongValue] ||
                info.st_birthtimespec.tv_sec != [row[6] longLongValue] || info.st_birthtimespec.tv_nsec != [row[7] longLongValue]) return NO;
        }
    } }
    return YES;
}
static BOOL download(NSURLSession *session, NSURLRequest *request, NSString *path, uint64_t expected,
                     BOOL (^cancelled)(void), void (^received)(uint64_t)) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0); __block BOOL ok = NO;
    NSURLSessionDownloadTask *task = [session downloadTaskWithRequest:request completionHandler:^(NSURL *location, NSURLResponse *response, NSError *error) {
        struct stat info;
        if (!error && [(NSHTTPURLResponse *)response statusCode] == 200 && location &&
            !stat(location.fileSystemRepresentation, &info) && (uint64_t)info.st_size == expected)
            ok = [NSFileManager.defaultManager moveItemAtURL:location toURL:[NSURL fileURLWithPath:path] error:NULL];
        else fprintf(stderr, "assets: download failed (%ld): %s\n", (long)[(NSHTTPURLResponse *)response statusCode], error.localizedDescription.UTF8String ?: "invalid response");
        dispatch_semaphore_signal(done);
    }];
    [task resume];
    uint64_t previous = UINT64_MAX;
    while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC))) {
        if (cancelled()) [task cancel];
        uint64_t bytes = (uint64_t)MAX((int64_t)0, task.countOfBytesReceived);
        if (bytes != previous) { received(MIN(bytes, expected)); previous = bytes; }
    }
    if (ok) received(expected);
    return ok;
}
static NSURLRequest *download_request(NSDictionary *job, DMGofileSource *gofile, DMAssetSource *source) {
    // The reader token and JavaScript context are shared; only the resulting
    // immutable download requests may be used concurrently.
    if (job[@"chunk"][@"gofile_id"]) { @synchronized(gofile) { return [gofile requestForChunk:job[@"chunk"]]; } }
    return [source requestForGroup:job[@"source"] name:job[@"chunk"][@"name"]];
}
static BOOL install_pending(NSArray<NSDictionary *> *pending, NSString *directory, NSURLSession *session,
                            NSURLRequest *(^request_for_job)(NSDictionary *), NSUInteger *completed,
                            NSUInteger total, void (^progress)(NSString *)) {
    const NSUInteger workers = MIN((NSUInteger)4, pending.count);
    if (!workers) return YES;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *temporary_root = [directory stringByAppendingPathComponent:@"temp"];
    if (![fm createDirectoryAtPath:temporary_root withIntermediateDirectories:YES attributes:nil error:NULL]) return NO;
    NSObject *state = [NSObject new], *commit = [NSObject new];
    __block BOOL failed = NO;
    __block NSUInteger next = 0, done = *completed, downloaded = 0, active = 0;
    __block uint64_t committed_bytes = 0;
    uint64_t pending_bytes = 0;
    for (NSDictionary *job in pending) pending_bytes += [job[@"chunk"][@"bytes"] unsignedLongLongValue];
    NSMutableArray<NSNumber *> *received = [NSMutableArray array];
    for (NSUInteger worker = 0; worker < workers; worker++) [received addObject:@0];
    BOOL (^cancelled)(void) = ^BOOL { @synchronized(state) { return failed; } };
    // Called under state so snapshots and main-queue notifications stay ordered.
    void (^publish)(void) = ^{
        if (failed) return;
        uint64_t bytes = committed_bytes;
        for (NSNumber *count in received) bytes += count.unsignedLongLongValue;
        NSString *message = active
            ? [NSString stringWithFormat:dm_tvos_text(@"Downloading game files%@: %lu / %lu"), @"",
               (unsigned long)MIN(downloaded + 1, pending.count), (unsigned long)pending.count]
            : [NSString stringWithFormat:dm_tvos_text(@"Preparing game resources: %lu / %lu"),
               (unsigned long)MIN(downloaded + 1, pending.count), (unsigned long)pending.count];
        // Receiving the last byte is not sufficient to start the guest.
        double fraction = pending_bytes ? MIN(0.999, (double)bytes / pending_bytes) : 0;
        report_details(active ? @"downloading" : @"preparing", done, total, message, fraction,
            @{@"active_downloads": @(active), @"max_parallel_downloads": @(workers),
              @"downloaded_bytes": @(bytes), @"total_download_bytes": @(pending_bytes)}, progress);
    };
    NSOperationQueue *queue = [NSOperationQueue new];
    queue.maxConcurrentOperationCount = workers;
    queue.qualityOfService = NSQualityOfServiceUtility;
    // Each worker retains its slot until unpack and receipt commit finish. This
    // bounds downloading + waiting archives to four, even with a slow disk.
    for (NSUInteger worker = 0; worker < workers; worker++) [queue addOperationWithBlock:^{
        while (YES) { @autoreleasepool {
            NSDictionary *job;
            @synchronized(state) {
                if (failed || next == pending.count) return;
                job = pending[next++];
            }
            NSDictionary *chunk = job[@"chunk"];
            uint64_t expected = [chunk[@"bytes"] unsignedLongLongValue];
            NSString *temporary = [temporary_root stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
            BOOL ok = NO;
            for (unsigned attempt = 0; attempt < 3 && !ok && !cancelled(); attempt++) {
                // Backoff remains interruptible when a sibling encounters a fatal error.
                for (unsigned tick = 0; tick < 50 * attempt && !cancelled(); tick++) [NSThread sleepForTimeInterval:0.1];
                if (cancelled()) break;
                NSURLRequest *request = request_for_job(job);
                if (!request) continue;
                @synchronized(state) {
                    if (failed) break;
                    received[worker] = @0;
                    active++; publish();
                }
                fprintf(stderr, "assets: fetching %s from %s (attempt %u)\n",
                        [chunk[@"name"] UTF8String], request.URL.host.UTF8String, attempt + 1);
                ok = download(session, request, temporary, expected, cancelled, ^(uint64_t bytes) {
                    @synchronized(state) { received[worker] = @(bytes); publish(); }
                });
                @synchronized(state) { active--; publish(); }
                uint8_t digest[32];
                ok = ok && !cancelled() && digest_file(temporary, digest) && [digest_hex(digest) isEqualToString:chunk[@"sha256"]];
                @synchronized(state) {
                    if (!ok) received[worker] = @0;
                    publish();
                }
                if (!ok) [fm removeItemAtPath:temporary error:NULL];
            }
            if (ok) { @synchronized(commit) {
                // Fragments from several archives can target the same sparse file.
                // Serialize both file mutation and the receipt that acknowledges it.
                NSArray *index = !cancelled()
                    ? dm_tvos_assets_unpack(temporary, job[@"root"], [chunk[@"files"] unsignedIntegerValue]) : nil;
                ok = index && [[NSJSONSerialization dataWithJSONObject:index options:0 error:NULL] writeToFile:job[@"index"] atomically:YES];
                @synchronized(state) {
                    if (ok) { done++; downloaded++; committed_bytes += expected; received[worker] = @0; }
                    else failed = YES;
                    publish();
                }
            } } else { @synchronized(state) { failed = YES; } }
            [fm removeItemAtPath:temporary error:NULL];
        } }
    }];
    // Cancellation callbacks finish before the caller publishes a terminal state.
    [queue waitUntilAllOperationsAreFinished];
    *completed = done;
    return !failed;
}
void dm_tvos_assets_prepare(NSString *directory, void (^progress)(NSString *), void (^completion)(BOOL)) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ @autoreleasepool {
        NSString *manifest_path = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm/assets-manifest.json"];
        NSData *data = [NSData dataWithContentsOfFile:manifest_path];
        NSDictionary *manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
        NSArray *groups = [manifest isKindOfClass:NSDictionary.class] ? manifest[@"groups"] : nil;
        BOOL ok = [manifest isKindOfClass:NSDictionary.class] && [manifest[@"version"] intValue] == 2 &&
            [groups isKindOfClass:NSArray.class] && groups.count;
        NSUInteger total = 0;
        __block NSUInteger completed = 0, checked = 0;
        NSMutableArray<NSDictionary *> *checks = [NSMutableArray array];
        NSMutableArray<NSDictionary *> *pending = [NSMutableArray array];
        if (ok) for (NSDictionary *group in groups) {
            if (![group isKindOfClass:NSDictionary.class] || ![group[@"chunks"] isKindOfClass:NSArray.class] ||
                ![@[@"romfs", @"msl", @"astc"] containsObject:group[@"root"]] ||
                ![@[@"assets", @"shaders", @"astc"] containsObject:group[@"source"]]) { ok = NO; break; }
            total += [group[@"chunks"] count];
        }
        NSFileManager *fm = NSFileManager.defaultManager;
        for (NSDictionary *group in ok ? groups : @[]) {
        if (!ok) break;
        NSString *root = [directory stringByAppendingPathComponent:group[@"root"]];
        NSString *indexes = [[directory stringByAppendingPathComponent:@"asset-index"] stringByAppendingPathComponent:group[@"root"]];
        ok = [fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:NULL] &&
            [fm createDirectoryAtPath:indexes withIntermediateDirectories:YES attributes:nil error:NULL];
        NSArray *chunks = group[@"chunks"];
        for (NSUInteger number = 0; ok && number < chunks.count; number++) { @autoreleasepool {
            NSDictionary *chunk = chunks[number];
            if (![chunk isKindOfClass:NSDictionary.class]) { ok = NO; break; }
            NSString *name = chunk[@"name"], *checksum = chunk[@"sha256"];
            if (![name isKindOfClass:NSString.class] || !relative_path(name) || [name containsString:@"/"] ||
                ![checksum isKindOfClass:NSString.class] || checksum.length != 64 || ![chunk[@"bytes"] unsignedLongLongValue]) { ok = NO; break; }
            NSString *index_path = [indexes stringByAppendingPathComponent:[name stringByAppendingString:@".json"]];
            [checks addObject:@{@"chunk": chunk, @"root": root, @"index": index_path, @"source": group[@"source"]}];
        } }
        }
        if (ok) {
            // Bound filesystem concurrency; each chunk owns its directory-validation cache.
            NSOperationQueue *queue = [NSOperationQueue new];
            queue.maxConcurrentOperationCount = 4;
            queue.qualityOfService = NSQualityOfServiceUtility;
            NSTimeInterval start = NSProcessInfo.processInfo.systemUptime;
            for (NSDictionary *job in checks) [queue addOperationWithBlock:^{ @autoreleasepool {
                BOOL found = installed(job[@"index"], job[@"root"], [job[@"chunk"][@"files"] unsignedIntegerValue]);
                @synchronized(pending) {
                    if (found) completed++;
                    else [pending addObject:job];
                    NSString *message = [NSString stringWithFormat:dm_tvos_text(@"Checking local game files: %lu / %lu"), (unsigned long)++checked, (unsigned long)total];
                    report(@"checking", completed, total, message, total ? (double)checked / total : 0, progress);
                }
            } }];
            [queue waitUntilAllOperationsAreFinished];
            fprintf(stderr, "assets: local check completed in %.3f s (%lu chunks, %lu missing, 4 workers)\n",
                    NSProcessInfo.processInfo.systemUptime - start, (unsigned long)checked, (unsigned long)pending.count);
        }
        // Installed resources must remain usable without a configured download service.
        DMAssetSource *source = [[DMAssetSource alloc] initWithDirectory:directory];
        NSURLSession *session = nil;
        DMGofileSource *gofile = nil;
        NSString *failure = @"Local game resources could not be verified.";
        if (ok && pending.count) {
            for (NSDictionary *job in pending) {
                id file_id = job[@"chunk"][@"gofile_id"];
                if (file_id ? (![file_id isKindOfClass:NSString.class] || ![[NSUUID alloc] initWithUUIDString:file_id])
                            : !source.ready) { ok = NO; break; }
            }
            failure = ok ? @"Required game files are missing. Check the download connection and reopen the game."
                         : @"Required game files are missing and no download source is configured.";
            if (ok) {
                NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
                configuration.timeoutIntervalForRequest = 300; configuration.timeoutIntervalForResource = 3600;
                configuration.URLCache = nil;
                configuration.HTTPMaximumConnectionsPerHost = 4;
                session = [NSURLSession sessionWithConfiguration:configuration delegate:source delegateQueue:nil];
                gofile = [[DMGofileSource alloc] initWithSession:session];
            }
        }
        if (ok) ok = install_pending(pending, directory, session, ^NSURLRequest *(NSDictionary *job) {
            return download_request(job, gofile, source);
        }, &completed, total, progress);
        // A cache file can disappear after the initial check, including while
        // another fragment is being repaired. Never launch with stale receipts.
        if (ok && pending.count) for (NSDictionary *job in checks)
            if (!installed(job[@"index"], job[@"root"], [job[@"chunk"][@"files"] unsignedIntegerValue])) { ok = NO; break; }
        [session finishTasksAndInvalidate];
        report(ok ? @"ready" : @"failed", completed, total,
               ok ? @"Local game resources ready. Starting..." : failure,
               ok ? 1 : (total ? (double)completed / total : 0), progress);
        dispatch_async(dispatch_get_main_queue(), ^{ completion(ok); });
    } });
}
