#ifndef LEDGER_H
#define LEDGER_H
#include "storage.h"

Err acct_balance(Store *s, int acct, money_t *out);
Err acct_deposit(Store *s, int acct, money_t amt, money_t *after);
Err acct_withdraw(Store *s, int acct, money_t amt, money_t *after);
Err acct_transfer(Store *s, int from, int to, money_t amt, money_t *from_after);

/* Newest first, at most max matches. cb returns nonzero to stop early. */
typedef int (*TxnCb)(const Transaction *t, void *ctx);
Err txn_scan(Store *s, int acct, int max, TxnCb cb, void *ctx);
Err acct_set_active(Store *s, int acct, int active, int *changed);
Err ledger_approve_loan(Store *s, const Loan *loan, money_t *after);
#endif
