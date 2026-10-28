#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "handlers.h"
#include "ledger.h"
#include "money.h"
#include "log.h"

static const char *type_name(int t) {
    switch (t) {
    case TXN_DEPOSIT:      return "DEPOSIT";
    case TXN_WITHDRAW:     return "WITHDRAW";
    case TXN_TRANSFER_OUT: return "TRANSFER-OUT";
    case TXN_TRANSFER_IN:  return "TRANSFER-IN";
    case TXN_LOAN_CREDIT:  return "LOAN-CREDIT";
    default:               return "?";
    }
}

int hist_cb(const Transaction *t, void *p) {
    HistCtx *c = p;
    char when[24], amt[40], bal[40], peer[24] = "", line[MSG_LEN];
    struct tm tm;
    time_t ts = (time_t)t->ts;
    localtime_r(&ts, &tm);
    strftime(when, sizeof when, "%d-%m-%Y %H:%M", &tm);
    fmt_money(amt, sizeof amt, t->amount);
    fmt_money(bal, sizeof bal, t->balance_after);
    if (t->peer_account) snprintf(peer, sizeof peer, " (acct %d)", t->peer_account);
    snprintf(line, sizeof line, "%s  %-12s %s%s  -> bal %s", when, type_name(t->type), amt, peer, bal);
    if (reply_more(c->fd, line) != 0) { c->broken = 1; return 1; }
    c->count++;
    return 0;
}

int handle_customer(int fd, Store *s, Session *ss, const Request *rq) {
    int acct = ss->account_id;
    char m[40], msg[MSG_LEN];
    money_t bal = 0;
    Err e;

    switch (rq->op) {
    case OP_BALANCE:  e = acct_balance(s, acct, &bal); break;
    case OP_DEPOSIT:  e = acct_deposit(s, acct, rq->amount, &bal); break;
    case OP_WITHDRAW: e = acct_withdraw(s, acct, rq->amount, &bal); break;
    case OP_TRANSFER: e = acct_transfer(s, acct, rq->id1, rq->amount, &bal); break;

    case OP_HISTORY: {
        HistCtx c = { fd, 0, 0 };
        e = txn_scan(s, acct, 50, hist_cb, &c);
        if (c.broken) return -1;
        if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
        if (c.count) snprintf(msg, sizeof msg, "-- %d most recent, newest first --", c.count);
        else         snprintf(msg, sizeof msg, "No transactions yet.");
        return reply(fd, ERR_OK, c.count, msg);
    }
    case OP_FEEDBACK: {
        for (const char *p = rq->s1; *p; p++)
            if ((unsigned char)*p < 32 || *p == 127)
                return reply(fd, ERR_INVALID, 0, "Feedback may not contain control characters.");
        if (!rq->s1[0]) return reply(fd, ERR_INVALID, 0, "Feedback cannot be empty.");
        Feedback f;
        memset(&f, 0, sizeof f);
        f.user_id = ss->user_id;
        f.ts = (int64_t)time(NULL);
        snprintf(f.text, sizeof f.text, "%s", rq->s1);
        int id = rec_alloc(s->feedback, sizeof f);          /* slot is ours alone */
        if (id < 0) return reply(fd, ERR_IO, 0, err_text(ERR_IO));
        f.id = id;
        e = rec_lock(s->feedback, sizeof f, id, LK_EXCLUSIVE);
        if (e == ERR_OK) {
            e = rec_write(s->feedback, sizeof f, id, &f);
            if (e == ERR_OK) e = rec_sync(s->feedback);
            rec_unlock(s->feedback, sizeof f, id);
        }
        return reply(fd, e, 0, e == ERR_OK ? "Thank you for your feedback." : err_text(e));
    }
    default:
        return reply(fd, ERR_INVALID, 0, "Not implemented yet.");
    }

    if (rq->op != OP_BALANCE)
        LOG_I("op=%d acct=%d to=%d amount=%lld result=%d", rq->op, acct, rq->id1, (long long)rq->amount, e);
    if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
    fmt_money(m, sizeof m, bal);
    snprintf(msg, sizeof msg, "%s: %s", rq->op == OP_BALANCE ? "Balance" : "Done. New balance", m);
    return reply(fd, ERR_OK, bal, msg);
}
