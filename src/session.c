// src/session.c
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include "session.h"
#include "lock.h"
#include "log.h"

void session_init(Session *s) {
    s->fd = -1; s->user_id = 0; s->role = 0; s->account_id = 0;
}

int session_active(const Session *s) { return s->fd >= 0; }

/* A session is an exclusive OFD lock on byte <user_id> of sessions.lock, held on
 * this thread's own fd. Closing the fd (logout, thread exit, crash) frees it. */
Err session_acquire(Session *s, const User *u) {
    int fd = open(SESSION_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0) { LOG_SYS("open sessions.lock"); return ERR_IO; }
    Err e = lock_range(fd, LK_EXCLUSIVE, (off_t)u->id, 1, 0);   /* non-blocking */
    if (e == ERR_BUSY) { close(fd); return ERR_ALREADY_LOGGED_IN; }
    if (e != ERR_OK)   { close(fd); return e; }
    s->fd = fd;
    s->user_id = u->id;
    s->role = u->role;
    s->account_id = u->account_id;
    return ERR_OK;
}

void session_release(Session *s) {
    if (s->fd >= 0) close(s->fd);        /* closing releases the lock */
    session_init(s);
}
