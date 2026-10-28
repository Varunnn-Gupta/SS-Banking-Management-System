#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#include "protocol.h"
#include "money.h"

static const char *role_name(int r) {
    switch (r) {
        case ROLE_CUSTOMER: return "Customer";
        case ROLE_EMPLOYEE: return "Employee";
        case ROLE_MANAGER:  return "Manager";
        case ROLE_ADMIN:    return "Administrator";
        default:            return "Unknown";
    }
}

/* 0 ok, -1 on EOF. Over-long input is truncated and the rest discarded. */
static int read_line(const char *prompt, char *buf, size_t n) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)n, stdin)) { buf[0] = '\0'; return -1; }
    if (!strchr(buf, '\n')) { int c; while ((c = getchar()) != '\n' && c != EOF) {} }
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static int read_secret(const char *prompt, char *buf, size_t n) {
    struct termios old, nw;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) { nw = old; nw.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(STDIN_FILENO, TCSANOW, &nw); }
    int r = read_line(prompt, buf, n);
    if (tty) { tcsetattr(STDIN_FILENO, TCSANOW, &old); putchar('\n'); }
    return r;
}

static int rpc(int fd, const Request *rq, Response *rs) {
    if (net_send_all(fd, rq, sizeof *rq) != 0 || net_recv_all(fd, rs, sizeof *rs) != 0) {
        puts("Connection to the server was lost.");
        return -1;
    }
    rs->msg[MSG_LEN - 1] = '\0';
    return 0;
}

/* for list replies: print frames until the server marks the last one */
static int rpc_stream(int fd, const Request *rq) {
    Response rs;
    if (net_send_all(fd, rq, sizeof *rq) != 0) { puts("Connection to the server was lost."); return -1; }
    do {
        if (net_recv_all(fd, &rs, sizeof rs) != 0) { puts("Connection to the server was lost."); return -1; }
        rs.msg[MSG_LEN - 1] = '\0';
        puts(rs.msg);
    } while (rs.more);
    return 0;
}

/* 0 ok, 1 invalid (message shown), -1 EOF */
static int ask_amount(const char *prompt, money_t *out) {
    char buf[32];
    if (read_line(prompt, buf, sizeof buf) != 0) return -1;
    if (parse_money(buf, out) != 0) { puts("Invalid amount. Use e.g. 250 or 250.50"); return 1; }
    return 0;
}

/* menus return: 1 = logged out, 0 = exit program, -1 = connection lost */
static int customer_menu(int fd) {
    char line[32];
    for (;;) {
        puts("\n=== CUSTOMER MENU ===\n1. View Balance\n2. Deposit\n3. Withdraw\n"
             "4. Transfer Funds\n5. Transaction History\n6. Change Password\n"
             "7. Add Feedback\n8. Apply for Loan\n9. View Loan Status\n10. Logout\n11. Exit");
        if (read_line("Choice: ", line, sizeof line) != 0) return 0;
        Request rq;
        Response rs;
        memset(&rq, 0, sizeof rq);
        int a;
        switch (atoi(line)) {
        case 1: rq.op = OP_BALANCE; break;
        case 2: case 3:
            rq.op = (atoi(line) == 2) ? OP_DEPOSIT : OP_WITHDRAW;
            a = ask_amount("Amount (Rs): ", &rq.amount);
            if (a < 0) return 0;
            if (a > 0) continue;
            break;
        case 4: {
            char buf[32];
            if (read_line("Destination account number: ", buf, sizeof buf) != 0) return 0;
            rq.id1 = atoi(buf);
            a = ask_amount("Amount (Rs): ", &rq.amount);
            if (a < 0) return 0;
            if (a > 0) continue;
            rq.op = OP_TRANSFER;
            break;
        }
        case 5: rq.op = OP_HISTORY; break;
        case 6:
            if (read_secret("Old password: ", rq.s1, sizeof rq.s1) != 0) return 0;
            if (read_secret("New password: ", rq.s2, sizeof rq.s2) != 0) return 0;
            rq.op = OP_CHANGE_PWD;
            break;
        case 7:
            if (read_line("Feedback: ", rq.s1, sizeof rq.s1) != 0) return 0;
            rq.op = OP_FEEDBACK;
            break;
        case 8:
            rq.op = OP_APPLY_LOAN;
            a = ask_amount("Loan amount (Rs): ", &rq.amount);
            if (a < 0) return 0;
            if (a > 0) continue;
            break;
        case 9:  rq.op = OP_LOAN_STATUS; break;
        case 10: rq.op = OP_LOGOUT; break;
        case 11: return 0;        default: puts("Invalid choice."); continue;
        }
        if (rq.op == OP_HISTORY || rq.op == OP_LOAN_STATUS) {
            if (rpc_stream(fd, &rq) != 0) return -1;
            continue;
        }
        if (rpc(fd, &rq, &rs) != 0) return -1;
        printf("-> %s\n", rs.msg);
        if (rq.op == OP_LOGOUT && rs.status == ERR_OK) return 1;
    }
}

static int ask_int(const char *prompt, int *out) {      /* 0 ok, 1 invalid, -1 EOF */
    char buf[24], *end;
    if (read_line(prompt, buf, sizeof buf) != 0) return -1;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0' || v < 0 || v > 1000000000L) { puts("Invalid number."); return 1; }
    *out = (int)v;
    return 0;
}
/* braces only (no do/while) so `continue` reaches the menu loop */
#define ASK_INT(p, v) { int a_ = ask_int((p), &(v)); if (a_ < 0) return 0; if (a_ > 0) continue; }

enum { A_BAD, A_LIST_ACCTS, A_LIST_USERS, A_FEEDBACK, A_ACTIVE, A_ADD, A_MODIFY, A_ROLE,
       A_PWD, A_LOGOUT, A_EXIT, A_MLOANS, A_ASSIGN, A_ELOANS, A_DECIDE, A_CADD, A_CMOD, A_CLIST, A_PASSBOOK };
static const int MGR_MAP[] = { A_BAD, A_LIST_ACCTS, A_ACTIVE, A_MLOANS, A_ASSIGN, A_FEEDBACK, A_PWD, A_LOGOUT, A_EXIT };
static const int ADM_MAP[] = { A_BAD, A_LIST_USERS, A_ADD, A_MODIFY, A_ROLE, A_PWD, A_LOGOUT, A_EXIT };
static const int EMP_MAP[] = { A_BAD, A_CADD, A_CMOD, A_CLIST, A_PASSBOOK, A_ELOANS, A_DECIDE, A_PWD, A_LOGOUT, A_EXIT };

/* returns 1 = logged out, 0 = exit, -1 = connection lost */
static int staff_menu(int fd, int role) {
    const int *map;
    int n;
    const char *text;
    if (role == ROLE_MANAGER) {
        map = MGR_MAP; n = 8;
        text = "1. List Customer Accounts\n2. Activate/Deactivate Account\n3. List Loan Applications\n"
               "4. Assign Loan to Employee\n5. Review Customer Feedback\n6. Change Password\n7. Logout\n8. Exit";
    } else if (role == ROLE_ADMIN) {
        map = ADM_MAP; n = 7;
        text = "1. List Users\n2. Add New Employee\n3. Modify User Details\n4. Change User Role\n"
               "5. Change Password\n6. Logout\n7. Exit";
    } else {
        map = EMP_MAP; n = 9;
        text = "1. Add New Customer\n2. Modify Customer Details\n3. List Customers\n4. View Customer Passbook\n"
               "5. View Assigned Loans\n6. Approve/Reject Loan\n7. Change Password\n8. Logout\n9. Exit";
    }
    char line[32];
    for (;;) {
        printf("\n=== %s MENU ===\n%s\n", role_name(role), text);
        if (read_line("Choice: ", line, sizeof line) != 0) return 0;
        int c = atoi(line);
        int act = (c >= 1 && c <= n) ? map[c] : A_BAD;
        Request rq;
        memset(&rq, 0, sizeof rq);
        switch (act) {
        case A_LIST_ACCTS: rq.op = OP_MGR_LIST_ACCOUNTS; break;
        case A_LIST_USERS: rq.op = OP_ADM_LIST_USERS; break;
        case A_FEEDBACK:   rq.op = OP_MGR_LIST_FEEDBACK; break;
        case A_MLOANS:     rq.op = OP_MGR_LIST_LOANS; break;
        case A_ELOANS:     rq.op = OP_EMP_LIST_LOANS; break;
        case A_ACTIVE:
            rq.op = OP_MGR_SET_ACTIVE;
            ASK_INT("Account number: ", rq.id1);
            ASK_INT("Enter 1 to ACTIVATE, 0 to DEACTIVATE: ", rq.id2);
            break;
        case A_ASSIGN:
            rq.op = OP_MGR_ASSIGN_LOAN;
            ASK_INT("Loan number: ", rq.id1);
            ASK_INT("Employee user id: ", rq.id2);
            break;
        case A_DECIDE:
            rq.op = OP_EMP_DECIDE_LOAN;
            ASK_INT("Loan number: ", rq.id1);
            ASK_INT("Enter 1 to APPROVE, 0 to REJECT: ", rq.id2);
            break;
        case A_ADD:
            rq.op = OP_ADM_ADD_EMPLOYEE;
            if (read_line("New employee username: ", rq.s1, sizeof rq.s1) != 0) return 0;
            if (read_secret("Initial password: ", rq.s2, sizeof rq.s2) != 0) return 0;
            break;
        case A_MODIFY:
            rq.op = OP_ADM_MODIFY_USER;
            ASK_INT("User id: ", rq.id1);
            if (read_line("New name (blank = keep): ", rq.s1, sizeof rq.s1) != 0) return 0;
            if (read_line("New phone (blank = keep): ", rq.s2, sizeof rq.s2) != 0) return 0;
            break;
        case A_ROLE:
            rq.op = OP_ADM_SET_ROLE;
            ASK_INT("User id: ", rq.id1);
            ASK_INT("New role (2=Employee, 3=Manager, 4=Admin): ", rq.id2);
            break;
        case A_PWD:
            rq.op = OP_CHANGE_PWD;
            if (read_secret("Old password: ", rq.s1, sizeof rq.s1) != 0) return 0;
            if (read_secret("New password: ", rq.s2, sizeof rq.s2) != 0) return 0;
            break;
                case A_CADD: {
            rq.op = OP_EMP_ADD_CUSTOMER;
            if (read_line("New customer username: ", rq.s1, sizeof rq.s1) != 0) return 0;
            if (read_secret("Initial password: ", rq.s2, sizeof rq.s2) != 0) return 0;
            int a = ask_amount("Opening balance (Rs, 0 for none): ", &rq.amount);
            if (a < 0) return 0;
            if (a > 0) continue;
            break;
        }
        case A_CMOD:
            rq.op = OP_EMP_MODIFY_CUSTOMER;
            ASK_INT("Customer user id: ", rq.id1);
            if (read_line("New name (blank = keep): ", rq.s1, sizeof rq.s1) != 0) return 0;
            if (read_line("New phone (blank = keep): ", rq.s2, sizeof rq.s2) != 0) return 0;
            break;
        case A_CLIST: rq.op = OP_EMP_LIST_CUSTOMERS; break;
        case A_PASSBOOK:
            rq.op = OP_EMP_PASSBOOK;
            ASK_INT("Account number: ", rq.id1);
            break;
        case A_LOGOUT: rq.op = OP_LOGOUT; break;
        case A_EXIT:   return 0;
        default: puts("Invalid choice."); continue;
        }
        if (rq.op == OP_MGR_LIST_ACCOUNTS || rq.op == OP_MGR_LIST_FEEDBACK || rq.op == OP_ADM_LIST_USERS ||
            rq.op == OP_MGR_LIST_LOANS || rq.op == OP_EMP_LIST_LOANS ||
            rq.op == OP_EMP_LIST_CUSTOMERS || rq.op == OP_EMP_PASSBOOK) {
            if (rpc_stream(fd, &rq) != 0) return -1;
            continue;
        }
        Response rs;
        if (rpc(fd, &rq, &rs) != 0) return -1;
        printf("-> %s\n", rs.msg);
        if (rq.op == OP_LOGOUT && rs.status == ERR_OK) return 1;
    }
}

int main(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); return 1; }

    char line[32];
    for (;;) {
        puts("\n=== BANK: MAIN MENU ===\n1. Login\n2. Exit");
        if (read_line("Choice: ", line, sizeof line) != 0) break;
        int c = atoi(line);
        if (c == 2) break;
        if (c != 1) { puts("Invalid choice."); continue; }

        Request rq;
        Response rs;
        memset(&rq, 0, sizeof rq);
        rq.op = OP_LOGIN;
        if (read_line("Username: ", rq.s1, sizeof rq.s1) != 0) break;
        if (read_secret("Password: ", rq.s2, sizeof rq.s2) != 0) break;
        if (rpc(fd, &rq, &rs) != 0) { close(fd); return 1; }
        printf("-> %s\n", rs.msg);
        if (rs.status != ERR_OK) continue;

        int r = (rs.value == ROLE_CUSTOMER) ? customer_menu(fd) : staff_menu(fd, (int)rs.value);
        if (r < 0) { close(fd); return 1; }
        if (r == 0) break;
    }
    Request rq;
    Response rs;
    memset(&rq, 0, sizeof rq);
    rq.op = OP_EXIT;
    if (rpc(fd, &rq, &rs) == 0) printf("-> %s\n", rs.msg);
    close(fd);
    return 0;
}
