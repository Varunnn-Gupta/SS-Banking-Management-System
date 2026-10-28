#ifndef LOG_H
#define LOG_H
#include <errno.h>
#include <string.h>

typedef enum { LOGLVL_DEBUG, LOGLVL_INFO, LOGLVL_WARN, LOGLVL_ERROR } LogLevel;

int  log_init(const char *path, LogLevel min);   /* before init: logs to stderr */
void log_msg(LogLevel lvl, const char *file, int line, const char *fmt, ...)
     __attribute__((format(printf, 4, 5)));

#ifdef NDEBUG
#define LOG_D(...) ((void)0)
#else
#define LOG_D(...) log_msg(LOGLVL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#endif
#define LOG_I(...) log_msg(LOGLVL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_W(...) log_msg(LOGLVL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_E(...) log_msg(LOGLVL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
/* failed syscall: LOG_SYS("pwrite accounts") */
#define LOG_SYS(what) LOG_E("%s failed: %s", (what), strerror(errno))

#endif
