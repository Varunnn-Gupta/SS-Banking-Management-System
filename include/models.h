#ifndef MODELS_H
#define MODELS_H
#include <stdint.h>
#include <sys/types.h>

/* ---- data files (record N lives at offset N * sizeof(record); slot 0 is
 *      reserved as the allocation lock slot, never holds real data) ---- */
#define USERS_FILE    "data/users.bin"
#define ACCOUNTS_FILE "data/accounts.bin"
#define TXNS_FILE     "data/transactions.bin"
#define LOANS_FILE    "data/loans.bin"
#define FEEDBACK_FILE "data/feedback.bin"
#define JOURNAL_FILE  "data/journal.bin"
#define SESSION_FILE  "data/sessions.lock"   /* byte N = session of user N */

#define REC_OFF(type, id) ((off_t)(id) * (off_t)sizeof(type))

#define NAME_LEN     32
#define DISP_LEN     48
#define PHONE_LEN    16
#define HASH_LEN     128
#define FEEDBACK_LEN 200
#define ARG_LEN_MAX  64
#define MAX_AMOUNT ((money_t)100000000000LL)   /* Rs 1,000,000,000.00 per operation */

typedef int64_t money_t;            /* paise: Rs 123.45 is stored as 12345 */

typedef enum { ROLE_CUSTOMER = 1, ROLE_EMPLOYEE, ROLE_MANAGER, ROLE_ADMIN } Role;

typedef enum { LOAN_PENDING = 0, LOAN_ASSIGNED, LOAN_APPROVED, LOAN_REJECTED } LoanStatus;

typedef enum { TXN_DEPOSIT = 1, TXN_WITHDRAW, TXN_TRANSFER_OUT,
               TXN_TRANSFER_IN, TXN_LOAN_CREDIT } TxnType;

typedef enum {
    ERR_OK = 0,
    ERR_IO,               /* syscall failure (details go to the log) */
    ERR_NOT_FOUND,
    ERR_AUTH,             /* bad username or password */
    ERR_ALREADY_LOGGED_IN,
    ERR_PERMISSION,       /* role not allowed for this op */
    ERR_INACTIVE,         /* account/user deactivated */
    ERR_INSUFFICIENT,     /* not enough balance */
    ERR_INVALID,          /* bad argument or amount */
    ERR_STATE,            /* illegal loan transition */
    ERR_DUPLICATE,        /* username exists */
    ERR_LIMIT,            /* e.g. open loan already exists */
    ERR_NOT_LOGGED_IN,
    ERR_BUSY              /* non-blocking lock refused */
} Err;

/* ---- records: fixed size, widest fields first, explicit padding ---- */
typedef struct {                    /* id == 0 means empty slot */
    int32_t id;
    int32_t account_id;             /* 0 for staff */
    uint8_t role;                   /* Role */
    uint8_t active;                 /* login allowed */
    uint8_t _pad[6];
    char    username[NAME_LEN];
    char    name[DISP_LEN];
    char    phone[PHONE_LEN];
    char    pw_hash[HASH_LEN];      /* salted hash, never plaintext */
} User;

typedef struct {
    money_t balance;
    int32_t id;
    int32_t user_id;
    uint8_t active;                 /* banking ops allowed (manager toggles) */
    uint8_t _pad[7];
} Account;

typedef struct {
    money_t amount;
    money_t balance_after;
    int64_t ts;
    int32_t id;
    int32_t account_id;
    int32_t peer_account;           /* other side of a transfer, else 0 */
    uint8_t type;                   /* TxnType */
    uint8_t _pad[3];
} Transaction;

typedef struct {
    money_t amount;
    int64_t created;
    int64_t updated;
    int32_t id;
    int32_t account_id;
    int32_t assigned_to;            /* employee user id, 0 = none */
    uint8_t status;                 /* LoanStatus */
    uint8_t _pad[3];
} Loan;

typedef struct {
    int64_t ts;
    int32_t id;
    int32_t user_id;
    char    text[FEEDBACK_LEN];
} Feedback;

#endif
