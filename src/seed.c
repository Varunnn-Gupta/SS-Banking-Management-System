// src/seed.c
#include <stdio.h>
#include <sys/stat.h>
#include "seed.h"
#include "storage.h"
#include "auth.h"
#include "log.h"
#include "ledger.h"

#define SEED_PASSWORD "pass123"

Err seed_data(void) {
    Store s;
    if (store_open(&s) != ERR_OK) return ERR_IO;
    struct stat st;
    if (fstat(s.users, &st) == 0 && st.st_size > 0) {
        puts("Data already exists. Delete the data/ folder to re-seed.");
        store_close(&s);
        return ERR_DUPLICATE;
    }
    static const struct { const char *user, *name; Role role; money_t bal; } seed[] = {
        { "admin1", "Admin One",    ROLE_ADMIN,    0 },
        { "mgr1",   "Manager One",  ROLE_MANAGER,  0 },
        { "emp1",   "Employee One", ROLE_EMPLOYEE, 0 },
        { "emp2",   "Employee Two", ROLE_EMPLOYEE, 0 },
        { "cust1",  "Customer One",   ROLE_CUSTOMER, 500000 },   /* Rs 5000.00  */
        { "cust2",  "Customer Two",   ROLE_CUSTOMER, 200000 },   /* Rs 2000.00  */
        { "cust3",  "Customer Three", ROLE_CUSTOMER, 1000000 },  /* Rs 10000.00 */
        { "cust4",  "Customer Four",  ROLE_CUSTOMER, 50000 },    /* Rs 500.00   */
    };
    Err result = ERR_OK;
    for (size_t i = 0; i < sizeof seed / sizeof seed[0]; i++) {
        int acct = 0;
        if (seed[i].role == ROLE_CUSTOMER) {
            acct = rec_alloc(s.accounts, sizeof(Account));
            if (acct < 0) { result = ERR_IO; break; }
        }
        User u;
        Err e = user_create(&s, seed[i].user, SEED_PASSWORD, seed[i].role,
                            seed[i].name, acct, &u);
        if (e != ERR_OK) { LOG_E("seed user %s failed (%d)", seed[i].user, e); result = e; break; }
        if (acct > 0) {
            Account a = { .balance = 0, .id = acct, .user_id = u.id, .active = 1 };
            rec_write(s.accounts, sizeof a, acct, &a);
            rec_sync(s.accounts);
            money_t after;       /* opening balance goes through the ledger, so the passbook shows it */
            if (seed[i].bal > 0) acct_deposit(&s, acct, seed[i].bal, &after);
        }
        printf("created %-7s role=%d account=%d\n", seed[i].user, seed[i].role, acct);
    }
    store_close(&s);
    if (result == ERR_OK) puts("Seed done. Every user's password is: " SEED_PASSWORD);
    return result;
}
