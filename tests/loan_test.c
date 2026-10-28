#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "auth.h"
#include "ledger.h"
#include "loans.h"
#include "log.h"

#define T 8
static Store m;                       /* main thread's handle, for setup and inspection */
static int g_bad;
static int g_cnt[16];                /* results of the last race, indexed by Err */

typedef struct { int op; int a; int b; Err res; money_t bal; } Job;

static void *worker(void *p) {
    Job *j = p;
    Store s;
    if (store_open(&s) != ERR_OK) { j->res = ERR_IO; return NULL; }
    int id = 0;
    if (j->op == 0) j->res = loan_apply(&s, j->a, 500, &id);
    else            j->res = loan_decide(&s, j->a, j->b, 1, &j->bal);
    store_close(&s);
    return NULL;
}

static void race(int op, int a, int alt_emp) {
    Job jobs[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        jobs[i] = (Job){ op, a, alt_emp ? 1 + (i % 2) : 0, ERR_OK, 0 };
        pthread_create(&th[i], NULL, worker, &jobs[i]);
    }
    memset(g_cnt, 0, sizeof g_cnt);
    for (int i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
        int r = (int)jobs[i].res;
        if (r >= 0 && r < 16) g_cnt[r]++;
    }
}

static void check(const char *name, int cond) {
    printf("[%-42s] %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) g_bad++;
}
static Loan getloan(int id) { Loan l; memset(&l, 0, sizeof l); rec_read(m.loans, sizeof l, id, &l); return l; }
static money_t getbal(int id) { Account a; memset(&a, 0, sizeof a); rec_read(m.accounts, sizeof a, id, &a); return a.balance; }
static int cb_count(const Transaction *t, void *p) { if (t->type == TXN_LOAN_CREDIT) (*(int *)p)++; return 0; }

int main(void) {
    mkdir("/tmp/banktest3", 0755);
    mkdir("/tmp/banktest3/data", 0755);
    if (chdir("/tmp/banktest3") != 0) { perror("chdir"); return 1; }
    unlink(USERS_FILE); unlink(ACCOUNTS_FILE); unlink(TXNS_FILE); unlink(LOANS_FILE); unlink(JOURNAL_FILE);
    if (store_init() != ERR_OK || store_open(&m) != ERR_OK) return 1;

    User u;
    user_create(&m, "e1", "pass123", ROLE_EMPLOYEE, "E1", 0, &u);    /* user id 1 */
    user_create(&m, "e2", "pass123", ROLE_EMPLOYEE, "E2", 0, &u);    /* user id 2 */
    user_create(&m, "mg", "pass123", ROLE_MANAGER, "M", 0, &u);      /* user id 3 */
    for (int i = 0; i < 3; i++) {
        int id = rec_alloc(m.accounts, sizeof(Account));
        Account a = { .balance = 0, .id = id, .user_id = 100 + i, .active = 1 };
        rec_write(m.accounts, sizeof a, id, &a);
    }

    int lid = 0, ch = 0, old = 0;
    money_t b = 0;

    race(0, 2, 0);                                                   /* 8 applications, one account */
    check("apply race: exactly 1 accepted", g_cnt[ERR_OK] == 1);
    check("apply race: others get ERR_LIMIT", g_cnt[ERR_LIMIT] == T - 1);

    loan_apply(&m, 1, 10000, &lid);
    loan_assign(&m, lid, 1);
    race(1, lid, 1);                                                 /* 8 decisions on one loan */
    int credits = 0;
    txn_scan(&m, 1, 1000, cb_count, &credits);
    check("decide race: exactly 1 approval", g_cnt[ERR_OK] == 1);
    check("decide race: credited exactly once", getbal(1) == 10000 && credits == 1);
    check("decide race: loan is APPROVED", getloan(lid).status == LOAN_APPROVED);
    check("approved loan cannot be reassigned", loan_assign(&m, lid, 2) == ERR_STATE);
    check("approved loan cannot be decided twice", loan_decide(&m, lid, 1, 1, &b) == ERR_STATE);

    acct_set_active(&m, 3, 0, &ch);
    check("inactive account cannot apply", loan_apply(&m, 3, 700, &lid) == ERR_INACTIVE);
    acct_set_active(&m, 3, 1, &ch);
    loan_apply(&m, 3, 700, &lid);
    loan_assign(&m, lid, 1);
    acct_set_active(&m, 3, 0, &ch);
    check("approve refused while account inactive", loan_decide(&m, lid, 1, 1, &b) == ERR_INACTIVE);
    check("loan stays ASSIGNED, no money moved", getloan(lid).status == LOAN_ASSIGNED && getbal(3) == 0);
    check("reject allowed while account inactive",
          loan_decide(&m, lid, 1, 0, &b) == ERR_OK && getloan(lid).status == LOAN_REJECTED);
    acct_set_active(&m, 3, 1, &ch);
    loan_apply(&m, 3, 900, &lid);                                    /* reapply after a rejection works */
    loan_assign(&m, lid, 1);
    check("approve works after reactivation",
          loan_decide(&m, lid, 1, 1, &b) == ERR_OK && b == 900 && getbal(3) == 900);

    loan_apply(&m, 3, 400, &lid);
    loan_assign(&m, lid, 2);
    loans_unassign_employee(&m, 2);
    check("employee removal returns loan to PENDING",
          getloan(lid).status == LOAN_PENDING && getloan(lid).assigned_to == 0);
    check("cannot assign to a manager", loan_assign(&m, lid, 3) == ERR_INVALID);
    check("cannot assign to unknown user", loan_assign(&m, lid, 999) == ERR_INVALID);
    loan_assign(&m, lid, 2);
    user_set_role(&m, 2, ROLE_MANAGER, &old);                        /* simulate crash after role write */
    loans_sweep_orphans(&m);
    check("startup sweep releases orphaned loan", getloan(lid).status == LOAN_PENDING);

    store_close(&m);
    puts(g_bad ? "RESULT: FAIL" : "RESULT: ALL PASS");
    return g_bad ? 1 : 0;
}