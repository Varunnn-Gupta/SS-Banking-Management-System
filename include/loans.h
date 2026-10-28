#ifndef LOANS_H
#define LOANS_H
#include "storage.h"

typedef int (*LoanCb)(const Loan *l, void *ctx);          /* return nonzero to stop */

const char *loan_status_str(int st);
Err loan_scan(Store *s, LoanCb cb, void *ctx);            /* ascending id, skips empty slots */
Err loan_apply(Store *s, int acct, money_t amt, int *loan_id);
Err loan_assign(Store *s, int loan_id, int emp_uid);
Err loan_decide(Store *s, int loan_id, int emp_uid, int approve, money_t *new_bal);
Err loans_unassign_employee(Store *s, int emp_uid);       /* called when an employee is demoted */
Err loans_sweep_orphans(Store *s);                        /* startup cleanup */
#endif