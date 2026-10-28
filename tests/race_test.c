#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "storage.h"
#include "log.h"

#define THREADS 8
#define ITERS   2000
#define ALLOC_N 500

static int use_lock = 1;
static int g_fail;
static int g_ids[THREADS][ALLOC_N];

static void *w_deposit(void *p) {
    (void)p;
    Store s;
    if (store_open(&s) != ERR_OK) { __sync_fetch_and_add(&g_fail, 1); return NULL; }
    for (int i = 0; i < ITERS; i++) {
        Account a;
        if (use_lock) rec_lock(s.accounts, sizeof a, 1, LK_EXCLUSIVE);
        if (rec_read(s.accounts, sizeof a, 1, &a) == ERR_OK) {
            a.balance += 1;
            rec_write(s.accounts, sizeof a, 1, &a);
        } else __sync_fetch_and_add(&g_fail, 1);
        if (use_lock) rec_unlock(s.accounts, sizeof a, 1);
    }
    store_close(&s);
    return NULL;
}

static void *w_transfer(void *p) {
    int tid = (int)(intptr_t)p;
    int from = (tid % 2) ? 2 : 3, to = (tid % 2) ? 3 : 2;    /* opposite directions */
    Store s;
    if (store_open(&s) != ERR_OK) { __sync_fetch_and_add(&g_fail, 1); return NULL; }
    for (int i = 0; i < ITERS; i++) {
        money_t amt = (i % 7) + 1;
        Account f, t;
        rec_lock_pair(s.accounts, sizeof f, from, to);
        if (rec_read(s.accounts, sizeof f, from, &f) != ERR_OK ||
            rec_read(s.accounts, sizeof t, to, &t) != ERR_OK) {
            __sync_fetch_and_add(&g_fail, 1);
        } else {
            f.balance -= amt; t.balance += amt;
            rec_write(s.accounts, sizeof f, from, &f);
            rec_write(s.accounts, sizeof t, to, &t);
        }
        rec_unlock_pair(s.accounts, sizeof f, from, to);
    }
    store_close(&s);
    return NULL;
}

static void *w_alloc(void *p) {
    int tid = (int)(intptr_t)p;
    Store s;
    if (store_open(&s) != ERR_OK) { __sync_fetch_and_add(&g_fail, 1); return NULL; }
    for (int i = 0; i < ALLOC_N; i++) g_ids[tid][i] = rec_alloc(s.loans, sizeof(Loan));
    store_close(&s);
    return NULL;
}

static int run(void *(*fn)(void *)) {
    pthread_t t[THREADS];
    for (intptr_t i = 0; i < THREADS; i++)
        if (pthread_create(&t[i], NULL, fn, (void *)i)) { perror("pthread_create"); return -1; }
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    return 0;
}

static void seed(void) {
    Store s;
    store_open(&s);
    money_t init[3] = {0, 1000000, 1000000};
    for (int i = 0; i < 3; i++) {
        int id = rec_alloc(s.accounts, sizeof(Account));
        Account a = { .balance = init[i], .id = id, .user_id = id, .active = 1 };
        rec_write(s.accounts, sizeof a, id, &a);
    }
    store_close(&s);
}

static money_t bal(int id) {
    Store s; Account a = {0};
    store_open(&s);
    rec_read(s.accounts, sizeof a, id, &a);
    store_close(&s);
    return a.balance;
}

int main(int argc, char **argv) {
    use_lock = !(argc > 1 && !strcmp(argv[1], "nolock"));
    mkdir("/tmp/banktest", 0755);
    mkdir("/tmp/banktest/data", 0755);
    if (chdir("/tmp/banktest") != 0) { perror("chdir"); return 1; }
    unlink(ACCOUNTS_FILE); unlink(LOANS_FILE);
    if (store_init() != ERR_OK) return 1;
    seed();
    int bad = 0;

    /* 1. lost-update test: 8 threads x 2000 deposits of 1 paisa */
    run(w_deposit);
    money_t got = bal(1), want = (money_t)THREADS * ITERS;
    printf("[deposit ] %s: expected %lld got %lld -> %s\n", use_lock ? "locked" : "NO LOCK",
           (long long)want, (long long)got, got == want ? "PASS" : "FAIL (lost updates)");
    if (!use_lock) { puts("(nolock mode: FAIL above is the point of the demo)"); return 0; }
    if (got != want) bad++;

    /* 2. opposite-direction transfers: must not deadlock, must not lose money */
    run(w_transfer);
    money_t b2 = bal(2), b3 = bal(3);
    int ok = (b2 == 1000000 && b3 == 1000000);
    printf("[transfer] 2<->3 both directions: acct2=%lld acct3=%lld -> %s\n",
           (long long)b2, (long long)b3, ok ? "PASS" : "FAIL");
    if (!ok) bad++;

    /* 3. concurrent ID allocation: every id unique and in range */
    run(w_alloc);
    static char seen[THREADS * ALLOC_N + 1];
    int dup = 0;
    for (int t = 0; t < THREADS; t++)
        for (int i = 0; i < ALLOC_N; i++) {
            int id = g_ids[t][i];
            if (id < 1 || id > THREADS * ALLOC_N || seen[id]++) dup++;
        }
    printf("[alloc   ] %d ids, bad/duplicate: %d -> %s\n", THREADS * ALLOC_N, dup, dup ? "FAIL" : "PASS");
    if (dup) bad++;

    if (g_fail) { printf("internal failures: %d\n", g_fail); bad++; }
    puts(bad ? "RESULT: FAIL" : "RESULT: ALL PASS");
    return bad ? 1 : 0;
}
