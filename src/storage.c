#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "storage.h"
#include "log.h"

static const char *const ALL_FILES[] = {
    USERS_FILE, ACCOUNTS_FILE, TXNS_FILE, LOANS_FILE,
    FEEDBACK_FILE, JOURNAL_FILE, SESSION_FILE };
#define NALL (sizeof ALL_FILES / sizeof ALL_FILES[0])

Err store_init(void) {
    if (mkdir("data", 0755) == -1 && errno != EEXIST) { LOG_SYS("mkdir data"); return ERR_IO; }
    for (size_t i = 0; i < NALL; i++) {
        int fd = open(ALL_FILES[i], O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) { LOG_SYS(ALL_FILES[i]); return ERR_IO; }
        close(fd);
    }
    return ERR_OK;
}

static int *store_fds(Store *s, int i) {
    int *f[] = { &s->users, &s->accounts, &s->txns, &s->loans, &s->feedback, &s->journal };
    return f[i];
}
static const char *const STORE_PATHS[] = {
    USERS_FILE, ACCOUNTS_FILE, TXNS_FILE, LOANS_FILE, FEEDBACK_FILE, JOURNAL_FILE };
#define NSTORE 6

void store_close(Store *s) {
    for (int i = 0; i < NSTORE; i++) {
        int *p = store_fds(s, i);
        if (*p >= 0) { close(*p); *p = -1; }
    }
}

Err store_open(Store *s) {
    for (int i = 0; i < NSTORE; i++) *store_fds(s, i) = -1;
    for (int i = 0; i < NSTORE; i++) {
        int fd = open(STORE_PATHS[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) { LOG_SYS(STORE_PATHS[i]); store_close(s); return ERR_IO; }
        *store_fds(s, i) = fd;
    }
    return ERR_OK;
}

#define OFF(recsz, id) ((off_t)(id) * (off_t)(recsz))

Err rec_read(int fd, size_t recsz, int id, void *out) {
    if (id <= 0) return ERR_INVALID;
    ssize_t n = pread(fd, out, recsz, OFF(recsz, id));
    if (n == 0) return ERR_NOT_FOUND;               /* past end of file */
    if (n != (ssize_t)recsz) {
        if (n < 0) LOG_SYS("pread"); else LOG_E("short read id=%d", id);
        return ERR_IO;
    }
    return ERR_OK;      /* caller must still treat an all-zero record (id==0) as empty */
}

Err rec_write(int fd, size_t recsz, int id, const void *rec) {
    if (id <= 0) return ERR_INVALID;
    ssize_t n = pwrite(fd, rec, recsz, OFF(recsz, id));
    if (n != (ssize_t)recsz) {
        if (n < 0) LOG_SYS("pwrite"); else LOG_E("short write id=%d", id);
        return ERR_IO;
    }
    return ERR_OK;
}

Err rec_sync(int fd) {
    if (fsync(fd) == -1) { LOG_SYS("fsync"); return ERR_IO; }
    return ERR_OK;
}

Err rec_lock(int fd, size_t recsz, int id, LockMode m) {
    if (id <= 0) return ERR_INVALID;
    return lock_range(fd, m, OFF(recsz, id), (off_t)recsz, 1);
}

Err rec_unlock(int fd, size_t recsz, int id) {
    if (id <= 0) return ERR_INVALID;
    return unlock_range(fd, OFF(recsz, id), (off_t)recsz);
}

Err rec_lock_pair(int fd, size_t recsz, int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }          /* lower id first, always */
    Err e = rec_lock(fd, recsz, a, LK_EXCLUSIVE);
    if (e != ERR_OK || a == b) return e;
    e = rec_lock(fd, recsz, b, LK_EXCLUSIVE);
    if (e != ERR_OK) rec_unlock(fd, recsz, a);
    return e;
}

Err rec_unlock_pair(int fd, size_t recsz, int a, int b) {
    Err e1 = rec_unlock(fd, recsz, a);
    Err e2 = (a == b) ? ERR_OK : rec_unlock(fd, recsz, b);
    return e1 != ERR_OK ? e1 : e2;
}

/* Slot 0 is never real data: it is the lock that serialises ID allocation. */
int rec_alloc(int fd, size_t recsz) {
    char zero[1024] = {0};
    if (recsz > sizeof zero) return -1;
    if (lock_range(fd, LK_EXCLUSIVE, 0, (off_t)recsz, 1) != ERR_OK) return -1;
    int id = -1;
    struct stat st;
    if (fstat(fd, &st) == -1) {
        LOG_SYS("fstat");
    } else {
        off_t next = (st.st_size + (off_t)recsz - 1) / (off_t)recsz;   /* round up if torn */
        if (next < 1) next = 1;
        if (pwrite(fd, zero, recsz, next * (off_t)recsz) == (ssize_t)recsz) id = (int)next;
        else LOG_SYS("pwrite alloc");
    }
    unlock_range(fd, 0, (off_t)recsz);
    return id;
}
