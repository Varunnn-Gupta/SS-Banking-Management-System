#define _GNU_SOURCE
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "journal.h"
#include "log.h"

_Static_assert(sizeof(Account) <= JDATA, "Account too big for journal");
_Static_assert(sizeof(Transaction) <= JDATA, "Transaction too big for journal");
_Static_assert(sizeof(Loan) <= JDATA, "Loan too big for journal");

/* Test hook: BANK_CRASH=before_commit|after_commit|mid_apply kills the server there. */
static void crash_point(const char *name) {
    const char *c = getenv("BANK_CRASH");
    if (c && strcmp(c, name) == 0) {
        LOG_W("crash injected at %s", name);
        kill(getpid(), SIGKILL);
    }
}

static int fd_for(Store *s, int kind, size_t *sz) {
    switch (kind) {
    case JK_ACCOUNT: *sz = sizeof(Account);     return s->accounts;
    case JK_TXN:     *sz = sizeof(Transaction); return s->txns;
    case JK_LOAN:    *sz = sizeof(Loan);        return s->loans;
    default:         return -1;
    }
}

static Err set_state(Store *s, int slot, uint8_t st) {
    off_t off = (off_t)slot * (off_t)sizeof(JEntry) + (off_t)offsetof(JEntry, state);
    if (pwrite(s->journal, &st, 1, off) != 1) { LOG_SYS("pwrite journal state"); return ERR_IO; }
    return ERR_OK;
}

/* Idempotent: writes after-images. Callers already hold the account/loan locks. */
static Err apply(Store *s, const JEntry *e) {
    int touched[4] = {0, 0, 0, 0};
    for (int i = 0; i < e->n && i < JMAX; i++) {
        const JChange *c = &e->ch[i];
        size_t sz;
        int fd = fd_for(s, c->kind, &sz);
        if (fd < 0) return ERR_INVALID;
        int locked = 0;
        Err r = ERR_OK;
        if (c->kind == JK_TXN) {            /* fresh txn slot: lock so readers never see it half-written */
            r = rec_lock(fd, sz, c->id, LK_EXCLUSIVE);
            locked = (r == ERR_OK);
        }
        if (r == ERR_OK) r = rec_write(fd, sz, c->id, c->data);
        if (locked) rec_unlock(fd, sz, c->id);
        if (r != ERR_OK) return r;
        touched[c->kind] = 1;
        if (i == 0) crash_point("mid_apply");
    }
    for (int k = 1; k <= 3; k++) {
        if (!touched[k]) continue;
        size_t sz;
        Err r = rec_sync(fd_for(s, k, &sz));
        if (r != ERR_OK) return r;
    }
    return ERR_OK;
}

Err jrnl_begin(Store *s, Jrnl *j) {
    memset(j, 0, sizeof *j);
    j->slot = rec_alloc(s->journal, sizeof(JEntry));
    if (j->slot < 0) return ERR_IO;
    j->e.id = j->slot;
    return ERR_OK;
}

Err jrnl_add(Jrnl *j, int kind, int id, const void *rec, size_t sz) {
    if (j->e.n >= JMAX || sz > JDATA) return ERR_INVALID;
    JChange *c = &j->e.ch[j->e.n++];
    c->kind = (uint8_t)kind;
    c->id = id;
    memcpy(c->data, rec, sz);
    return ERR_OK;
}

Err jrnl_commit(Store *s, Jrnl *j) {
    j->e.state = JS_PREPARED;
    Err r = rec_write(s->journal, sizeof j->e, j->slot, &j->e);
    if (r == ERR_OK) r = rec_sync(s->journal);            /* entry is durable */
    if (r != ERR_OK) return r;
    crash_point("before_commit");
    r = set_state(s, j->slot, JS_COMMITTED);
    if (r == ERR_OK) r = rec_sync(s->journal);            /* COMMIT POINT */
    if (r != ERR_OK) return r;
    crash_point("after_commit");
    if (apply(s, &j->e) != ERR_OK || set_state(s, j->slot, JS_DONE) != ERR_OK) {
        /* Past the commit point we cannot report failure. Crash-only: die while
         * still holding the locks, and recovery will redo the entry on restart. */
        LOG_E("FATAL: I/O error after commit; aborting so recovery can finish it");
        abort();
    }
    return ERR_OK;
}

Err jrnl_recover(Store *s) {
    struct stat st;
    if (fstat(s->journal, &st) == -1) { LOG_SYS("fstat journal"); return ERR_IO; }
    int n = (int)(st.st_size / (off_t)sizeof(JEntry));
    int redone = 0, dropped = 0;
    for (int id = 1; id < n; id++) {
        JEntry e;
        if (rec_read(s->journal, sizeof e, id, &e) != ERR_OK) continue;
        if (e.state == JS_COMMITTED) {
            if (apply(s, &e) != ERR_OK) { LOG_E("recovery: apply failed for entry %d", id); return ERR_IO; }
            redone++;
        } else if (e.state == JS_PREPARED) {
            dropped++;
        }
    }
    if (ftruncate(s->journal, 0) == -1) { LOG_SYS("ftruncate journal"); return ERR_IO; }
    rec_sync(s->journal);
    LOG_I("recovery: %d committed entries re-applied, %d uncommitted discarded", redone, dropped);
    return ERR_OK;
}
