#import "asset_source.h"
#import <Security/Security.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

@implementation DMAssetSource {
    NSURL *_endpoint;
    NSString *_authorization;
}
static NSDictionary *credential_query(void) {
    return @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
             (__bridge id)kSecAttrService: NSBundle.mainBundle.bundleIdentifier,
             (__bridge id)kSecAttrAccount: @"nxnative-assets"};
}
static NSData *load_credential(void) {
    NSMutableDictionary *query = [credential_query() mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    CFTypeRef result = NULL;
    return SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) == errSecSuccess ? CFBridgingRelease(result) : nil;
}
static BOOL store_credential(NSData *data) {
    NSDictionary *values = @{(__bridge id)kSecValueData: data,
        (__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    OSStatus result = SecItemUpdate((__bridge CFDictionaryRef)credential_query(), (__bridge CFDictionaryRef)values);
    if (result == errSecItemNotFound) {
        NSMutableDictionary *item = [credential_query() mutableCopy]; [item addEntriesFromDictionary:values];
        result = SecItemAdd((__bridge CFDictionaryRef)item, NULL);
    }
    return result == errSecSuccess;
}
- (instancetype)initWithDirectory:(NSString *)directory {
    if (!(self = [super init])) return nil;
    const char *source = getenv("DM_TV_ASSET_URL"), *mode = getenv("DM_TV_ASSET_AUTH");
    if (!source || !*source) return self;
    NSString *endpoint = @(source);
    while ([endpoint hasSuffix:@"/"]) endpoint = [endpoint substringToIndex:endpoint.length - 1];
    NSURLComponents *parts = [NSURLComponents componentsWithString:endpoint];
    if (!parts.host.length || parts.user || parts.password || parts.query || parts.fragment) return self;
    BOOL basic = mode && !strcmp(mode, "basic");
    if (!basic) {
        // The development LAN service uses its existing diagnostic bearer token.
        const char *token = getenv("DM_TV_DIAGNOSTIC_TOKEN");
        if ([@[@"http", @"https"] containsObject:parts.scheme] && token && *token) {
            _endpoint = parts.URL; _authorization = [@"Bearer " stringByAppendingString:@(token)];
        }
        return self;
    }
    if (![parts.scheme isEqual:@"https"]) return self;
    // Provision once through the device container. Neither the password nor an
    // administrator token belongs in the distributable app or its environment.
    NSString *seed = [directory stringByAppendingPathComponent:@"asset-credential.json"];
    struct stat info;
    BOOL provision = !lstat(seed.fileSystemRepresentation, &info);
    if (provision && (!S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 8192)) return self;
    NSData *data = provision ? [NSData dataWithContentsOfFile:seed] : load_credential();
    id credential = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![credential isKindOfClass:NSDictionary.class] || ![credential[@"endpoint"] isEqual:endpoint]) return self;
    NSString *username = credential[@"username"], *password = credential[@"password"];
    if (![username isKindOfClass:NSString.class] || !username.length || [username containsString:@":"] ||
        ![password isKindOfClass:NSString.class] || !password.length ||
        [username rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound ||
        [password rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound) return self;
    if (provision) {
        if (!store_credential(data) || ![NSFileManager.defaultManager removeItemAtPath:seed error:nil]) {
            fprintf(stderr, "assets: cannot finish provisioning the download credential\n"); return self;
        }
        fprintf(stderr, "assets: download credential stored in Keychain; provisioning file removed\n");
    }
    NSData *pair = [[NSString stringWithFormat:@"%@:%@", username, password] dataUsingEncoding:NSUTF8StringEncoding];
    _endpoint = parts.URL;
    _authorization = [@"Basic " stringByAppendingString:[pair base64EncodedStringWithOptions:0]];
    return self;
}
- (BOOL)ready { return _endpoint && _authorization.length; }
- (NSURLRequest *)requestForGroup:(NSString *)group name:(NSString *)name {
    if (!self.ready) return nil;
    NSURL *url = [[_endpoint URLByAppendingPathComponent:group] URLByAppendingPathComponent:name];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    [request setValue:_authorization forHTTPHeaderField:@"Authorization"];
    [request setValue:@"nxnative/1.0" forHTTPHeaderField:@"User-Agent"];
    return request;
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))completion {
    // Authenticated sources must proxy their bytes without redirecting device
    // credentials. Anonymous Gofile downloads retain their normal CDN redirects.
    completion([task.originalRequest valueForHTTPHeaderField:@"Authorization"] ? nil : request);
}
@end
