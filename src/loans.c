#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "loans.h"
#include "auth.h"
#include "ledger.h"
#include "log.h"

/* Loan creation is rare and in-process: a mutex makes "no open loan + allocate" atomic. */
static pthread_mutex_t g_create_mu = PTHREAD_MUTEX_INITIALIZER;

const char *loan_status_str(int st) {
    switch (st) {
    case LOAN_PENDING:  return "PENDING";
    case LOAN_ASSIGNED: return "ASSIGNED";
    case LOAN_APPROVED: return "APPROVED";
    case LOAN_REJECTED: return "REJECTED";
    default:            return "?";
    }
}

static Err loan_store(Store *s, const Loan *l) {          /* caller holds the loan lock */
    Err e = rec_write(s->loans, sizeof *l, l->id, l);
    if (e == ERR_OK) e = rec_sync(s->loans);
    return e;
}

Err loan_scan(Store *s, LoanCb cb, void *ctx) {
    struct stat st;
    if (fstat(s->loans, &st) == -1) { LOG_SYS("fstat loans"); return ERR_IO; }
    int n = (int)(st.st_size / (off_t)sizeof(Loan));
    for (int id = 1; id < n; id++) {
        Loan l;
        Err e = rec_lock(s->loans, sizeof l, id, LK_SHARED);
        if (e != ERR_OK) return e;
        e = rec_read(s->loans, sizeof l, id, &l);
        rec_unlock(s->loans, sizeof l, id);               /* callback runs with NO lock held */
        if (e != ERR_OK) return e;
        if (l.id == 0) continue;
        if (cb(&l, ctx)) break;
    }
    return ERR_OK;
}

typedef struct { int acct; int open; } OpenCtx;
static int open_cb(const Loan *l, void *p) {
    OpenCtx *c = p;
    if (l->account_id == c->acct && (l->status == LOAN_PENDING || l->status == LOAN_ASSIGNED)) {
        c->open = 1;
        return 1;
    }
    return 0;
}

Err loan_apply(Store *s, int acct, money_t amt, int *loan_id) {
    if (amt <= 0 || amt > MAX_AMOUNT) return ERR_INVALID;
    pthread_mutex_lock(&g_create_mu);
    OpenCtx oc = { acct, 0 };
    Err e = loan_scan(s, open_cb, &oc);
    if (e == ERR_OK && oc.open) e = ERR_LIMIT;               /* one open loan per customer */
    if (e == ERR_OK) {                                       /* account must be active, checked under its lock */
        Account a;
        e = rec_lock(s->accounts, sizeof a, acct, LK_SHARED);
        if (e == ERR_OK) {
            e = rec_read(s->accounts, sizeof a, acct, &a);
            rec_unlock(s->accounts, sizeof a, acct);
            if (e == ERR_OK && a.id == 0) e = ERR_NOT_FOUND;
            if (e == ERR_OK && !a.active) e = ERR_INACTIVE;
        }
    }
    if (e == ERR_OK) {
        int id = rec_alloc(s->loans, sizeof(Loan));
        if (id < 0) {
            e = ERR_IO;
        } else {
            Loan l;
            memset(&l, 0, sizeof l);
            l.id = id;
            l.account_id = acct;
            l.amount = amt;
            l.status = LOAN_PENDING;
            l.created = l.updated = (int64_t)time(NULL);
            e = rec_lock(s->loans, sizeof l, id, LK_EXCLUSIVE);
            if (e == ERR_OK) {
                e = loan_store(s, &l);
                rec_unlock(s->loans, sizeof l, id);
            }
            if (e == ERR_OK) *loan_id = id;
        }
    }
    pthread_mutex_unlock(&g_create_mu);
    return e;
}

Err loan_assign(Store *s, int loan_id, int emp_uid) {
    Err e = rec_lock(s->loans, sizeof(Loan), loan_id, LK_EXCLUSIVE);
    if (e != ERR_OK) return e;
    Loan l;
    e = rec_read(s->loans, sizeof l, loan_id, &l);
    if (e == ERR_OK && l.id == 0) e = ERR_NOT_FOUND;
    if (e == ERR_OK && l.status != LOAN_PENDING && l.status != LOAN_ASSIGNED) e = ERR_STATE;
    if (e == ERR_OK) {            /* employee is validated AFTER the loan lock is held (see notes) */
        User u;
        e = user_get(s, emp_uid, &u);
        if (e == ERR_NOT_FOUND) e = ERR_INVALID;
        if (e == ERR_OK && (u.role != ROLE_EMPLOYEE || !u.active)) e = ERR_INVALID;
    }
    if (e == ERR_OK) {
        l.status = LOAN_ASSIGNED;
        l.assigned_to = emp_uid;
        l.updated = (int64_t)time(NULL);
        e = loan_store(s, &l);
    }
    rec_unlock(s->loans, sizeof l, loan_id);
    return e;
}

Err loan_decide(Store *s, int loan_id, int emp_uid, int approve, money_t *new_bal) {
    Err e = rec_lock(s->loans, sizeof(Loan), loan_id, LK_EXCLUSIVE);
    if (e != ERR_OK) return e;
    Loan l;
    e = rec_read(s->loans, sizeof l, loan_id, &l);
    if (e == ERR_OK && l.id == 0) e = ERR_NOT_FOUND;
    if (e == ERR_OK && l.status != LOAN_ASSIGNED) e = ERR_STATE;          /* terminal or not yet assigned */
    if (e == ERR_OK && l.assigned_to != emp_uid) e = ERR_PERMISSION;      /* someone else's loan */
    if (e == ERR_OK) {
        l.updated = (int64_t)time(NULL);
        if (approve) {
            l.status = LOAN_APPROVED;                    /* only becomes real if the journal commits */
            e = ledger_approve_loan(s, &l, new_bal);     /* loan lock -> account lock, one atomic unit */
        } else {
            l.status = LOAN_REJECTED;
            e = loan_store(s, &l);
        }
    }
    rec_unlock(s->loans, sizeof l, loan_id);
    return e;
}

/* mode 0: release ASSIGNED loans of emp_uid. mode 1: release ASSIGNED loans whose
 * assignee is no longer an active Employee. Always loan lock first, user lock second. */
static Err release_loans(Store *s, int mode, int emp_uid) {
    struct stat st;
    if (fstat(s->loans, &st) == -1) { LOG_SYS("fstat loans"); return ERR_IO; }
    int n = (int)(st.st_size / (off_t)sizeof(Loan));
    Err first = ERR_OK;
    for (int id = 1; id < n; id++) {
        Loan l;
        Err e = rec_lock(s->loans, sizeof l, id, LK_EXCLUSIVE);
        if (e != ERR_OK) { if (first == ERR_OK) first = e; continue; }
        e = rec_read(s->loans, sizeof l, id, &l);
        if (e == ERR_OK && l.id != 0 && l.status == LOAN_ASSIGNED) {
            int release;
            if (mode == 0) {
                release = (l.assigned_to == emp_uid);
            } else {
                User u;
                Err ue = user_get(s, l.assigned_to, &u);
                release = (ue == ERR_NOT_FOUND || ue == ERR_INVALID ||
                           (ue == ERR_OK && (u.role != ROLE_EMPLOYEE || !u.active)));
            }
            if (release) {
                l.status = LOAN_PENDING;
                l.assigned_to = 0;
                l.updated = (int64_t)time(NULL);
                e = loan_store(s, &l);
                if (e == ERR_OK) LOG_I("loan %d returned to PENDING (assignee no longer an employee)", id);
            }
        }
        rec_unlock(s->loans, sizeof l, id);
        if (e != ERR_OK && first == ERR_OK) first = e;
    }
    return first;
}

Err loans_unassign_employee(Store *s, int emp_uid) { return release_loans(s, 0, emp_uid); }
Err loans_sweep_orphans(Store *s)                  { return release_loans(s, 1, 0); }