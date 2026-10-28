#ifndef REPAIR_H
#define REPAIR_H
#include "storage.h"
Err repair_orphans(Store *s);       /* completes customers whose account slot is still empty */
int check_consistency(Store *s);    /* prints problems, returns how many (0 = healthy) */
#endif