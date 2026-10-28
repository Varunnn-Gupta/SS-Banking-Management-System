#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "ledger.h"
#include "journal.h"
#include "log.h"

static Err load_account(Store *s, int id, Account *a) {
    Err e = rec_read(s->accounts, sizeof *a, id, a);
    if (e == ERR_OK && a->id == 0) e = ERR_NOT_FOUND;
    return e;
}

static Err make_txn(Store *s, Transaction *t, int acct, int peer, TxnType type,
                    money_t amt, money_t after) {
    int id = rec_alloc(s->txns, sizeof *t);
    if (id < 0) return ERR_IO;
    memset(t, 0, sizeof *t);
    t->id = id;
    t->account_id = acct;
    t->peer_account = peer;
    t->type = (uint8_t)type;
    t->amount = amt;
    t->balance_after = after;
    t->ts = (int64_t)time(NULL);
    return ERR_OK;
}

/* account(s) + txn record(s) become durable together, or not at all */
static Err commit_ops(Store *s, const Account *a, const Transaction *t,
                      const Account *b, const Transaction *u) {
    Jrnl j;
    Err e = jrnl_begin(s, &j);
    if (e != ERR_OK) return e;
    e = jrnl_add(&j, JK_ACCOUNT, a->id, a, sizeof *a);
    if (e == ERR_OK) e = jrnl_add(&j, JK_TXN, t->id, t, sizeof *t);
    if (e == ERR_OK && b) e = jrnl_add(&j, JK_ACCOUNT, b->id, b, sizeof *b);
    if (e == ERR_OK && u) e = jrnl_add(&j, JK_TXN, u->id, u, sizeof *u);
    return e == ERR_OK ? jrnl_commit(s, &j) : e;
}

Err acct_balance(Store *s, int acct, money_t *out) {
    Err e = rec_lock(s->accounts, sizeof(Account), acct, LK_SHARED);
    if (e != ERR_OK) return e;
    Account a;
    e = load_account(s, acct, &a);
    rec_unlock(s->accounts, sizeof a, acct);
    if (e == ERR_OK) *out = a.balance;
    return e;
}

static Err single(Store *s, int acct, money_t amt, int is_deposit, money_t *after) {
    if (amt <= 0 || amt > MAX_AMOUNT) return ERR_INVALID;
    Err e = rec_lock(s->accounts, sizeof(Account), acct, LK_EXCLUSIVE);
    if (e != ERR_OK) return e;
    Account a;
    Transaction t;
    e = load_account(s, acct, &a);
    if (e == ERR_OK && !a.active) e = ERR_INACTIVE;
    if (e == ERR_OK) {
        if (is_deposit) {
            if (a.balance > INT64_MAX - amt) e = ERR_INVALID; else a.balance += amt;
        } else {
            if (a.balance < amt) e = ERR_INSUFFICIENT; else a.balance -= amt;
        }
    }
    if (e == ERR_OK) e = make_txn(s, &t, acct, 0, is_deposit ? TXN_DEPOSIT : TXN_WITHDRAW, amt, a.balance);
    if (e == ERR_OK) e = commit_ops(s, &a, &t, NULL, NULL);
    rec_unlock(s->accounts, sizeof a, acct);
    if (e == ERR_OK) *after = a.balance;
    return e;
}

Err acct_deposit(Store *s, int acct, money_t amt, money_t *after)  { return single(s, acct, amt, 1, after); }
Err acct_withdraw(Store *s, int acct, money_t amt, money_t *after) { return single(s, acct, amt, 0, after); }

Err acct_transfer(Store *s, int from, int to, money_t amt, money_t *from_after) {
    if (amt <= 0 || amt > MAX_AMOUNT || from == to) return ERR_INVALID;
    Err e = rec_lock_pair(s->accounts, sizeof(Account), from, to);   /* ascending id order */
    if (e != ERR_OK) return e;
    Account a, b;
    Transaction t1, t2;
    e = load_account(s, from, &a);
    if (e == ERR_OK) e = load_account(s, to, &b);
    if (e == ERR_OK && (!a.active || !b.active)) e = ERR_INACTIVE;
    if (e == ERR_OK && a.balance < amt) e = ERR_INSUFFICIENT;
    if (e == ERR_OK && b.balance > INT64_MAX - amt) e = ERR_INVALID;
    if (e == ERR_OK) {
        a.balance -= amt;
        b.balance += amt;
        e = make_txn(s, &t1, from, to, TXN_TRANSFER_OUT, amt, a.balance);
        if (e == ERR_OK) e = make_txn(s, &t2, to, from, TXN_TRANSFER_IN, amt, b.balance);
        if (e == ERR_OK) e = commit_ops(s, &a, &t1, &b, &t2);
    }
    rec_unlock_pair(s->accounts, sizeof(Account), from, to);
    if (e == ERR_OK) *from_after = a.balance;
    return e;
}

Err txn_scan(Store *s, int acct, int max, TxnCb cb, void *ctx) {
    struct stat st;
    if (fstat(s->txns, &st) == -1) { LOG_SYS("fstat txns"); return ERR_IO; }
    int n = (int)(st.st_size / (off_t)sizeof(Transaction));
    int found = 0;
    for (int id = n - 1; id >= 1 && found < max; id--) {
        Transaction t;
        Err e = rec_lock(s->txns, sizeof t, id, LK_SHARED);
        if (e != ERR_OK) return e;
        e = rec_read(s->txns, sizeof t, id, &t);
        rec_unlock(s->txns, sizeof t, id);
        if (e != ERR_OK) return e;
        if (t.id == 0 || t.account_id != acct) continue;   /* empty slot or other account */
        found++;
        if (cb(&t, ctx)) break;
    }
    return ERR_OK;
}

Err acct_set_active(Store *s, int acct, int active, int *changed) {
    *changed = 0;
    Err e = rec_lock(s->accounts, sizeof(Account), acct, LK_EXCLUSIVE);
    if (e != ERR_OK) return e;
    Account a;
    e = load_account(s, acct, &a);
    if (e == ERR_OK && a.active != (uint8_t)active) {
        a.active = (uint8_t)active;
        e = rec_write(s->accounts, sizeof a, acct, &a);
        if (e == ERR_OK) e = rec_sync(s->accounts);
        if (e == ERR_OK) *changed = 1;
    }
    rec_unlock(s->accounts, sizeof a, acct);
    return e;
}

Err ledger_approve_loan(Store *s, const Loan *loan, money_t *after) {
    Err e = rec_lock(s->accounts, sizeof(Account), loan->account_id, LK_EXCLUSIVE);   /* loan lock is held by the caller: order is loan -> account */
    if (e != ERR_OK) return e;
    Account a;
    Transaction t;
    e = load_account(s, loan->account_id, &a);
    if (e == ERR_OK && !a.active) e = ERR_INACTIVE;
    if (e == ERR_OK && a.balance > INT64_MAX - loan->amount) e = ERR_INVALID;
    if (e == ERR_OK) {
        a.balance += loan->amount;
        e = make_txn(s, &t, a.id, 0, TXN_LOAN_CREDIT, loan->amount, a.balance);
    }
    if (e == ERR_OK) {
        Jrnl j;
        e = jrnl_begin(s, &j);
        if (e == ERR_OK) e = jrnl_add(&j, JK_ACCOUNT, a.id, &a, sizeof a);
        if (e == ERR_OK) e = jrnl_add(&j, JK_TXN, t.id, &t, sizeof t);
        if (e == ERR_OK) e = jrnl_add(&j, JK_LOAN, loan->id, loan, sizeof *loan);
        if (e == ERR_OK) e = jrnl_commit(s, &j);
    }
    rec_unlock(s->accounts, sizeof(Account), loan->account_id);
    if (e == ERR_OK) *after = a.balance;
    return e;
}
