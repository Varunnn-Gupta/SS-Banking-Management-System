#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "handlers.h"
#include "auth.h"
#include "loans.h"
#include "money.h"
#include "log.h"

int is_loan_op(int op) {
    return op == OP_APPLY_LOAN || op == OP_LOAN_STATUS || op == OP_MGR_LIST_LOANS ||
           op == OP_MGR_ASSIGN_LOAN || op == OP_EMP_LIST_LOANS || op == OP_EMP_DECIDE_LOAN;
}

typedef struct { int fd; Store *s; int acct; int emp; int count; int broken; } ListCtx;

static int emit(ListCtx *c, const char *line) {
    if (reply_more(c->fd, line) != 0) { c->broken = 1; return 1; }
    c->count++;
    return 0;
}

static const char *acct_state(Store *s, int id) {
    Account a;
    if (rec_lock(s->accounts, sizeof a, id, LK_SHARED) != ERR_OK) return "?";
    Err e = rec_read(s->accounts, sizeof a, id, &a);
    rec_unlock(s->accounts, sizeof a, id);
    if (e != ERR_OK || a.id == 0) return "?";
    return a.active ? "ACTIVE" : "DEACTIVATED";
}

static void emp_name(Store *s, int uid, char out[NAME_LEN]) {
    User u;
    if (uid > 0 && user_get(s, uid, &u) == ERR_OK) snprintf(out, NAME_LEN, "%.31s", u.username);
    else snprintf(out, NAME_LEN, "-");
}

static int cust_cb(const Loan *l, void *p) {              /* customer: own loans */
    ListCtx *c = p;
    if (l->account_id != c->acct) return 0;
    char amt[40], when[24], line[MSG_LEN];
    struct tm tm;
    time_t ts = (time_t)l->created;
    localtime_r(&ts, &tm);
    strftime(when, sizeof when, "%d-%m-%Y %H:%M", &tm);
    fmt_money(amt, sizeof amt, l->amount);
    snprintf(line, sizeof line, "Loan #%-4d %-16s %-9s applied %s", l->id, amt, loan_status_str(l->status), when);
    return emit(c, line);
}

static int mgr_cb(const Loan *l, void *p) {               /* manager: open loans */
    ListCtx *c = p;
    if (l->status != LOAN_PENDING && l->status != LOAN_ASSIGNED) return 0;
    char amt[40], en[NAME_LEN], line[MSG_LEN];
    fmt_money(amt, sizeof amt, l->amount);
    emp_name(c->s, l->assigned_to, en);
    snprintf(line, sizeof line, "loan %-4d acct %-4d %-14s %-9s emp: %-10s acct is %s", l->id, l->account_id,
             amt, loan_status_str(l->status), en, acct_state(c->s, l->account_id));
    return emit(c, line);
}

static int emp_cb(const Loan *l, void *p) {               /* employee: loans assigned to me */
    ListCtx *c = p;
    if (l->assigned_to != c->emp) return 0;
    char amt[40], line[MSG_LEN];
    fmt_money(amt, sizeof amt, l->amount);
    snprintf(line, sizeof line, "loan %-4d acct %-4d %-14s %-9s acct is %s", l->id, l->account_id,
             amt, loan_status_str(l->status), acct_state(c->s, l->account_id));
    return emit(c, line);
}

static int list_loans(int fd, Store *s, LoanCb cb, ListCtx *c, const char *empty) {
    Err e = loan_scan(s, cb, c);
    if (c->broken) return -1;
    if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
    char msg[MSG_LEN];
    if (c->count) snprintf(msg, sizeof msg, "-- %d loan(s) --", c->count);
    else          snprintf(msg, sizeof msg, "%s", empty);
    return reply(fd, ERR_OK, c->count, msg);
}

static const char *loan_err(Err e) {
    switch (e) {
    case ERR_NOT_FOUND:  return "No such loan.";
    case ERR_STATE:      return "This loan is not awaiting that action (already decided, or not assigned).";
    case ERR_PERMISSION: return "This loan is not assigned to you.";
    case ERR_INACTIVE:   return "The customer's account is deactivated. The loan stays pending: reject it, or retry after reactivation.";
    case ERR_LIMIT:      return "You already have an open loan application.";
    case ERR_INVALID:    return "Invalid input (check the amount or ids).";
    default:             return err_text(e);
    }
}

int handle_loans(int fd, Store *s, Session *ss, const Request *rq) {
    char msg[MSG_LEN], m[40];
    Err e;
    switch (rq->op) {
    case OP_APPLY_LOAN: {
        int lid = 0;
        e = loan_apply(s, ss->account_id, rq->amount, &lid);
        LOG_I("customer id %d: apply loan amount=%lld -> result %d (loan %d)",
              ss->user_id, (long long)rq->amount, e, lid);
        if (e != ERR_OK)
            return reply(fd, e, 0, e == ERR_INACTIVE ? "Your account is deactivated; please contact the bank." : loan_err(e));
        fmt_money(m, sizeof m, rq->amount);
        snprintf(msg, sizeof msg, "Loan application #%d for %s submitted. Status: PENDING.", lid, m);
        return reply(fd, ERR_OK, lid, msg);
    }
    case OP_LOAN_STATUS: {
        ListCtx c = { fd, s, ss->account_id, 0, 0, 0 };
        return list_loans(fd, s, cust_cb, &c, "You have no loan applications.");
    }
    case OP_MGR_LIST_LOANS: {
        ListCtx c = { fd, s, 0, 0, 0, 0 };
        return list_loans(fd, s, mgr_cb, &c, "No open loan applications.");
    }
    case OP_EMP_LIST_LOANS: {
        ListCtx c = { fd, s, 0, ss->user_id, 0, 0 };
        return list_loans(fd, s, emp_cb, &c, "No loans are assigned to you.");
    }
    case OP_MGR_ASSIGN_LOAN:
        e = loan_assign(s, rq->id1, rq->id2);
        LOG_I("manager id %d: assign loan %d to employee %d -> result %d", ss->user_id, rq->id1, rq->id2, e);
        if (e != ERR_OK)
            return reply(fd, e, 0, e == ERR_INVALID ? "That user is not an active employee."
                                 : e == ERR_STATE ? "That loan is already decided." : loan_err(e));
        snprintf(msg, sizeof msg, "Loan %d assigned to employee %d.", rq->id1, rq->id2);
        return reply(fd, ERR_OK, 0, msg);

    case OP_EMP_DECIDE_LOAN: {
        if (rq->id2 != 0 && rq->id2 != 1) return reply(fd, ERR_INVALID, 0, "Enter 1 to approve or 0 to reject.");
        money_t bal = 0;
        e = loan_decide(s, rq->id1, ss->user_id, rq->id2, &bal);
        LOG_I("employee id %d: %s loan %d -> result %d", ss->user_id, rq->id2 ? "approve" : "reject", rq->id1, e);
        if (e != ERR_OK) return reply(fd, e, 0, loan_err(e));
        if (rq->id2) {
            fmt_money(m, sizeof m, bal);
            snprintf(msg, sizeof msg, "Loan %d APPROVED. Customer credited; their new balance is %s.", rq->id1, m);
        } else {
            snprintf(msg, sizeof msg, "Loan %d REJECTED.", rq->id1);
        }
        return reply(fd, ERR_OK, 0, msg);
    }
    default:
        return reply(fd, ERR_INVALID, 0, "Not implemented yet.");
    }
}