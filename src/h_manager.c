#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "handlers.h"
#include "auth.h"
#include "ledger.h"
#include "log.h"

static int list_accounts(int fd, Store *s) {
    struct stat st;
    if (fstat(s->accounts, &st) == -1) { LOG_SYS("fstat accounts"); return reply(fd, ERR_IO, 0, err_text(ERR_IO)); }
    int n = (int)(st.st_size / (off_t)sizeof(Account)), count = 0;
    for (int id = 1; id < n; id++) {
        Account a;
        Err e = rec_lock(s->accounts, sizeof a, id, LK_SHARED);
        if (e == ERR_OK) {
            e = rec_read(s->accounts, sizeof a, id, &a);
            rec_unlock(s->accounts, sizeof a, id);
        }
        if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
        if (a.id == 0) continue;
        const char *uname = "?", *dname = "";
        User u;
        if (user_get(s, a.user_id, &u) == ERR_OK) { uname = u.username; dname = u.name; }
        char line[MSG_LEN];
        snprintf(line, sizeof line, "acct %-4d %-16.31s %-24.47s %s",
                 a.id, uname, dname, a.active ? "ACTIVE" : "DEACTIVATED");
        if (reply_more(fd, line) != 0) return -1;
        count++;
    }
    return reply(fd, ERR_OK, count, count ? "-- end of list --" : "No accounts.");
}

static int list_feedback(int fd, Store *s) {
    struct stat st;
    if (fstat(s->feedback, &st) == -1) { LOG_SYS("fstat feedback"); return reply(fd, ERR_IO, 0, err_text(ERR_IO)); }
    int n = (int)(st.st_size / (off_t)sizeof(Feedback)), count = 0;
    for (int id = n - 1; id >= 1 && count < 50; id--) {
        Feedback f;
        Err e = rec_lock(s->feedback, sizeof f, id, LK_SHARED);
        if (e == ERR_OK) {
            e = rec_read(s->feedback, sizeof f, id, &f);
            rec_unlock(s->feedback, sizeof f, id);
        }
        if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
        if (f.id == 0) continue;
        const char *uname = "?";
        User u;
        if (user_get(s, f.user_id, &u) == ERR_OK) uname = u.username;
        char when[24], line[MSG_LEN];
        struct tm tm;
        time_t ts = (time_t)f.ts;
        localtime_r(&ts, &tm);
        strftime(when, sizeof when, "%d-%m-%Y %H:%M", &tm);
        snprintf(line, sizeof line, "[%s] %.31s: %.150s", when, uname, f.text);
        if (reply_more(fd, line) != 0) return -1;
        count++;
    }
    char msg[MSG_LEN];
    snprintf(msg, sizeof msg, count ? "-- %d most recent, newest first --" : "No feedback yet.", count);
    return reply(fd, ERR_OK, count, msg);
}

int handle_manager(int fd, Store *s, Session *ss, const Request *rq) {
    switch (rq->op) {
    case OP_MGR_LIST_ACCOUNTS: return list_accounts(fd, s);
    case OP_MGR_LIST_FEEDBACK: return list_feedback(fd, s);
    case OP_MGR_SET_ACTIVE: {
        if (rq->id2 != 0 && rq->id2 != 1)
            return reply(fd, ERR_INVALID, 0, "Enter 1 to activate or 0 to deactivate.");
        int changed = 0;
        Err e = acct_set_active(s, rq->id1, rq->id2, &changed);
        LOG_I("manager id %d: set account %d active=%d -> result %d (changed=%d)",
              ss->user_id, rq->id1, rq->id2, e, changed);
        if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
        char msg[MSG_LEN];
        snprintf(msg, sizeof msg, "Account %d is now %s%s.", rq->id1,
                 rq->id2 ? "ACTIVE" : "DEACTIVATED", changed ? "" : " (no change)");
        return reply(fd, ERR_OK, 0, msg);
    }
    default:
        return reply(fd, ERR_INVALID, 0, "Not implemented yet.");
    }
}