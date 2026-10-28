#ifndef AUTH_H
#define AUTH_H
#include "models.h"
#include "storage.h"

Err pw_hash(const char *pw, char out[HASH_LEN]);      /* salted SHA-512 crypt */
int pw_verify(const char *pw, const char *stored);    /* 1 = match */

Err user_find_by_name(Store *s, const char *name, User *out);
/* Creates a user with a unique username. account_id = 0 for staff. */
Err user_create(Store *s, const char *username, const char *password,
                Role role, const char *name, int account_id, User *out);

Err user_set_password(Store *s, int uid, const char *oldpw, const char *newpw);

Err user_get(Store *s, int id, User *out);
Err user_update_details(Store *s, int uid, const char *name, const char *phone, int only_role);
Err user_set_role(Store *s, int uid, int newrole, int *old_role);

#endif
