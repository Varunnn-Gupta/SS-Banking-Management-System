#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "protocol.h"
#include "storage.h"
#include "conn.h"
#include "seed.h"
#include "log.h"
#include "journal.h"
#include "loans.h"
#include "repair.h"
#include "lock.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/time.h>

static sem_t g_slots;                         /* counting semaphore, init MAX_CLIENTS */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int idle_secs(void) {                       /* BANK_IDLE_SECS overrides, for testing */
    const char *e = getenv("BANK_IDLE_SECS");
    int v = e ? atoi(e) : 600;
    return v > 0 ? v : 600;
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    int v = 0;
    sem_getvalue(&g_slots, &v);
    LOG_I("client connected (active=%d/%d)", MAX_CLIENTS - v, MAX_CLIENTS);
    struct timeval tv = { .tv_sec = idle_secs(), .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);    /* silent client: recv fails, thread exits */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);    /* client that never reads: send fails */
    handle_connection(fd);
    close(fd);
    sem_post(&g_slots);                       /* free the slot */
    LOG_I("client disconnected");
    return NULL;
}

int main(int argc, char **argv) {
    if (mkdir("logs", 0755) == -1 && errno != EEXIST) { perror("mkdir logs"); return 1; }
    if (log_init("logs/server.log", LOGLVL_DEBUG) != 0) { perror("log_init"); return 1; }
    if (store_init() != ERR_OK) { fputs("store_init failed (see logs/server.log)\n", stderr); return 1; }
    int lockfd = open("data/server.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lockfd < 0 || lock_range(lockfd, LK_EXCLUSIVE, 0, 1, 0) != ERR_OK) {
        fputs("Another server (or --seed/--check) is already using this data directory.\n", stderr);
        return 1;
    }/* held until the process exits */

    if (argc > 1 && strcmp(argv[1], "--seed") == 0)
        return seed_data() == ERR_OK ? 0 : 1;

    {   /* startup: journal recovery -> loan sweep -> orphan repair -> optional check */
        Store rs;
        if (store_open(&rs) != ERR_OK || jrnl_recover(&rs) != ERR_OK ||
            loans_sweep_orphans(&rs) != ERR_OK || repair_orphans(&rs) != ERR_OK) {
            fputs("recovery failed (see logs/server.log)\n", stderr);
            return 1;
        }
        if (argc > 1 && strcmp(argv[1], "--check") == 0) {
            int bad = check_consistency(&rs);
            store_close(&rs);
            puts(bad ? "CONSISTENCY: PROBLEMS FOUND" : "CONSISTENCY: OK");
            return bad ? 1 : 0;
        }
        store_close(&rs);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;                /* no SA_RESTART: accept()/sem_wait() return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { LOG_SYS("socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { LOG_SYS("bind"); perror("bind"); return 1; }
    if (listen(lfd, 128) < 0) { LOG_SYS("listen"); return 1; }

    if (sem_init(&g_slots, 0, MAX_CLIENTS) != 0) { LOG_SYS("sem_init"); return 1; }

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);

    printf("Server listening on 127.0.0.1:%d (max %d clients). Ctrl+C to stop.\n",
           SERVER_PORT, MAX_CLIENTS);
    LOG_I("server started on port %d", SERVER_PORT);

    while (!g_stop) {
        if (sem_wait(&g_slots) == -1) {       /* wait for a free slot BEFORE accepting */
            if (errno == EINTR) continue;
            LOG_SYS("sem_wait");
            break;
        }
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            sem_post(&g_slots);
            if (errno != EINTR) LOG_SYS("accept");
            continue;
        }
        pthread_t tid;
        if (pthread_create(&tid, &at, client_thread, (void *)(intptr_t)cfd) != 0) {
            LOG_E("pthread_create failed");
            close(cfd);
            sem_post(&g_slots);
        }
    }
    pthread_attr_destroy(&at);
    close(lfd);
    LOG_I("server stopped");
    puts("\nServer stopped.");
    return 0;
}
