#import <Foundation/Foundation.h>

@interface DMGofileSource : NSObject
- (instancetype)initWithSession:(NSURLSession *)session;
- (NSURLRequest *)requestForChunk:(NSDictionary *)chunk;
@end
