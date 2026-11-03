// Follow Gofile's public website flow with a device-local anonymous reader.
#import "gofile.h"
#import <JavaScriptCore/JavaScriptCore.h>
#include <sys/stat.h>

static NSString *const user_agent = @"nxnative/1.0";
static NSString *const reader_key = @"nxnative.gofile.reader";

static BOOL matches(NSString *value, NSString *pattern) {
    return [value isKindOfClass:NSString.class] &&
        [value rangeOfString:pattern options:NSRegularExpressionSearch].location != NSNotFound;
}

@implementation DMGofileSource {
    NSURLSession *_session;
    NSString *_reader;
    JSContext *_javascript;
    NSTimeInterval _script_time;
}
- (instancetype)initWithSession:(NSURLSession *)session {
    if ((self = [super init])) {
        _session = session;
        _reader = [NSUserDefaults.standardUserDefaults stringForKey:reader_key];
    }
    return self;
}
- (NSMutableURLRequest *)request:(NSString *)url {
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]
        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:30];
    [request setValue:user_agent forHTTPHeaderField:@"User-Agent"];
    request.HTTPShouldHandleCookies = NO;
    return request;
}
- (NSData *)data:(NSURLRequest *)request {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSData *data = nil;
    // Small API/script responses are downloaded to disk before their size is
    // checked, so an unexpected response cannot grow a memory buffer unbounded.
    NSURLSessionDownloadTask *task = [_session downloadTaskWithRequest:request
        completionHandler:^(NSURL *file, NSURLResponse *response, NSError *error) {
        struct stat info;
        if (!error && [(NSHTTPURLResponse *)response statusCode] == 200 && file &&
            !stat(file.fileSystemRepresentation, &info) && info.st_size > 0 && info.st_size <= 262144)
            data = [NSData dataWithContentsOfURL:file];
        else fprintf(stderr, "assets: Gofile metadata request failed (%ld)\n",
                     (long)[(NSHTTPURLResponse *)response statusCode]);
        dispatch_semaphore_signal(done);
    }];
    [task resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return data;
}
- (NSDictionary *)json:(NSURLRequest *)request {
    NSData *data = [self data:request];
    id value = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    return [value isKindOfClass:NSDictionary.class] ? value : nil;
}
- (BOOL)prepare {
    if (!matches(_reader, @"^[A-Za-z0-9]{32}$")) {
        NSMutableURLRequest *request = [self request:@"https://api.gofile.io/accounts"];
        request.HTTPMethod = @"POST";
        NSDictionary *result = [self json:request];
        NSDictionary *data = [result[@"data"] isKindOfClass:NSDictionary.class] ? result[@"data"] : nil;
        if (![result[@"status"] isEqual:@"ok"] || !matches(data[@"token"], @"^[A-Za-z0-9]{32}$")) return NO;
        _reader = data[@"token"];
        [NSUserDefaults.standardUserDefaults setObject:_reader forKey:reader_key];
    }
    if (!_javascript || NSProcessInfo.processInfo.systemUptime - _script_time > 3600) {
        // The site's request-signing script rotates. Load it afresh instead of
        // embedding a soon-expiring website token or an uploader credential.
        NSData *data = [self data:[self request:@"https://gofile.io/js/wt.obf.js"]];
        NSString *script = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
        if (!script.length) return NO;
        JSContext *context = [JSContext new];
        context[@"navigator"] = @{@"userAgent": user_agent, @"language": @"en-US"};
        [context evaluateScript:script];
        if (context.exception || context[@"generateWT"].isUndefined) return NO;
        _javascript = context;
        _script_time = NSProcessInfo.processInfo.systemUptime;
    }
    return YES;
}
- (NSURLRequest *)requestForChunk:(NSDictionary *)chunk {
    NSString *file_id = chunk[@"gofile_id"];
    if (![file_id isKindOfClass:NSString.class] || ![[NSUUID alloc] initWithUUIDString:file_id] || ![self prepare]) return nil;
    _javascript.exception = nil;
    NSString *website = [[_javascript[@"generateWT"] callWithArguments:@[_reader]] toString];
    if (_javascript.exception || !matches(website, @"^[a-f0-9]{64}$")) return nil;
    NSMutableURLRequest *request = [self request:[@"https://api.gofile.io/contents/" stringByAppendingString:file_id]];
    [request setValue:[@"Bearer " stringByAppendingString:_reader] forHTTPHeaderField:@"Authorization"];
    [request setValue:website forHTTPHeaderField:@"X-Website-Token"];
    [request setValue:@"en-US" forHTTPHeaderField:@"X-BL"];
    NSDictionary *result = [self json:request];
    if (! [result[@"status"] isEqual:@"ok"]) {
        if ([@[@"error-token", @"error-wrongToken", @"error-accountId"] containsObject:result[@"status"] ?: @""]) {
            _reader = nil;
            [NSUserDefaults.standardUserDefaults removeObjectForKey:reader_key];
        }
        return nil;
    }
    NSDictionary *data = [result[@"data"] isKindOfClass:NSDictionary.class] ? result[@"data"] : nil;
    if (![data[@"id"] isEqual:file_id] || ![data[@"name"] isEqual:chunk[@"name"]] ||
        ![data[@"size"] isKindOfClass:NSNumber.class] ||
        [data[@"size"] unsignedLongLongValue] != [chunk[@"bytes"] unsignedLongLongValue] ||
        ![data[@"link"] isKindOfClass:NSString.class]) return nil;
    NSURL *url = [NSURL URLWithString:data[@"link"]];
    NSString *prefix = [NSString stringWithFormat:@"/download/web/%@/", file_id];
    if (![url.scheme isEqual:@"https"] || ![url.host hasSuffix:@".gofile.io"] ||
        url.user || url.password || url.port || url.query || url.fragment ||
        ![url.path hasPrefix:prefix] || ![url.lastPathComponent isEqual:chunk[@"name"]]) return nil;
    NSMutableURLRequest *download = [self request:url.absoluteString];
    download.timeoutInterval = 300;
    [download setValue:[@"accountToken=" stringByAppendingString:_reader] forHTTPHeaderField:@"Cookie"];
    return download;
}
@end
