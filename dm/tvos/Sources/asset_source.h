#import <Foundation/Foundation.h>

@interface DMAssetSource : NSObject <NSURLSessionTaskDelegate>
@property (nonatomic, readonly) BOOL ready;
- (instancetype)initWithDirectory:(NSString *)directory;
- (NSURLRequest *)requestForGroup:(NSString *)group name:(NSString *)name;
@end
