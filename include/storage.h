#ifndef STORAGE_H
#define STORAGE_H
#include <stddef.h>
#include "models.h"
#include "lock.h"

/* Each thread opens its OWN Store: OFD locks are per open file description,
 * so separate fds = real mutual exclusion between threads. */
typedef struct { int users, accounts, txns, loans, feedback, journal; } Store;

Err  store_init(void);              /* once at startup: create data/ and files */
Err  store_open(Store *s);          /* once per thread */
void store_close(Store *s);

/* id is the slot number (>=1). recsz = sizeof(record type). */
Err rec_read(int fd, size_t recsz, int id, void *out);
Err rec_write(int fd, size_t recsz, int id, const void *rec);
Err rec_sync(int fd);               /* fsync: call before telling the client "success" */

Err rec_lock(int fd, size_t recsz, int id, LockMode m);
Err rec_unlock(int fd, size_t recsz, int id);

/* Lock two records in ASCENDING id order (deadlock prevention). a==b locks once. */
Err rec_lock_pair(int fd, size_t recsz, int a, int b);
Err rec_unlock_pair(int fd, size_t recsz, int a, int b);

/* Reserve the next free slot (zero-filled). Returns new id (>=1) or -1. */
int rec_alloc(int fd, size_t recsz);

#endif
