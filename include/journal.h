#ifndef JOURNAL_H
#define JOURNAL_H
#include <stdint.h>
#include "storage.h"

enum { JK_ACCOUNT = 1, JK_TXN, JK_LOAN };                       /* what a change targets */
enum { JS_FREE = 0, JS_PREPARED, JS_COMMITTED, JS_DONE };      /* entry state */
#define JMAX  4
#define JDATA 64

typedef struct { uint8_t kind; uint8_t _p[3]; int32_t id; uint8_t data[JDATA]; } JChange;
typedef struct { int32_t id; uint8_t state; uint8_t n; uint8_t _p[2]; JChange ch[JMAX]; } JEntry;
typedef struct { int slot; JEntry e; } Jrnl;

Err jrnl_begin(Store *s, Jrnl *j);
Err jrnl_add(Jrnl *j, int kind, int id, const void *rec, size_t sz);
Err jrnl_commit(Store *s, Jrnl *j);     /* durable + applied, or error */
Err jrnl_recover(Store *s);             /* once at startup, before accepting clients */
#endif
