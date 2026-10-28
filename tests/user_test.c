#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "auth.h"
#include "log.h"

#define T 8
#define PER 10
static int g_bad;
static Err g_res[T];
static int g_okn[T];
static pthread_barrier_t g_bar;
static Store m;

static void check(const char *name, int cond) {
    printf("[%-44s] %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) g_bad++;
}

static void *w_dup(void *p) {
    int i = (int)(intptr_t)p;
    Store s;
    User u;
    if (store_open(&s) != ERR_OK) { g_res[i] = ERR_IO; return NULL; }
    g_res[i] = user_create(&s, "same", "pass123", ROLE_CUSTOMER, "x", 0, &u);
    store_close(&s);
    return NULL;
}

static void *w_uniq(void *p) {
    int i = (int)(intptr_t)p;
    Store s;
    User u;
    if (store_open(&s) != ERR_OK) return NULL;
    for (int j = 0; j < PER; j++) {
        char nm[NAME_LEN];
        snprintf(nm, sizeof nm, "u%d_%d", i, j);
        if (user_create(&s, nm, "pass123", ROLE_CUSTOMER, nm, 0, &u) == ERR_OK) g_okn[i]++;
    }
    store_close(&s);
    return NULL;
}

static void *w_demote(void *p) {            /* thread 0 demotes user 1, thread 1 demotes user 2 */
    int idx = (int)(intptr_t)p, old = 0;
    Store s;
    int opened = (store_open(&s) == ERR_OK);
    pthread_barrier_wait(&g_bar);           /* both fire at the same instant */
    g_res[idx] = opened ? user_set_role(&s, idx + 1, ROLE_MANAGER, &old) : ERR_IO;
    if (opened) store_close(&s);
    return NULL;
}

static int count_role(int role) {
    struct stat st;
    if (fstat(m.users, &st) == -1) return -1;
    int n = (int)(st.st_size / (off_t)sizeof(User)), c = 0;
    for (int id = 1; id < n; id++) {
        User u;
        if (user_get(&m, id, &u) == ERR_OK && u.role == role) c++;
    }
    return c;
}

int main(void) {
    mkdir("/tmp/banktest4", 0755);
    mkdir("/tmp/banktest4/data", 0755);
    if (chdir("/tmp/banktest4") != 0) { perror("chdir"); return 1; }
    unlink(USERS_FILE);
    if (store_init() != ERR_OK || store_open(&m) != ERR_OK) return 1;

    User u;
    int old = 0;
    user_create(&m, "a1", "pass123", ROLE_ADMIN, "A1", 0, &u);       /* user id 1 */
    user_create(&m, "a2", "pass123", ROLE_ADMIN, "A2", 0, &u);       /* user id 2 */

    pthread_t th[T];
    for (intptr_t i = 0; i < T; i++) pthread_create(&th[i], NULL, w_dup, (void *)i);
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    int ok = 0, dup = 0;
    for (int i = 0; i < T; i++) { ok += (g_res[i] == ERR_OK); dup += (g_res[i] == ERR_DUPLICATE); }
    check("same username x8: exactly 1 created", ok == 1 && dup == T - 1);

    for (intptr_t i = 0; i < T; i++) pthread_create(&th[i], NULL, w_uniq, (void *)i);
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    int total = 0;
    for (int i = 0; i < T; i++) total += g_okn[i];
    check("80 distinct usernames: all created", total == T * PER);
    check("table holds 2 admins + 81 customers", count_role(ROLE_ADMIN) == 2 && count_role(ROLE_CUSTOMER) == 1 + T * PER);

    check("bad username rejected", user_create(&m, "bad name", "pass123", ROLE_CUSTOMER, "x", 0, &u) == ERR_INVALID);
    check("short password rejected", user_create(&m, "okname", "ab", ROLE_CUSTOMER, "x", 0, &u) == ERR_INVALID);
    check("employee cannot edit an admin record", user_update_details(&m, 1, "Hacked", "", ROLE_CUSTOMER) == ERR_PERMISSION);
    User cust;
    user_find_by_name(&m, "same", &cust);
    check("customer cannot be given a staff role", user_set_role(&m, cust.id, ROLE_MANAGER, &old) == ERR_STATE);

    pthread_barrier_init(&g_bar, NULL, 2);
    int bad_rounds = 0;
    for (int r = 0; r < 100; r++) {
        pthread_t d[2];
        for (intptr_t i = 0; i < 2; i++) pthread_create(&d[i], NULL, w_demote, (void *)i);
        for (int i = 0; i < 2; i++) pthread_join(d[i], NULL);
        int okc = (g_res[0] == ERR_OK) + (g_res[1] == ERR_OK);
        int lim = (g_res[0] == ERR_LIMIT) + (g_res[1] == ERR_LIMIT);
        if (okc != 1 || lim != 1 || count_role(ROLE_ADMIN) != 1) bad_rounds++;
        user_set_role(&m, 1, ROLE_ADMIN, &old);                      /* reset for the next round */
        user_set_role(&m, 2, ROLE_ADMIN, &old);
    }
    pthread_barrier_destroy(&g_bar);
    check("last-admin race x100: never zero admins", bad_rounds == 0);

    store_close(&m);
    puts(g_bad ? "RESULT: FAIL" : "RESULT: ALL PASS");
    return g_bad ? 1 : 0;
}