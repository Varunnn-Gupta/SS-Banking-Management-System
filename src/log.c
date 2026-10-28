// src/log.c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "log.h"

static int g_fd = STDERR_FILENO;
static LogLevel g_min = LOGLVL_DEBUG;

int log_init(const char *path, LogLevel min) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    g_fd = fd; g_min = min;
    return 0;
}

void log_msg(LogLevel lvl, const char *file, int line, const char *fmt, ...) {
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    if (lvl < g_min) return;
    char buf[512];
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    int n = (int)strftime(buf, sizeof buf, "%F %T", &tm);
    n += snprintf(buf + n, sizeof buf - (size_t)n, " [%s] [t%ld] %s:%d ",
                  names[lvl], (long)syscall(SYS_gettid), file, line);
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    n += m;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    (void)!write(g_fd, buf, (size_t)n);   /* one write() per line: O_APPEND keeps lines whole */
}
