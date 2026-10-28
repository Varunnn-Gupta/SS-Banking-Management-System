#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "handlers.h"
#include "auth.h"
#include "loans.h"
#include "log.h"

static const char *role_str(int r) {
    switch (r) {
    case ROLE_CUSTOMER: return "Customer";
    case ROLE_EMPLOYEE: return "Employee";
    case ROLE_MANAGER:  return "Manager";
    case ROLE_ADMIN:    return "Admin";
    default:            return "?";
    }
}

static int list_users(int fd, Store *s) {
    struct stat st;
    if (fstat(s->users, &st) == -1) { LOG_SYS("fstat users"); return reply(fd, ERR_IO, 0, err_text(ERR_IO)); }
    int n = (int)(st.st_size / (off_t)sizeof(User)), count = 0;
    for (int id = 1; id < n; id++) {
        User u;
        Err e = user_get(s, id, &u);
        if (e == ERR_NOT_FOUND) continue;
        if (e != ERR_OK) return reply(fd, e, 0, err_text(e));
        char line[MSG_LEN];
        snprintf(line, sizeof line, "id %-3d %-16.31s %-9s acct %-4d %-24.47s %.15s",
                 u.id, u.username, role_str(u.role), u.account_id, u.name, u.phone);
        if (reply_more(fd, line) != 0) return -1;
        count++;
    }
    return reply(fd, ERR_OK, count, "-- end of list --");
}

int handle_admin(int fd, Store *s, Session *ss, const Request *rq) {
    char msg[MSG_LEN];
    Err e;
    switch (rq->op) {
    case OP_ADM_LIST_USERS:
        return list_users(fd, s);

    case OP_ADM_ADD_EMPLOYEE: {
        User u;
        e = user_create(s, rq->s1, rq->s2, ROLE_EMPLOYEE, rq->s1, 0, &u);
        LOG_I("admin id %d: add employee '%.31s' -> result %d", ss->user_id, rq->s1, e);
        if (e == ERR_OK) {
            snprintf(msg, sizeof msg, "Employee created. User id: %d", u.id);
            return reply(fd, ERR_OK, u.id, msg);
        }
        return reply(fd, e, 0, e == ERR_DUPLICATE ? "Username already exists."
                     : e == ERR_INVALID ? "Username: letters, digits, _ . - (1-31 chars). Password: 4-63 chars."
                     : err_text(e));
    }
    case OP_ADM_MODIFY_USER:
        e = user_update_details(s, rq->id1, rq->s1, rq->s2, 0);
        LOG_I("admin id %d: modify user %d -> result %d", ss->user_id, rq->id1, e);
        return reply(fd, e, 0, e == ERR_OK ? "Details updated."
                     : e == ERR_NOT_FOUND ? "No such user."
                     : e == ERR_INVALID ? "Name max 47 chars, phone max 15 chars, no control characters."
                     : err_text(e));

    case OP_ADM_SET_ROLE: {
        int old = 0;
        e = user_set_role(s, rq->id1, rq->id2, &old);
        LOG_I("admin id %d: set user %d role %d -> %d, result %d", ss->user_id, rq->id1, old, rq->id2, e);
        if (e == ERR_OK && old == ROLE_EMPLOYEE && rq->id2 != ROLE_EMPLOYEE) {
            /* role is already changed, so no new assignment can reach this user; now release old ones */
            Err e2 = loans_unassign_employee(s, rq->id1);
            if (e2 != ERR_OK) LOG_E("unassign loans of user %d failed (%d)", rq->id1, e2);
        }
        if (e == ERR_OK) {
            snprintf(msg, sizeof msg, "User %d role is now %s%s.", rq->id1, role_str(rq->id2),
                     old == rq->id2 ? " (no change)" : "");
            return reply(fd, ERR_OK, 0, msg);
        }
        return reply(fd, e, 0, e == ERR_NOT_FOUND ? "No such user."
                     : e == ERR_STATE ? "Customers cannot get a staff role, and staff cannot become customers."
                     : e == ERR_LIMIT ? "Cannot demote the last administrator."
                     : e == ERR_INVALID ? "Invalid user id or role (use 2, 3 or 4)."
                     : err_text(e));
    }
    default:
        return reply(fd, ERR_INVALID, 0, "Not implemented yet.");
    }
}