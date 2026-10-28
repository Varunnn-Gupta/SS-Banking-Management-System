// src/repair.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "repair.h"
#include "auth.h"
#include "log.h"

static int nslots(int fd, size_t sz) {
    struct stat st;
    if (fstat(fd, &st) == -1) return -1;
    return (int)(st.st_size / (off_t)sz);
}

static int acct_load(Store *s, int id, Account *a) {            /* 1 = exists */
    return id >= 1 && rec_read(s->accounts, sizeof *a, id, a) == ERR_OK && a->id != 0;
}

Err repair_orphans(Store *s) {
    int nu = nslots(s->users, sizeof(User)), fixed = 0;
    if (nu < 0) return ERR_IO;
    for (int id = 1; id < nu; id++) {
        User u;
        if (user_get(s, id, &u) != ERR_OK || u.role != ROLE_CUSTOMER || u.account_id <= 0) continue;
        Account a;
        Err e = rec_lock(s->accounts, sizeof a, u.account_id, LK_EXCLUSIVE);
        if (e != ERR_OK) return e;
        e = rec_read(s->accounts, sizeof a, u.account_id, &a);
        if (e == ERR_NOT_FOUND || (e == ERR_OK && a.id == 0)) {      /* creation was interrupted */
            memset(&a, 0, sizeof a);
            a.id = u.account_id;
            a.user_id = u.id;
            a.active = 1;
            e = rec_write(s->accounts, sizeof a, u.account_id, &a);
            if (e == ERR_OK) e = rec_sync(s->accounts);
            if (e == ERR_OK) { fixed++; LOG_W("repair: completed account %d of user '%s'", u.account_id, u.username); }
        }
        rec_unlock(s->accounts, sizeof a, u.account_id);
        if (e != ERR_OK) return e;
    }
    LOG_I("repair: %d orphaned customer account(s) completed", fixed);
    return ERR_OK;
}

int check_consistency(Store *s) {
    int na = nslots(s->accounts, sizeof(Account)), nt = nslots(s->txns, sizeof(Transaction));
    int nu = nslots(s->users, sizeof(User)), nl = nslots(s->loans, sizeof(Loan));
    if (na < 1) na = 1;
    if (nt < 1) nt = 1;
    if (nu < 1) nu = 1;
    if (nl < 1) nl = 1;
    money_t *sum = calloc((size_t)na, sizeof *sum);
    if (!sum) return 1;
    int bad = 0, ntx = 0, nac = 0, nus = 0, nln = 0;
    money_t out = 0, in = 0;

    for (int id = 1; id < nt; id++) {                              /* 1. replay the transaction log */
        Transaction t;
        if (rec_read(s->txns, sizeof t, id, &t) != ERR_OK || t.id == 0) continue;
        ntx++;
        if (t.account_id < 1 || t.account_id >= na) {
            printf("  txn %d refers to missing account %d\n", id, t.account_id);
            bad++;
            continue;
        }
        if (t.type == TXN_DEPOSIT || t.type == TXN_LOAN_CREDIT) sum[t.account_id] += t.amount;
        else if (t.type == TXN_TRANSFER_IN)  { sum[t.account_id] += t.amount; in += t.amount; }
        else if (t.type == TXN_WITHDRAW)     sum[t.account_id] -= t.amount;
        else if (t.type == TXN_TRANSFER_OUT) { sum[t.account_id] -= t.amount; out += t.amount; }
        else { printf("  txn %d has unknown type %d\n", id, t.type); bad++; }
    }
    for (int id = 1; id < na; id++) {                              /* 2. balance == log, never negative */
        Account a;
        if (!acct_load(s, id, &a)) continue;
        nac++;
        if (a.id != id) { printf("  account slot %d holds id %d\n", id, a.id); bad++; }
        if (a.balance < 0) { printf("  account %d has a negative balance\n", id); bad++; }
        if (a.balance != sum[id]) {
            printf("  account %d: balance %lld but transaction log says %lld\n", id,
                   (long long)a.balance, (long long)sum[id]);
            bad++;
        }
    }
    if (out != in) {
        printf("  transfers out (%lld) != transfers in (%lld)\n", (long long)out, (long long)in);
        bad++;
    }
    for (int id = 1; id < nu; id++) {                              /* 3. users <-> accounts */
        User u;
        if (user_get(s, id, &u) != ERR_OK) continue;
        nus++;
        if (u.role != ROLE_CUSTOMER) continue;
        Account a;
        if (!acct_load(s, u.account_id, &a)) { printf("  customer '%s' has no account %d\n", u.username, u.account_id); bad++; }
        else if (a.user_id != u.id) { printf("  account %d does not point back to user %d\n", a.id, u.id); bad++; }
    }
    for (int id = 1; id < nl; id++) {                              /* 4. loans */
        Loan l;
        if (rec_read(s->loans, sizeof l, id, &l) != ERR_OK || l.id == 0) continue;
        nln++;
        Account a;
        if (!acct_load(s, l.account_id, &a)) { printf("  loan %d refers to missing account %d\n", id, l.account_id); bad++; }
        if (l.status == LOAN_PENDING && l.assigned_to != 0) { printf("  loan %d is PENDING but assigned\n", id); bad++; }
        if (l.status == LOAN_ASSIGNED) {
            User e;
            if (user_get(s, l.assigned_to, &e) != ERR_OK || e.role != ROLE_EMPLOYEE || !e.active) {
                printf("  loan %d is assigned to a non-employee (user %d)\n", id, l.assigned_to);
                bad++;
            }
        }
    }
    printf("checked %d accounts, %d transactions, %d users, %d loans: %d problem(s)\n", nac, ntx, nus, nln, bad);
    free(sum);
    return bad;
}