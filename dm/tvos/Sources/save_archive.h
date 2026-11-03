#import <Foundation/Foundation.h>

// The archive contains a bounded manifest followed by regular file contents.
// Empty directories and guest-name aliases survive a restore.
NSDictionary *dm_save_archive_create(NSString *root, NSString *archive, NSError **error);
NSArray *dm_save_tree_index(NSString *root, NSError **error);
BOOL dm_save_archive_restore(NSString *archive, NSString *root, NSError **error);
NSString *dm_save_archive_hash(NSString *path, NSError **error);
extern const uint64_t DM_SAVE_ARCHIVE_LIMIT;
