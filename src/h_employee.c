#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "handlers.h"
#include "auth.h"
#include "ledger.h"
#include "money.h"
#include "log.h"

static const char *acct_state(Store *s, int id) {
    Account a;
    if (id <= 0 || rec_lock(s->accounts, sizeof a, id, LK_SHARED) != ERR_OK) return "?";
    Err e = rec_read(s->accounts, sizeof a, id, &a);
    rec_unlock(s->accounts, sizeof a, id);
    if (e != ERR_OK || a.id == 0) return "?";
    return a.active ? "ACTIVE" : "DEACTIVATED";
}

static int list_customers(int fd, Store *s) {
    struct stat st;
    if (fstat(s->users, &st) == -1) { LOG_SYS("fstat users"); return reply(fd, ERR_IO, 0, err_text(ERR_IO)); }
    int n = (int)(st.st_size / (off_t)sizeof(User)), count = 0;
    for (int id = 1; id < n; id++) {
        User u;
        Err e = user_get(s, id, &u);
        if (e == ERR_NOT_FOUND) continue;
        if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
        if (u.role != ROLE_CUSTOMER) continue;
        char line[MSG_LEN];
        snprintf(line, sizeof line, "user %-3d %-14.31s %-22.47s acct %-4d %-12s %.15s",
                 u.id, u.username, u.name, u.account_id, acct_state(s, u.account_id), u.phone);
        if (reply_more(fd, line) != 0) return -1;
        count++;
    }
    return reply(fd, ERR_OK, count, count ? "-- end of list --" : "No customers.");
}

/* Order: account slot -> user record -> account record -> opening deposit (journaled). */
static int add_customer(int fd, Store *s, const Session *ss, const Request *rq) {
    if (rq->amount < 0 || rq->amount > MAX_AMOUNT)
        return reply(fd, ERR_INVALID, 0, "Invalid opening balance.");
    int acct = rec_alloc(s->accounts, sizeof(Account));
    if (acct < 0) return reply(fd, ERR_IO, 0, err_text(ERR_IO));
    User u;
    Err e = user_create(s, rq->s1, rq->s2, ROLE_CUSTOMER, rq->s1, acct, &u);
    LOG_I("employee id %d: add customer '%.31s' -> result %d", ss->user_id, rq->s1, e);
    if (e != ERR_OK)
        return reply(fd, e, 0, e == ERR_DUPLICATE ? "Username already exists."
                     : e == ERR_INVALID ? "Username: letters, digits, _ . - (1-31 chars). Password: 4-63 chars."
                     : err_text(e));
    Account a = { .balance = 0, .id = acct, .user_id = u.id, .active = 1 };
    e = rec_lock(s->accounts, sizeof a, acct, LK_EXCLUSIVE);
    if (e == ERR_OK) {
        e = rec_write(s->accounts, sizeof a, acct, &a);
        if (e == ERR_OK) e = rec_sync(s->accounts);
        rec_unlock(s->accounts, sizeof a, acct);
    }
    if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
    char msg[MSG_LEN];
    money_t bal = 0;
    if (rq->amount > 0) {
        e = acct_deposit(s, acct, rq->amount, &bal);
        if (e != ERR_OK) {
            LOG_E("opening deposit failed for account %d (%d)", acct, e);
            snprintf(msg, sizeof msg, "Customer created (user %d, account %d) but the opening deposit failed.", u.id, acct);
            return reply(fd, e, 0, msg);
        }
    }
    snprintf(msg, sizeof msg, "Customer created. User id: %d, Account number: %d.", u.id, acct);
    return reply(fd, ERR_OK, acct, msg);
}

static int passbook(int fd, Store *s, const Request *rq) {
    money_t bal;
    Err e = acct_balance(s, rq->id1, &bal);
    if (e != ERR_OK) return reply(fd, e, 0, e == ERR_NOT_FOUND ? "No such account." : err_text(e));
    HistCtx c = { fd, 0, 0 };
    e = txn_scan(s, rq->id1, 100, hist_cb, &c);
    if (c.broken) return -1;
    if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
    char m[40], msg[MSG_LEN];
    fmt_money(m, sizeof m, bal);
    snprintf(msg, sizeof msg, "-- account %d: %d most recent entries, balance %s --", rq->id1, c.count, m);
    return reply(fd, ERR_OK, c.count, msg);
}

int handle_employee(int fd, Store *s, Session *ss, const Request *rq) {
    Err e;
    switch (rq->op) {
    case OP_EMP_ADD_CUSTOMER:   return add_customer(fd, s, ss, rq);
    case OP_EMP_LIST_CUSTOMERS: return list_customers(fd, s);
    case OP_EMP_PASSBOOK:       return passbook(fd, s, rq);
    case OP_EMP_MODIFY_CUSTOMER:
        e = user_update_details(s, rq->id1, rq->s1, rq->s2, ROLE_CUSTOMER);
        LOG_I("employee id %d: modify customer %d -> result %d", ss->user_id, rq->id1, e);
        return reply(fd, e, 0, e == ERR_OK ? "Details updated."
                     : e == ERR_NOT_FOUND ? "No such user."
                     : e == ERR_PERMISSION ? "That user is not a customer."
                     : e == ERR_INVALID ? "Name max 47 chars, phone max 15 chars, no control characters."
                     : err_text(e));
    default:
        return reply(fd, ERR_INVALID, 0, "Not implemented yet.");
    }
}