// CPU-only integration harness: real NSURLSession, archives, receipts, and disk I/O.
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t writer_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned writers, peak_writers;
static int observed_fsync(int fd) {
    pthread_mutex_lock(&writer_lock);
    writers++;
    if (writers > peak_writers) peak_writers = writers;
    pthread_mutex_unlock(&writer_lock);
    usleep(200000);
    int result = fsync(fd);
    pthread_mutex_lock(&writer_lock);
    writers--;
    pthread_mutex_unlock(&writer_lock);
    return result;
}
#define fsync observed_fsync
#import "../Sources/assets.m"
#undef fsync

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 4) return 2;
    NSString *input = [NSString stringWithUTF8String:argv[1]];
    NSString *directory = [NSString stringWithUTF8String:argv[2]];
    NSString *endpoint = [NSString stringWithUTF8String:argv[3]];
    NSArray *jobs = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:input] options:0 error:NULL];
    if (![jobs isKindOfClass:NSArray.class]) return 2;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableArray *pending = [NSMutableArray array];
    NSUInteger completed = 0;
    for (NSDictionary *job in jobs) {
        [fm createDirectoryAtPath:job[@"root"] withIntermediateDirectories:YES attributes:nil error:NULL];
        [fm createDirectoryAtPath:[job[@"index"] stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:NULL];
        if (installed(job[@"index"], job[@"root"], [job[@"chunk"][@"files"] unsignedIntegerValue])) completed++;
        else [pending addObject:job];
    }
    NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    configuration.HTTPMaximumConnectionsPerHost = 4;
    configuration.timeoutIntervalForRequest = 10;
    configuration.timeoutIntervalForResource = 30;
    configuration.connectionProxyDictionary = @{};
    configuration.URLCache = nil;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration];
    NSObject *lock = [NSObject new];
    __block BOOL finished = NO;
    __block NSUInteger peak_archives = 0;
    NSMutableArray *samples = [NSMutableArray array];
    dispatch_group_t monitor = dispatch_group_create();
    dispatch_group_async(monitor, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        while (YES) { @autoreleasepool {
            @synchronized(lock) { if (finished) return; }
            [samples addObject:dm_tvos_asset_status()];
            NSUInteger archives = [fm contentsOfDirectoryAtPath:[directory stringByAppendingPathComponent:@"temp"] error:NULL].count;
            peak_archives = MAX(peak_archives, archives);
            usleep(20000);
        } }
    });
    BOOL ok = install_pending(pending, directory, session, ^NSURLRequest *(NSDictionary *job) {
        NSURL *url = [NSURL URLWithString:[endpoint stringByAppendingFormat:@"/%@", job[@"chunk"][@"name"]]];
        return [NSURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:10];
    }, &completed, jobs.count, nil);
    [session finishTasksAndInvalidate];
    @synchronized(lock) { finished = YES; }
    dispatch_group_wait(monitor, DISPATCH_TIME_FOREVER);
    BOOL receipts = YES;
    if (ok) for (NSDictionary *job in jobs)
        receipts = receipts && installed(job[@"index"], job[@"root"], [job[@"chunk"][@"files"] unsignedIntegerValue]);
    NSDictionary *result = @{@"success": @(ok), @"completed": @(completed), @"receipts_valid": @(receipts),
        @"peak_writers": @(peak_writers), @"peak_archives": @(peak_archives), @"samples": samples};
    NSData *data = [NSJSONSerialization dataWithJSONObject:result options:0 error:NULL];
    fwrite(data.bytes, 1, data.length, stdout);
    return ok ? 0 : 1;
} }
