#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "conn.h"
#include "handlers.h"
#include "storage.h"
#include "auth.h"
#include "session.h"
#include "log.h"

const char *err_text(Err e) {
    switch (e) {
    case ERR_OK:           return "OK.";
    case ERR_NOT_FOUND:    return "Account or record not found.";
    case ERR_AUTH:         return "Authentication failed.";
    case ERR_PERMISSION:   return "Not allowed for your role.";
    case ERR_INACTIVE:     return "Account is deactivated.";
    case ERR_INSUFFICIENT: return "Insufficient balance.";
    case ERR_INVALID:      return "Invalid input.";
    case ERR_STATE:        return "Not allowed in the current state.";
    case ERR_DUPLICATE:    return "Already exists.";
    case ERR_LIMIT:        return "Limit reached.";
    default:               return "Server error. Please try again.";
    }
}

int reply(int fd, Err st, int64_t val, const char *msg) {
    Response r;
    memset(&r, 0, sizeof r);
    r.status = st;
    r.value = val;
    snprintf(r.msg, sizeof r.msg, "%s", msg);
    return net_send_all(fd, &r, sizeof r);
}

int reply_more(int fd, const char *msg) {
    Response r;
    memset(&r, 0, sizeof r);
    r.more = 1;
    snprintf(r.msg, sizeof r.msg, "%s", msg);
    return net_send_all(fd, &r, sizeof r);
}

/* Which role may use this op? 0 = no specific role (common op). */
static int required_role(int op) {
    if (op >= 20 && op < 40) return ROLE_CUSTOMER;
    if (op >= 40 && op < 60) return ROLE_EMPLOYEE;
    if (op >= 60 && op < 80) return ROLE_MANAGER;
    if (op >= 80 && op < 100) return ROLE_ADMIN;
    return 0;
}

static void do_login(int fd, Store *s, Session *ss, const Request *rq) {
    User u;
    Err e = user_find_by_name(s, rq->s1, &u);
    if (e == ERR_NOT_FOUND || (e == ERR_OK && !pw_verify(rq->s2, u.pw_hash))) {
        LOG_W("failed login for '%s'", rq->s1);
        reply(fd, ERR_AUTH, 0, "Invalid username or password.");
        return;
    }
    if (e != ERR_OK) { reply(fd, ERR_IO, 0, "Server error."); return; }
    if (!u.active)   { reply(fd, ERR_INACTIVE, 0, "This user is deactivated."); return; }

    e = session_acquire(ss, &u);
    if (e == ERR_ALREADY_LOGGED_IN) {
        LOG_W("second login refused for '%s'", u.username);
        reply(fd, e, 0, "This user is already logged in elsewhere.");
    } else if (e != ERR_OK) {
        reply(fd, e, 0, "Server error.");
    } else {
        LOG_I("login: '%s' (role %d)", u.username, u.role);
        reply(fd, ERR_OK, u.role, "Login successful.");
    }
}

static void do_change_pwd(int fd, Store *s, const Session *ss, const Request *rq) {
    Err e = user_set_password(s, ss->user_id, rq->s1, rq->s2);
    const char *m = e == ERR_OK      ? "Password changed."
                  : e == ERR_AUTH    ? "Old password is incorrect."
                  : e == ERR_INVALID ? "New password must be 4-63 characters."
                  : err_text(e);
    if (e == ERR_OK) LOG_I("password changed: user id %d", ss->user_id);
    reply(fd, e, 0, m);
}

/* An admin can change a role while the user is logged in: re-read it on every request. */
static void refresh_role(Store *s, Session *ss) {
    User u;
    if (user_get(s, ss->user_id, &u) == ERR_OK) ss->role = u.role;
}
void handle_connection(int fd) {
    Store s;
    if (store_open(&s) != ERR_OK) { reply(fd, ERR_IO, 0, "Server error."); return; }
    Session ss;
    session_init(&ss);
    Request rq;

    for (;;) {
        if (net_recv_all(fd, &rq, sizeof rq) != 0) break;       /* disconnect */
        req_sanitize(&rq);
        int op = rq.op;

        if (op == OP_EXIT) { reply(fd, ERR_OK, 0, "Goodbye."); break; }

        if (!session_active(&ss)) {                             /* main menu state */
            if (op == OP_LOGIN) do_login(fd, &s, &ss, &rq);
            else reply(fd, ERR_NOT_LOGGED_IN, 0, "Please log in first.");
            continue;
        }

        if (op == OP_LOGIN) {
            reply(fd, ERR_INVALID, 0, "Already logged in. Log out first.");
        } else if (op == OP_LOGOUT) {
            LOG_I("logout: user id %d", ss.user_id);
            session_release(&ss);                               /* back to main menu */
            reply(fd, ERR_OK, 0, "Logged out.");
        } else if (op == OP_CHANGE_PWD) {
            do_change_pwd(fd, &s, &ss, &rq);
        } else {
            int need = required_role(op);
            if (need == 0) {
                reply(fd, ERR_INVALID, 0, "Unknown operation.");
            } else {
                refresh_role(&s, &ss);
                if (need != ss.role)
                    reply(fd, ERR_PERMISSION, 0, "Not allowed for your role. If it was changed, log out and log in again.");
                else if (is_loan_op(op)) handle_loans(fd, &s, &ss, &rq);
                else if (need == ROLE_CUSTOMER) handle_customer(fd, &s, &ss, &rq);
                else if (need == ROLE_MANAGER)  handle_manager(fd, &s, &ss, &rq);
                else if (need == ROLE_ADMIN)    handle_admin(fd, &s, &ss, &rq);
                else if (need == ROLE_EMPLOYEE) handle_employee(fd, &s, &ss, &rq);
                else reply(fd, ERR_INVALID, 0, "Not implemented yet.");
            }
        }
    }                                                           /* <- end of for(;;) */

    if (session_active(&ss)) LOG_I("connection dropped: releasing session of user id %d", ss.user_id);
    session_release(&ss);
    store_close(&s);
}