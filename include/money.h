#ifndef MONEY_H
#define MONEY_H
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include "models.h"

static inline void fmt_money(char *buf, size_t n, money_t v) {
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    snprintf(buf, n, "%sRs %llu.%02llu", v < 0 ? "-" : "", u / 100, u % 100);
}

/* "250", "250.5", "250.50" -> paise. Rejects signs, junk, >2 decimals. 0 = ok */
static inline int parse_money(const char *s, money_t *out) {
    long long whole = 0;
    int digits = 0;
    while (isdigit((unsigned char)*s)) {
        whole = whole * 10 + (*s - '0');
        if (++digits > 12) return -1;
        s++;
    }
    if (digits == 0) return -1;
    int frac = 0;
    if (*s == '.') {
        int nd = 0;
        s++;
        while (isdigit((unsigned char)*s)) {
            if (nd == 2) return -1;
            frac = frac * 10 + (*s - '0');
            nd++; s++;
        }
        if (nd == 0) return -1;
        if (nd == 1) frac *= 10;
    }
    if (*s != '\0') return -1;
    *out = (money_t)whole * 100 + frac;
    return 0;
}
#endif
