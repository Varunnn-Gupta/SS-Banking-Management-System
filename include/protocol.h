#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include "models.h"

#define SERVER_PORT 5050
#define MAX_CLIENTS 100             /* counting semaphore initial value */
#define ARG_LEN     200
#define MSG_LEN     256

typedef enum {
    /* common */
    OP_LOGIN = 1,        /* s1=user s2=pass; reply value=role */
    OP_LOGOUT,
    OP_EXIT,
    OP_CHANGE_PWD,       /* s1=old s2=new */
    /* customer */
    OP_BALANCE = 20,     /* reply value=balance */
    OP_DEPOSIT,          /* amount */
    OP_WITHDRAW,         /* amount */
    OP_TRANSFER,         /* id1=dest account, amount */
    OP_APPLY_LOAN,       /* amount */
    OP_FEEDBACK,         /* s1=text */
    OP_HISTORY,          /* streams */
    OP_LOAN_STATUS,      /* streams */    
    /* employee */
    OP_EMP_ADD_CUSTOMER = 40, /* s1=username s2=password amount=opening balance */
    OP_EMP_MODIFY_CUSTOMER,   /* id1=user id s1=name s2=phone */
    OP_EMP_LIST_LOANS,        /* my assigned loans, streams */
    OP_EMP_DECIDE_LOAN,       /* id1=loan id id2=1 approve / 0 reject */
    OP_EMP_PASSBOOK,          /* id1=account id, streams */
    OP_EMP_LIST_CUSTOMERS,    /* streams */    
    /* manager */
    OP_MGR_SET_ACTIVE = 60,   /* id1=account id id2=1/0 */
    OP_MGR_LIST_LOANS,        /* pending + assigned, streams */
    OP_MGR_ASSIGN_LOAN,       /* id1=loan id id2=employee user id */
    OP_MGR_LIST_FEEDBACK,     /* streams */
    OP_MGR_LIST_ACCOUNTS,     /* streams */
    /* admin */
    OP_ADM_ADD_EMPLOYEE = 80, /* s1=username s2=password */
    OP_ADM_MODIFY_USER,       /* id1=user id s1=name s2=phone */
    OP_ADM_SET_ROLE,           /* id1=user id id2=Role */
    OP_ADM_LIST_USERS         /* streams */
} Op;

typedef struct {
    int64_t  amount;
    int32_t  id1;
    int32_t  id2;
    uint16_t op;
    uint16_t _pad[3];
    char     s1[ARG_LEN];
    char     s2[ARG_LEN];
} Request;

typedef struct {
    int64_t value;
    int32_t status;                 /* Err */
    int32_t more;                   /* 1 = more frames follow */
    char    msg[MSG_LEN];
} Response;

/* never trust the peer: force NUL termination after every receive */
static inline void req_sanitize(Request *r) {
    r->s1[ARG_LEN - 1] = '\0';
    r->s2[ARG_LEN - 1] = '\0';
}

/* send/recv exactly len bytes (handles partial transfers and EINTR) */
static inline int net_send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);   /* no SIGPIPE */
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

static inline int net_recv_all(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) return -1;                        /* peer closed */
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

#endif
