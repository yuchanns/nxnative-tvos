#import "save_archive.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

const uint64_t DM_SAVE_ARCHIVE_LIMIT = 64ull << 20;
static const NSUInteger manifest_limit = 1u << 20, entry_limit = 4096;
static const char guest_name_attribute[] = "org.d2rmobile.guest-name";

static BOOL fail(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"NXSaveArchive" code:1 userInfo:@{NSLocalizedDescriptionKey: message}];
    return NO;
}
static NSString *digest_hex(const unsigned char *digest) {
    char text[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(text + i * 2, 3, "%02x", digest[i]);
    return @(text);
}
static BOOL relative_path(id value) {
    if (![value isKindOfClass:NSString.class]) return NO;
    NSString *path = value;
    if (!path.length || [path lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 1024 || [path hasPrefix:@"/"] || [path containsString:@"\\"] ||
        [path rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound) return NO;
    for (NSString *part in [path componentsSeparatedByString:@"/"])
        if (!part.length || [part isEqualToString:@"."] || [part isEqualToString:@".."]) return NO;
    return YES;
}
static BOOL valid_hash(id value) {
    return [value isKindOfClass:NSString.class] && [value length] == 64 &&
        [value rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location == NSNotFound;
}
NSString *dm_save_archive_hash(NSString *path, NSError **error) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { fail(error, @"Cannot open save data."); return nil; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > DM_SAVE_ARCHIVE_LIMIT) {
        close(fd); fail(error, @"Invalid save data size or type."); return nil;
    }
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    unsigned char buffer[65536], digest[CC_SHA256_DIGEST_LENGTH];
    ssize_t n; uint64_t size = 0;
    while ((n = read(fd, buffer, sizeof buffer)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; close(fd); fail(error, @"Cannot read save data."); return nil; }
        CC_SHA256_Update(&hash, buffer, (CC_LONG)n); size += (uint64_t)n;
    }
    close(fd);
    if (size != (uint64_t)st.st_size) { fail(error, @"Save data changed while reading."); return nil; }
    CC_SHA256_Final(digest, &hash); return digest_hex(digest);
}
static BOOL write_bytes(FILE *out, const void *bytes, size_t size) {
    return fwrite(bytes, 1, size, out) == size;
}
static BOOL copy_contents(FILE *input, FILE *output, uint64_t size, NSString *expected) {
    unsigned char buffer[65536], digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    while (size) {
        size_t n = (size_t)MIN(size, sizeof buffer);
        if (fread(buffer, 1, n, input) != n || !write_bytes(output, buffer, n)) return NO;
        CC_SHA256_Update(&hash, buffer, (CC_LONG)n); size -= n;
    }
    CC_SHA256_Final(digest, &hash);
    return [digest_hex(digest) isEqualToString:expected];
}
NSArray *dm_save_tree_index(NSString *root, NSError **error) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray *paths = [[fm subpathsOfDirectoryAtPath:root error:error] sortedArrayUsingSelector:@selector(compare:)];
    if (!paths || paths.count > entry_limit) { if (paths) fail(error, @"Save has too many entries."); return nil; }
    NSMutableArray *entries = [NSMutableArray array]; uint64_t size = 0;
    for (NSString *path in paths) {
        NSString *full = [root stringByAppendingPathComponent:path]; struct stat st;
        if (!relative_path(path) || lstat(full.fileSystemRepresentation, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
            fail(error, @"Save contains an unsupported path or file type."); return nil;
        }
        NSMutableDictionary *entry = [@{@"path": path, @"type": S_ISDIR(st.st_mode) ? @"directory" : @"file"} mutableCopy];
        if (S_ISREG(st.st_mode)) {
            if (st.st_size < 0 || (uint64_t)st.st_size > DM_SAVE_ARCHIVE_LIMIT - size) { fail(error, @"Save exceeds the archive limit."); return nil; }
            entry[@"bytes"] = @((uint64_t)st.st_size); size += st.st_size;
        }
        unsigned char alias[1024];
        ssize_t n = getxattr(full.fileSystemRepresentation, guest_name_attribute, alias, sizeof alias, 0, XATTR_NOFOLLOW);
        if (n < 0 && errno != ENOATTR) { fail(error, @"Cannot read save filename metadata."); return nil; }
        if (n >= 0) entry[@"guestName"] = [[NSData dataWithBytes:alias length:(NSUInteger)n] base64EncodedStringWithOptions:0];
        [entries addObject:entry];
    }
    return entries;
}
NSDictionary *dm_save_archive_create(NSString *root, NSString *archive, NSError **error) {
    NSArray *index = dm_save_tree_index(root, error); if (!index) return nil;
    NSMutableArray *entries = [NSMutableArray array]; uint64_t size = 0; NSUInteger files = 0;
    for (NSDictionary *item in index) {
        NSMutableDictionary *entry = [item mutableCopy];
        if ([entry[@"type"] isEqual:@"file"]) {
            NSString *sha = dm_save_archive_hash([root stringByAppendingPathComponent:entry[@"path"]], error);
            if (!sha) return nil;
            entry[@"sha256"] = sha; size += [entry[@"bytes"] unsignedLongLongValue]; files++;
        }
        [entries addObject:entry];
    }
    NSData *manifest = [NSJSONSerialization dataWithJSONObject:@{@"version": @1, @"entries": entries} options:NSJSONWritingSortedKeys error:error];
    if (!manifest || manifest.length > manifest_limit || size + manifest.length + 12 > DM_SAVE_ARCHIVE_LIMIT) {
        fail(error, @"Save manifest exceeds the archive limit."); return nil;
    }
    int fd = open(archive.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { fail(error, @"Cannot create save archive."); return nil; }
    FILE *out = fdopen(fd, "wb"); if (!out) { close(fd); unlink(archive.fileSystemRepresentation); fail(error, @"Cannot open save archive."); return nil; }
    uint32_t length = CFSwapInt32HostToLittle((uint32_t)manifest.length);
    BOOL ok = write_bytes(out, "NXSAVE01", 8) && write_bytes(out, &length, 4) && write_bytes(out, manifest.bytes, manifest.length);
    for (NSDictionary *entry in entries) {
        if (!ok || ![entry[@"type"] isEqualToString:@"file"]) continue;
        NSString *path = [root stringByAppendingPathComponent:entry[@"path"]];
        int input_fd = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW);
        FILE *input = input_fd < 0 ? NULL : fdopen(input_fd, "rb");
        if (!input) { if (input_fd >= 0) close(input_fd); ok = NO; break; }
        ok = copy_contents(input, out, [entry[@"bytes"] unsignedLongLongValue], entry[@"sha256"]) && fgetc(input) == EOF && !ferror(input);
        fclose(input);
    }
    if (fflush(out) || fsync(fd)) ok = NO;
    if (fclose(out)) ok = NO;
    if (!ok) { unlink(archive.fileSystemRepresentation); fail(error, @"Save archive could not be completed."); return nil; }
    NSString *sha = dm_save_archive_hash(archive, error);
    if (!sha) { unlink(archive.fileSystemRepresentation); return nil; }
    return @{@"sha256": sha, @"bytes": @(size + manifest.length + 12), @"files": @(files)};
}
BOOL dm_save_archive_restore(NSString *archive, NSString *root, NSError **error) {
    int fd = open(archive.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 12 || (uint64_t)st.st_size > DM_SAVE_ARCHIVE_LIMIT) {
        if (fd >= 0) close(fd); return fail(error, @"Invalid save archive.");
    }
    FILE *input = fdopen(fd, "rb"); if (!input) { close(fd); return fail(error, @"Cannot read save archive."); }
    unsigned char header[12] = {0}; uint32_t length = 0;
    BOOL ok = fread(header, 1, sizeof header, input) == sizeof header && !memcmp(header, "NXSAVE01", 8);
    memcpy(&length, header + 8, 4); length = CFSwapInt32LittleToHost(length);
    if (!ok || !length || length > manifest_limit || (uint64_t)length + 12 > (uint64_t)st.st_size) { fclose(input); return fail(error, @"Invalid save manifest length."); }
    NSMutableData *data = [NSMutableData dataWithLength:length];
    if (fread(data.mutableBytes, 1, length, input) != length) { fclose(input); return fail(error, @"Incomplete save manifest."); }
    id manifest = [NSJSONSerialization JSONObjectWithData:data options:0 error:error];
    id entries = [manifest isKindOfClass:NSDictionary.class] ? manifest[@"entries"] : nil;
    if (![entries isKindOfClass:NSArray.class] || [entries count] > entry_limit || ![manifest[@"version"] isEqual:@1]) {
        fclose(input); return fail(error, @"Unsupported save manifest.");
    }
    NSMutableSet *seen = [NSMutableSet set], *directories = [NSMutableSet setWithObject:@""];
    uint64_t total = length + 12;
    for (id entry in entries) {
        if (![entry isKindOfClass:NSDictionary.class] || !relative_path(entry[@"path"]) || [seen containsObject:entry[@"path"]]) { ok = NO; break; }
        NSString *path = entry[@"path"], *parent = [path stringByDeletingLastPathComponent];
        if (![directories containsObject:parent]) { ok = NO; break; }
        [seen addObject:path];
        if ([entry[@"type"] isEqual:@"directory"]) [directories addObject:path];
        else if ([entry[@"type"] isEqual:@"file"] && [entry[@"bytes"] isKindOfClass:NSNumber.class] && valid_hash(entry[@"sha256"])) {
            double value = [entry[@"bytes"] doubleValue];
            uint64_t bytes = [entry[@"bytes"] unsignedLongLongValue];
            if (value < 0 || value != (double)bytes || bytes > DM_SAVE_ARCHIVE_LIMIT - total) { ok = NO; break; }
            total += bytes;
        } else { ok = NO; break; }
        if (entry[@"guestName"]) {
            if (![entry[@"guestName"] isKindOfClass:NSString.class]) { ok = NO; break; }
            NSData *alias = [[NSData alloc] initWithBase64EncodedString:entry[@"guestName"] options:0];
            NSString *name = alias ? [[NSString alloc] initWithData:alias encoding:NSUTF8StringEncoding] : nil;
            if (!alias.length || alias.length > 1024 || !relative_path(name) || [name containsString:@"/"]) { ok = NO; break; }
        }
    }
    if (!ok || total != (uint64_t)st.st_size) { fclose(input); return fail(error, @"Save archive entries or size are invalid."); }
    if (mkdir(root.fileSystemRepresentation, 0700)) { fclose(input); return fail(error, @"Cannot create the restore directory."); }
    for (NSDictionary *entry in entries) {
        NSString *path = [root stringByAppendingPathComponent:entry[@"path"]];
        if ([entry[@"type"] isEqual:@"directory"]) ok = !mkdir(path.fileSystemRepresentation, 0700);
        else {
            int output_fd = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
            FILE *output = output_fd < 0 ? NULL : fdopen(output_fd, "wb");
            if (!output) { if (output_fd >= 0) close(output_fd); ok = NO; }
            else {
                ok = copy_contents(input, output, [entry[@"bytes"] unsignedLongLongValue], entry[@"sha256"]);
                if (fflush(output) || fsync(output_fd)) ok = NO;
                if (fclose(output)) ok = NO;
            }
        }
        if (ok && entry[@"guestName"]) {
            NSData *alias = [[NSData alloc] initWithBase64EncodedString:entry[@"guestName"] options:0];
            ok = !setxattr(path.fileSystemRepresentation, guest_name_attribute, alias.bytes, alias.length, 0, XATTR_NOFOLLOW);
        }
        if (!ok) break;
    }
    fclose(input);
    if (!ok) { [NSFileManager.defaultManager removeItemAtPath:root error:nil]; return fail(error, @"Save restore failed integrity or filesystem checks."); }
    return YES;
}
