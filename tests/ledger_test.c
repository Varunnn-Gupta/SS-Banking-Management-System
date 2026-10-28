#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ledger.h"
#include "log.h"

#define THREADS 8
#define OPS     100
#define NACC    4
#define INIT    1000000

static int64_t g_net;                 /* money entering (+) / leaving (-) the bank */
static int g_fail;
static money_t g_sum[NACC + 1];

static void *worker(void *p) {
    unsigned seed = (unsigned)(intptr_t)p * 7919u + 1;
    Store s;
    if (store_open(&s) != ERR_OK) { __sync_fetch_and_add(&g_fail, 1); return NULL; }
    for (int i = 0; i < OPS; i++) {
        int a = 1 + (int)(rand_r(&seed) % NACC);
        int b = 1 + (int)(rand_r(&seed) % NACC);
        int kind = (int)(rand_r(&seed) % 100);
        money_t amt = 1 + (money_t)(rand_r(&seed) % 500);
        money_t after = 0;
        Err e;
        if (kind < 50) {
            e = (a == b) ? ERR_INVALID : acct_transfer(&s, a, b, amt, &after);
        } else if (kind < 75) {
            e = acct_deposit(&s, a, amt, &after);
            if (e == ERR_OK) __sync_fetch_and_add(&g_net, amt);
        } else {
            e = acct_withdraw(&s, a, amt, &after);
            if (e == ERR_OK) __sync_fetch_and_sub(&g_net, amt);
        }
        if (e != ERR_OK && e != ERR_INSUFFICIENT && e != ERR_INVALID) __sync_fetch_and_add(&g_fail, 1);
    }
    store_close(&s);
    return NULL;
}

static int tally(const Transaction *t, void *ctx) {
    (void)ctx;
    if (t->account_id < 1 || t->account_id > NACC) return 0;
    money_t d = (t->type == TXN_DEPOSIT || t->type == TXN_TRANSFER_IN) ? t->amount : -t->amount;
    g_sum[t->account_id] += d;
    return 0;
}

int main(void) {
    mkdir("/tmp/banktest2", 0755);
    mkdir("/tmp/banktest2/data", 0755);
    if (chdir("/tmp/banktest2") != 0) { perror("chdir"); return 1; }
    unlink(ACCOUNTS_FILE); unlink(TXNS_FILE); unlink(JOURNAL_FILE);
    if (store_init() != ERR_OK) return 1;

    Store s;
    if (store_open(&s) != ERR_OK) return 1;
    for (int i = 0; i < NACC; i++) {
        int id = rec_alloc(s.accounts, sizeof(Account));
        Account a = { .balance = INIT, .id = id, .user_id = id, .active = 1 };
        rec_write(s.accounts, sizeof a, id, &a);
    }

    pthread_t t[THREADS];
    for (intptr_t i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);

    int bad = 0;
    money_t total = 0;
    for (int id = 1; id <= NACC; id++) {
        txn_scan(&s, id, INT_MAX, tally, NULL);
        Account a;
        rec_read(s.accounts, sizeof a, id, &a);
        total += a.balance;
        int ok = (a.balance == INIT + g_sum[id]);
        printf("[acct %d ] balance=%lld  initial+txn_log=%lld -> %s\n", id,
               (long long)a.balance, (long long)(INIT + g_sum[id]), ok ? "PASS" : "FAIL");
        if (!ok) bad++;
    }
    int ok = (total == (money_t)NACC * INIT + g_net);
    printf("[total   ] %lld expected %lld (money conserved) -> %s\n",
           (long long)total, (long long)((money_t)NACC * INIT + g_net), ok ? "PASS" : "FAIL");
    if (!ok) bad++;
    if (g_fail) { printf("unexpected errors: %d\n", g_fail); bad++; }
    store_close(&s);
    puts(bad ? "RESULT: FAIL" : "RESULT: ALL PASS");
    return bad ? 1 : 0;
}
