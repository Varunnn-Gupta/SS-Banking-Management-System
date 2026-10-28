#ifndef LOCK_H
#define LOCK_H
#include <sys/types.h>
#include "models.h"

typedef enum { LK_SHARED, LK_EXCLUSIVE } LockMode;

/* OFD byte-range lock. wait=1 blocks; wait=0 returns ERR_BUSY if held. */
Err lock_range(int fd, LockMode m, off_t start, off_t len, int wait);
Err unlock_range(int fd, off_t start, off_t len);

#endif
