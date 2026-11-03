#pragma once
#import <Foundation/Foundation.h>

// The handler runs off the main queue and borrows its connection. The listener
// owns closing it, including when the application enters the background.
void dm_tvos_listener_start(const char *name, int port, unsigned max_connections,
                            dispatch_queue_t workers, void (^handler)(int));
