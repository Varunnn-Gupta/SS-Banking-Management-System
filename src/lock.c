#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "lock.h"
#include "log.h"

static Err do_lock(int fd, short type, off_t start, off_t len, int wait) {
    struct flock fl;
    memset(&fl, 0, sizeof fl);          /* l_pid MUST be 0 for OFD locks */
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    int cmd = wait ? F_OFD_SETLKW : F_OFD_SETLK;
    while (fcntl(fd, cmd, &fl) == -1) {
        if (errno == EINTR) continue;
        if (!wait && (errno == EAGAIN || errno == EACCES)) return ERR_BUSY;
        LOG_SYS("fcntl OFD lock");
        return ERR_IO;
    }
    return ERR_OK;
}

Err lock_range(int fd, LockMode m, off_t start, off_t len, int wait) {
    return do_lock(fd, m == LK_SHARED ? F_RDLCK : F_WRLCK, start, len, wait);
}

Err unlock_range(int fd, off_t start, off_t len) {
    return do_lock(fd, F_UNLCK, start, len, 1);
}
