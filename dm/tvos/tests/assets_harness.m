#import "../Sources/assets.m"

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 5) return 2;
    NSString *mode = [NSString stringWithUTF8String:argv[1]];
    NSString *input = [NSString stringWithUTF8String:argv[2]];
    NSString *root = [NSString stringWithUTF8String:argv[3]];
    NSUInteger count = strtoull(argv[4], NULL, 10);
    if ([mode isEqualToString:@"check"]) return installed(input, root, count) ? 0 : 1;
    NSArray *index = dm_tvos_assets_unpack(input, root, count);
    if (!index) return 1;
    NSData *data = [NSJSONSerialization dataWithJSONObject:index options:0 error:NULL];
    fwrite(data.bytes, 1, data.length, stdout); return 0;
} }
