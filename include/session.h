#ifndef SESSION_H
#define SESSION_H
#include "models.h"

typedef struct { int fd; int user_id; int role; int account_id; } Session;

void session_init(Session *s);
int  session_active(const Session *s);
Err  session_acquire(Session *s, const User *u);  /* ERR_ALREADY_LOGGED_IN if held */
void session_release(Session *s);

#endif
