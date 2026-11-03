#import <Foundation/Foundation.h>

void dm_tvos_cloud_save_prepare(NSString *state, void (^progress)(NSString *), void (^completion)(BOOL));
void dm_tvos_cloud_save_resume(void);
NSDictionary *dm_tvos_cloud_save_status(void);
