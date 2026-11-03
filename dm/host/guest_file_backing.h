#pragma once
#include <stddef.h>

// Replace an owned, unused PROT_NONE reservation with a shared regular-file
// mapping. The returned descriptor belongs to the caller; closing it does not
// release the mapping. Failures preserve the reservation and return -1/errno.
int dm_guest_file_backing_map(void *reservation, size_t bytes, const char *directory);

// Set normal (0) or random (1) access advice on an owned, page-aligned shared
// file mapping without changing its contents or permissions. Returns 0 or -1/errno.
int dm_guest_file_backing_access(void *reservation, size_t bytes, int random_access);

// Deactivate only whole host pages contained in both the supplied byte range
// and an owned shared-file reservation. Darwin retains their contents. Returns
// bytes successfully advised, not bytes reclaimed; invalid ranges return zero.
size_t dm_guest_file_backing_cold(const void *reservation, size_t capacity,
                                  const void *address, size_t bytes);
