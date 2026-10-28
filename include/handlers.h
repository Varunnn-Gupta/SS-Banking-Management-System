#ifndef HANDLERS_H
#define HANDLERS_H
#include "protocol.h"
#include "storage.h"
#include "session.h"

int reply(int fd, Err st, int64_t val, const char *msg);   /* final frame (more=0) */
int reply_more(int fd, const char *msg);                    /* streamed frame (more=1) */
const char *err_text(Err e);                                /* safe, short client messages */
int handle_customer(int fd, Store *s, Session *ss, const Request *rq);
int handle_manager(int fd, Store *s, Session *ss, const Request *rq);
int handle_admin(int fd, Store *s, Session *ss, const Request *rq);
int is_loan_op(int op);
int handle_loans(int fd, Store *s, Session *ss, const Request *rq);
typedef struct { int fd; int count; int broken; } HistCtx;
int hist_cb(const Transaction *t, void *p);      /* shared by customer history and employee passbook */
int handle_employee(int fd, Store *s, Session *ss, const Request *rq);
#endif
