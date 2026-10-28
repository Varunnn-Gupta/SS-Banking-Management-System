#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "protocol.h"

static int g_bad;

static void check(const char *name, int cond) {
    printf("[%-46s] %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) g_bad++;
}

static int dial(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

static int rpc(int fd, const Request *rq, Response *rs) {
    if (net_send_all(fd, rq, sizeof *rq) != 0) return -1;
    return net_recv_all(fd, rs, sizeof *rs);
}

static int do_login(int fd, const char *u, const char *p, Response *rs) {
    Request rq;
    memset(&rq, 0, sizeof rq);
    rq.op = OP_LOGIN;
    snprintf(rq.s1, sizeof rq.s1, "%s", u);
    snprintf(rq.s2, sizeof rq.s2, "%s", p);
    return rpc(fd, &rq, rs);
}

/* the server frees a dead connection's session a moment later, so retry briefly */
static int login_retry(int fd, const char *u, const char *p) {
    Response rs;
    memset(&rs, 0, sizeof rs);
    for (int i = 0; i < 30; i++) {
        if (do_login(fd, u, p, &rs) != 0) return -1;
        if (rs.status != ERR_ALREADY_LOGGED_IN) return rs.status;
        usleep(100000);
    }
    return ERR_ALREADY_LOGGED_IN;
}

static int readable(int fd, int ms) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, ms) > 0;
}

/* ---- 1. same user, 8 simultaneous logins ---- */
#define RT 8
static pthread_barrier_t g_bar;
static int g_st[RT], g_fd[RT];

static void *w_login(void *p) {
    int i = (int)(intptr_t)p;
    Response rs;
    memset(&rs, 0, sizeof rs);
    g_fd[i] = dial();
    pthread_barrier_wait(&g_bar);                       /* all fire at the same instant */
    g_st[i] = (g_fd[i] >= 0 && do_login(g_fd[i], "admin1", "pass123", &rs) == 0) ? rs.status : -1;
    return NULL;
}

static void test_race(void) {
    pthread_t th[RT];
    pthread_barrier_init(&g_bar, NULL, RT);
    for (intptr_t i = 0; i < RT; i++) pthread_create(&th[i], NULL, w_login, (void *)i);
    for (int i = 0; i < RT; i++) pthread_join(th[i], NULL);
    pthread_barrier_destroy(&g_bar);
    int ok = 0, dup = 0;
    for (int i = 0; i < RT; i++) {
        if (g_st[i] == ERR_OK) ok++;
        else if (g_st[i] == ERR_ALREADY_LOGGED_IN) dup++;
    }
    check("same user x8 at once: exactly 1 login wins", ok == 1 && dup == RT - 1);
    for (int i = 0; i < RT; i++) if (g_fd[i] >= 0) close(g_fd[i]);
    int fd = dial();
    check("session freed after the winner disconnects", fd >= 0 && login_retry(fd, "admin1", "pass123") == ERR_OK);
    if (fd >= 0) close(fd);
}

/* ---- 2. semaphore: the 101st client waits ---- */
static void test_sem(void) {
    int fds[MAX_CLIENTS], n = 0, served = 0;
    for (; n < MAX_CLIENTS; n++) { fds[n] = dial(); if (fds[n] < 0) break; }
    check("100 connections opened", n == MAX_CLIENTS);
    Request rq;
    Response rs;
    memset(&rq, 0, sizeof rq);
    memset(&rs, 0, sizeof rs);
    rq.op = OP_BALANCE;                                  /* not logged in: harmless, proves we are served */
    for (int i = 0; i < n; i++)
        if (rpc(fds[i], &rq, &rs) == 0 && rs.status == ERR_NOT_LOGGED_IN) served++;
    check("all 100 are being served", served == MAX_CLIENTS);
    int extra = dial();
    int sent = extra >= 0 && net_send_all(extra, &rq, sizeof rq) == 0;
    check("101st client waits while all slots are taken", sent && !readable(extra, 1500));
    close(fds[0]);                                       /* free one slot */
    int ok = sent && readable(extra, 6000) && net_recv_all(extra, &rs, sizeof rs) == 0 &&
             rs.status == ERR_NOT_LOGGED_IN;
    check("101st is served as soon as a slot frees", ok);
    for (int i = 1; i < n; i++) close(fds[i]);
    if (extra >= 0) close(extra);
}

/* ---- 3. garbage input ---- */
static void drain(int fd) {
    char buf[4096];
    while (readable(fd, 30)) if (recv(fd, buf, sizeof buf, 0) <= 0) break;
}

static void test_fuzz(void) {
    srand(20261028);
    for (int round = 0; round < 100; round++) {
        int fd = dial();
        if (fd < 0) break;
        if (round % 2 == 0) { Response rs; (void)do_login(fd, "cust4", "pass123", &rs); }
        int k = 1 + rand() % 5;
        for (int j = 0; j < k; j++) {
            Request rq;
            unsigned char *b = (unsigned char *)&rq;
            for (size_t x = 0; x < sizeof rq; x++) b[x] = (unsigned char)rand();
            if (rand() % 3) rq.op = (uint16_t)(rand() % 110);                 /* mostly real op codes */
            if (rand() % 2) { rq.amount &= 0xFFFF; rq.id1 &= 0xFF; rq.id2 &= 0xFF; }   /* plausible values */
            if (net_send_all(fd, &rq, sizeof rq) != 0) break;
            drain(fd);
        }
        if (rand() % 4 == 0) {                           /* half a request, then hang up */
            Request rq;
            memset(&rq, 0xAB, sizeof rq);
            (void)send(fd, &rq, sizeof rq / 2, MSG_NOSIGNAL);
        }
        close(fd);
    }
    int fd = dial();
    int st = (fd >= 0) ? login_retry(fd, "cust4", "pass123") : -1;
    Request rq;
    Response rs;
    memset(&rq, 0, sizeof rq);
    memset(&rs, 0, sizeof rs);
    rq.op = OP_BALANCE;
    check("server alive after 100 garbage sessions", st == ERR_OK);
    check("...and still serves real requests", st == ERR_OK && rpc(fd, &rq, &rs) == 0 && rs.status == ERR_OK);
    if (fd >= 0) close(fd);
}

/* ---- 4. idle timeout (server must run with BANK_IDLE_SECS=<secs>) ---- */
static void test_idle(int secs) {
    int fd = dial();
    if (fd < 0) { check("connect", 0); return; }
    struct timeval tv = { .tv_sec = secs + 10 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    time_t t0 = time(NULL);
    char c;
    ssize_t n = recv(fd, &c, 1, 0);                      /* blocks until the server hangs up */
    long el = (long)(time(NULL) - t0);
    check("silent client dropped after idle timeout", n == 0 && el >= secs - 1 && el <= secs + 3);
    close(fd);
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "all";
    int probe = dial();
    if (probe < 0) { puts("Cannot connect: start ./build/<mode>/server first."); return 1; }
    close(probe);
    if (!strcmp(m, "idle")) test_idle(argc > 2 ? atoi(argv[2]) : 3);
    else if (!strcmp(m, "race")) test_race();
    else if (!strcmp(m, "sem")) test_sem();
    else if (!strcmp(m, "fuzz")) test_fuzz();
    else { test_race(); test_sem(); test_fuzz(); }
    puts(g_bad ? "RESULT: FAIL" : "RESULT: ALL PASS");
    return g_bad ? 1 : 0;
}