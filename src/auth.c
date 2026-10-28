#define _GNU_SOURCE
#include <crypt.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include "auth.h"
#include "log.h"

/* User creation is rare and happens inside this one server process, so an
 * in-process mutex makes "check duplicate + allocate id" atomic. */
static pthread_mutex_t g_create_mu = PTHREAD_MUTEX_INITIALIZER;

Err pw_hash(const char *pw, char out[HASH_LEN]) {
    static const char al[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char rnd[16];
    char salt[24];
    if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) {
        LOG_SYS("getrandom");
        return ERR_IO;
    }
    memcpy(salt, "$6$", 3);
    for (int i = 0; i < 16; i++) salt[3 + i] = al[rnd[i] & 63];
    salt[19] = '$';
    salt[20] = '\0';
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    char *h = crypt_r(pw, salt, &cd);
    if (!h || h[0] == '*' || strlen(h) >= HASH_LEN) {
        LOG_E("crypt_r failed");
        return ERR_IO;
    }
    snprintf(out, HASH_LEN, "%s", h);
    return ERR_OK;
}

int pw_verify(const char *pw, const char *stored) {
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    char *h = crypt_r(pw, stored, &cd);
    if (!h || h[0] == '*') return 0;
    size_t n = strlen(stored);
    if (strlen(h) != n) return 0;
    unsigned char diff = 0;                 /* constant-time compare */
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(h[i] ^ stored[i]);
    return diff == 0;
}

Err user_find_by_name(Store *s, const char *name, User *out) {
    struct stat st;
    if (fstat(s->users, &st) == -1) { LOG_SYS("fstat users"); return ERR_IO; }
    int n = (int)(st.st_size / (off_t)sizeof(User));
    for (int id = 1; id < n; id++) {
        User u;
        Err e = rec_lock(s->users, sizeof u, id, LK_SHARED);
        if (e != ERR_OK) return e;
        e = rec_read(s->users, sizeof u, id, &u);
        rec_unlock(s->users, sizeof u, id);
        if (e != ERR_OK) return e;
        if (u.id != 0 && strcmp(u.username, name) == 0) { *out = u; return ERR_OK; }
    }
    return ERR_NOT_FOUND;
}

Err user_create(Store *s, const char *username, const char *password,
                Role role, const char *name, int account_id, User *out) {
    if (!username || !*username || strlen(username) >= NAME_LEN) return ERR_INVALID;
    for (const char *p = username; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.' && *p != '-') return ERR_INVALID;
    if (!password || strlen(password) < 4 || strlen(password) >= ARG_LEN_MAX) return ERR_INVALID;
    if (!name) name = "";
    if (strlen(name) >= DISP_LEN) return ERR_INVALID;

    User u;
    memset(&u, 0, sizeof u);
    Err e = pw_hash(password, u.pw_hash);
    if (e != ERR_OK) return e;
    u.account_id = account_id;
    u.role = (uint8_t)role;
    u.active = 1;
    snprintf(u.username, sizeof u.username, "%s", username);
    snprintf(u.name, sizeof u.name, "%s", name);

    pthread_mutex_lock(&g_create_mu);
    User dup;
    e = user_find_by_name(s, username, &dup);
    if (e == ERR_OK) {
        e = ERR_DUPLICATE;
    } else if (e == ERR_NOT_FOUND) {
        int id = rec_alloc(s->users, sizeof(User));
        if (id < 0) {
            e = ERR_IO;
        } else {
            u.id = id;
            e = rec_lock(s->users, sizeof u, id, LK_EXCLUSIVE);
            if (e == ERR_OK) {
                e = rec_write(s->users, sizeof u, id, &u);
                if (e == ERR_OK) e = rec_sync(s->users);
                rec_unlock(s->users, sizeof u, id);
            }
        }
    }
    pthread_mutex_unlock(&g_create_mu);
    if (e == ERR_OK && out) *out = u;
    return e;
}

Err user_set_password(Store *s, int uid, const char *oldpw, const char *newpw) {
    if (!newpw || strlen(newpw) < 4 || strlen(newpw) >= ARG_LEN_MAX) return ERR_INVALID;
    Err e = rec_lock(s->users, sizeof(User), uid, LK_EXCLUSIVE);
    if (e != ERR_OK) return e;
    User u;
    e = rec_read(s->users, sizeof u, uid, &u);
    if (e == ERR_OK && u.id == 0) e = ERR_NOT_FOUND;
    if (e == ERR_OK && !pw_verify(oldpw, u.pw_hash)) e = ERR_AUTH;
    if (e == ERR_OK) e = pw_hash(newpw, u.pw_hash);
    if (e == ERR_OK) e = rec_write(s->users, sizeof u, uid, &u);
    if (e == ERR_OK) e = rec_sync(s->users);
    rec_unlock(s->users, sizeof u, uid);
    return e;
}

static int clean_text(const char *s, size_t max) {      /* printable only, shorter than max */
    size_t n = strlen(s);
    if (n >= max) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 32 || s[i] == 127) return 0;
    return 1;
}

Err user_get(Store *s, int id, User *out) {
    Err e = rec_lock(s->users, sizeof *out, id, LK_SHARED);
    if (e != ERR_OK) return e;
    e = rec_read(s->users, sizeof *out, id, out);
    rec_unlock(s->users, sizeof *out, id);
    if (e == ERR_OK && out->id == 0) e = ERR_NOT_FOUND;
    return e;
}

/* Blank name/phone = keep the old value. only_role != 0 restricts the target's role. */
Err user_update_details(Store *s, int uid, const char *name, const char *phone, int only_role) {
    if (!clean_text(name, DISP_LEN) || !clean_text(phone, PHONE_LEN)) return ERR_INVALID;
    Err e = rec_lock(s->users, sizeof(User), uid, LK_EXCLUSIVE);
    if (e != ERR_OK) return e;
    User u;
    e = rec_read(s->users, sizeof u, uid, &u);
    if (e == ERR_OK && u.id == 0) e = ERR_NOT_FOUND;
    if (e == ERR_OK && only_role && u.role != only_role) e = ERR_PERMISSION;
    if (e == ERR_OK) {
        if (name[0])  snprintf(u.name, sizeof u.name, "%s", name);
        if (phone[0]) snprintf(u.phone, sizeof u.phone, "%s", phone);
        e = rec_write(s->users, sizeof u, uid, &u);
        if (e == ERR_OK) e = rec_sync(s->users);
    }
    rec_unlock(s->users, sizeof u, uid);
    return e;
}

static pthread_mutex_t g_role_mu = PTHREAD_MUTEX_INITIALIZER;

static int count_admins(Store *s) {
    struct stat st;
    if (fstat(s->users, &st) == -1) return 0;           /* error => count 0 => refuse (safe side) */
    int n = (int)(st.st_size / (off_t)sizeof(User)), cnt = 0;
    for (int id = 1; id < n; id++) {
        User u;
        if (user_get(s, id, &u) == ERR_OK && u.role == ROLE_ADMIN && u.active) cnt++;
    }
    return cnt;
}

/* ERR_STATE: customer involved. ERR_LIMIT: would remove the last admin. */
Err user_set_role(Store *s, int uid, int newrole, int *old_role) {
    if (newrole < ROLE_CUSTOMER || newrole > ROLE_ADMIN) return ERR_INVALID;
    pthread_mutex_lock(&g_role_mu);                     /* role changes are rare: serialise them */
    User t;
    Err e = user_get(s, uid, &t);
    if (e == ERR_OK) {
        *old_role = t.role;
        if (t.role == newrole) {
            /* nothing to do */
        } else if (t.role == ROLE_CUSTOMER || newrole == ROLE_CUSTOMER) {
            e = ERR_STATE;
        } else if (t.role == ROLE_ADMIN && count_admins(s) <= 1) {
            e = ERR_LIMIT;
        } else {
            e = rec_lock(s->users, sizeof t, uid, LK_EXCLUSIVE);
            if (e == ERR_OK) {
                e = rec_read(s->users, sizeof t, uid, &t);
                if (e == ERR_OK) {
                    t.role = (uint8_t)newrole;
                    e = rec_write(s->users, sizeof t, uid, &t);
                    if (e == ERR_OK) e = rec_sync(s->users);
                }
                rec_unlock(s->users, sizeof t, uid);
            }
        }
    }
    pthread_mutex_unlock(&g_role_mu);
    return e;
}