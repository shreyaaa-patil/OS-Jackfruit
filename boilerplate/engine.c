#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include "monitor_ioctl.h"

/* ──────────────────────────────────────────────
 * Constants
 * ────────────────────────────────────────────── */
#define STACK_SIZE      (1024 * 1024)
#define MAX_CONTAINERS  16
#define MAX_ID_LEN      32
#define MAX_PATH_LEN    256
#define MAX_CMD_LEN     512
#define LOG_BUF_SIZE    64        /* bounded buffer slots */
#define LOG_LINE_MAX    512
#define SOCK_PATH       "/tmp/mini_runtime.sock"
#define MONITOR_DEV     "/dev/container_monitor"

/* ──────────────────────────────────────────────
 * Container states
 * ────────────────────────────────────────────── */
typedef enum {
    STATE_STARTING = 0,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_HARD_LIMIT_KILLED,
    STATE_EXITED
} ContainerState;

static const char *state_str(ContainerState s) {
    switch (s) {
        case STATE_STARTING:          return "starting";
        case STATE_RUNNING:           return "running";
        case STATE_STOPPED:           return "stopped";
        case STATE_KILLED:            return "killed";
        case STATE_HARD_LIMIT_KILLED: return "hard_limit_killed";
        case STATE_EXITED:            return "exited";
        default:                      return "unknown";
    }
}

/* ──────────────────────────────────────────────
 * Log buffer entry
 * ────────────────────────────────────────────── */
typedef struct {
    char line[LOG_LINE_MAX];
} LogEntry;

/* ──────────────────────────────────────────────
 * Bounded log buffer (per-container)
 * ────────────────────────────────────────────── */
typedef struct {
    LogEntry   buf[LOG_BUF_SIZE];
    int        head, tail, count;
    int        done;          /* producer sets 1 when pipe closed */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} LogBuffer;

/* ──────────────────────────────────────────────
 * Container metadata
 * ────────────────────────────────────────────── */
typedef struct {
    int            active;
    char           id[MAX_ID_LEN];
    char           rootfs[MAX_PATH_LEN];
    char           command[MAX_CMD_LEN];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    unsigned long  soft_mib, hard_mib;
    char           log_path[MAX_PATH_LEN];
    int            exit_code;
    int            stop_requested;   /* set before sending SIGTERM */
    int            pipe_stdout[2];
    int            pipe_stderr[2];
    LogBuffer      logbuf;
    pthread_t      prod_thread;      /* reads pipe → logbuf */
    pthread_t      cons_thread;      /* logbuf → file */
    pthread_mutex_t meta_lock;
} Container;

/* ──────────────────────────────────────────────
 * Global state
 * ────────────────────────────────────────────── */
static Container containers[MAX_CONTAINERS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int supervisor_running = 1;
static int monitor_fd = -1;

/* ──────────────────────────────────────────────
 * Utility: timestamp string
 * ────────────────────────────────────────────── */
static void ts_str(char *buf, size_t n) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", t);
}

/* ──────────────────────────────────────────────
 * Log buffer helpers
 * ────────────────────────────────────────────── */
static void lb_init(LogBuffer *lb) {
    lb->head = lb->tail = lb->count = lb->done = 0;
    pthread_mutex_init(&lb->lock, NULL);
    pthread_cond_init(&lb->not_empty, NULL);
    pthread_cond_init(&lb->not_full, NULL);
}

static void lb_push(LogBuffer *lb, const char *line) {
    pthread_mutex_lock(&lb->lock);
    while (lb->count == LOG_BUF_SIZE && !lb->done)
        pthread_cond_wait(&lb->not_full, &lb->lock);
    if (lb->count < LOG_BUF_SIZE) {
        strncpy(lb->buf[lb->tail].line, line, LOG_LINE_MAX - 1);
        lb->tail = (lb->tail + 1) % LOG_BUF_SIZE;
        lb->count++;
        pthread_cond_signal(&lb->not_empty);
    }
    pthread_mutex_unlock(&lb->lock);
}

static int lb_pop(LogBuffer *lb, char *out) {
    pthread_mutex_lock(&lb->lock);
    while (lb->count == 0 && !lb->done)
        pthread_cond_wait(&lb->not_empty, &lb->lock);
    if (lb->count == 0) {
        pthread_mutex_unlock(&lb->lock);
        return 0; /* done + empty */
    }
    strncpy(out, lb->buf[lb->head].line, LOG_LINE_MAX - 1);
    lb->head = (lb->head + 1) % LOG_BUF_SIZE;
    lb->count--;
    pthread_cond_signal(&lb->not_full);
    pthread_mutex_unlock(&lb->lock);
    return 1;
}

static void lb_mark_done(LogBuffer *lb) {
    pthread_mutex_lock(&lb->lock);
    lb->done = 1;
    pthread_cond_broadcast(&lb->not_empty);
    pthread_cond_broadcast(&lb->not_full);
    pthread_mutex_unlock(&lb->lock);
}

/* ──────────────────────────────────────────────
 * Producer thread: reads pipe → logbuf
 * ────────────────────────────────────────────── */
typedef struct { Container *c; int fd; } ProdArg;

static void *producer_thread(void *arg) {
    ProdArg *pa = (ProdArg *)arg;
    Container *c = pa->c;
    int fd       = pa->fd;
    free(pa);

    char line[LOG_LINE_MAX];
    char ts[32];
    char buf[LOG_LINE_MAX * 2];
    int  pos = 0;
    char ch;

    while (read(fd, &ch, 1) == 1) {
        if (pos < (int)sizeof(buf) - 1)
            buf[pos++] = ch;
        if (ch == '\n' || pos >= (int)sizeof(buf) - 1) {
            buf[pos] = '\0';
            ts_str(ts, sizeof(ts));
            snprintf(line, sizeof(line), "[%s] %s", ts, buf);
            lb_push(&c->logbuf, line);
            pos = 0;
        }
    }
    /* flush partial line */
    if (pos > 0) {
        buf[pos] = '\0';
        ts_str(ts, sizeof(ts));
        snprintf(line, sizeof(line), "[%s] %s\n", ts, buf);
        lb_push(&c->logbuf, line);
    }
    close(fd);
    lb_mark_done(&c->logbuf);
    return NULL;
}

/* ──────────────────────────────────────────────
 * Consumer thread: logbuf → file
 * ────────────────────────────────────────────── */
static void *consumer_thread(void *arg) {
    Container *c = (Container *)arg;
    FILE *f = fopen(c->log_path, "a");
    if (!f) {
        fprintf(stderr, "[supervisor] Cannot open log file %s\n", c->log_path);
        return NULL;
    }
    char line[LOG_LINE_MAX];
    while (lb_pop(&c->logbuf, line)) {
        fputs(line, f);
        fflush(f);
    }
    fclose(f);
    return NULL;
}

/* ──────────────────────────────────────────────
 * Find container by id (caller holds table_lock)
 * ────────────────────────────────────────────── */
static Container *find_container(const char *id) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].active && strcmp(containers[i].id, id) == 0)
            return &containers[i];
    return NULL;
}

static Container *alloc_container(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!containers[i].active)
            return &containers[i];
    return NULL;
}

/* ──────────────────────────────────────────────
 * SIGCHLD handler — reap children
 * ────────────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&table_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].active && containers[i].host_pid == pid) {
                Container *c = &containers[i];
                pthread_mutex_lock(&c->meta_lock);
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    if (c->stop_requested)
                        c->state = STATE_STOPPED;
                    else
                        c->state = STATE_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_code = 128 + WTERMSIG(status);
                    if (c->stop_requested)
                        c->state = STATE_STOPPED;
                    else if (WTERMSIG(status) == SIGKILL)
                        c->state = STATE_HARD_LIMIT_KILLED;
                    else
                        c->state = STATE_KILLED;
                }
                /* close write ends of pipes so producers finish */
                close(c->pipe_stdout[1]);
                close(c->pipe_stderr[1]);
                pthread_mutex_unlock(&c->meta_lock);
                /* unregister from kernel monitor */
                if (monitor_fd >= 0) {
                    struct monitor_request req;
                    memset(&req, 0, sizeof(req));
                    req.pid = pid;
                    strncpy(req.container_id, c->id, MONITOR_NAME_LEN - 1);
                    ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
                }
                break;
            }
        }
        pthread_mutex_unlock(&table_lock);
    }
}

/* ──────────────────────────────────────────────
 * Container init function (runs inside clone())
 * ────────────────────────────────────────────── */
typedef struct {
    char rootfs[MAX_PATH_LEN];
    char command[MAX_CMD_LEN];
    int  pipe_out[2]; /* stdout pipe */
    int  pipe_err[2]; /* stderr pipe */
} CloneArg;

static char clone_stack[STACK_SIZE];

static int container_init(void *arg) {
    CloneArg *ca = (CloneArg *)arg;

    /* redirect stdout/stderr to supervisor pipes */
    dup2(ca->pipe_out[1], STDOUT_FILENO);
    dup2(ca->pipe_err[1], STDERR_FILENO);
    close(ca->pipe_out[0]); close(ca->pipe_out[1]);
    close(ca->pipe_err[0]); close(ca->pipe_err[1]);

    /* filesystem isolation */
    if (chroot(ca->rootfs) != 0) { perror("chroot"); return 1; }
    if (chdir("/") != 0)          { perror("chdir");  return 1; }
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount /proc (non-fatal)");

    /* exec the command */
    char *argv[] = { ca->command, NULL };
    execv(ca->command, argv);
    perror("execv");
    return 1;
}

/* ──────────────────────────────────────────────
 * Launch a container
 * ────────────────────────────────────────────── */
static int launch_container(const char *id, const char *rootfs,
                             const char *cmd,
                             unsigned long soft_mib, unsigned long hard_mib)
{
    pthread_mutex_lock(&table_lock);
    if (find_container(id)) {
        pthread_mutex_unlock(&table_lock);
        return -1; /* already exists */
    }
    Container *c = alloc_container();
    if (!c) {
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    memset(c, 0, sizeof(*c));
    c->active = 1;
    strncpy(c->id,      id,     MAX_ID_LEN - 1);
    strncpy(c->rootfs,  rootfs, MAX_PATH_LEN - 1);
    strncpy(c->command, cmd,    MAX_CMD_LEN - 1);
    c->soft_mib   = soft_mib;
    c->hard_mib   = hard_mib;
    c->start_time = time(NULL);
    c->state      = STATE_STARTING;
    snprintf(c->log_path, MAX_PATH_LEN, "/tmp/log_%s.txt", id);
    pthread_mutex_init(&c->meta_lock, NULL);

    /* create pipes */
    if (pipe(c->pipe_stdout) < 0 || pipe(c->pipe_stderr) < 0) {
        c->active = 0;
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    lb_init(&c->logbuf);

    /* start consumer thread before clone */
    pthread_create(&c->cons_thread, NULL, consumer_thread, c);

    /* start producer threads (one per fd) */
    ProdArg *pa1 = malloc(sizeof(ProdArg));
    pa1->c = c; pa1->fd = c->pipe_stdout[0];
    pthread_create(&c->prod_thread, NULL, producer_thread, pa1);

    /* we'll handle stderr inline — dup it to stdout pipe */
    /* (simple approach: merge stderr into same logbuf via dup) */
    ProdArg *pa2 = malloc(sizeof(ProdArg));
    pa2->c = c; pa2->fd = c->pipe_stderr[0];
    pthread_t prod2;
    pthread_create(&prod2, NULL, producer_thread, pa2);
    pthread_detach(prod2);

    /* clone args */
    static CloneArg ca; /* static so clone child can access */
    strncpy(ca.rootfs,  rootfs, MAX_PATH_LEN - 1);
    strncpy(ca.command, cmd,    MAX_CMD_LEN - 1);
    memcpy(ca.pipe_out, c->pipe_stdout, sizeof(ca.pipe_out));
    memcpy(ca.pipe_err, c->pipe_stderr, sizeof(ca.pipe_err));

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(container_init, clone_stack + STACK_SIZE, flags, &ca);

    if (pid < 0) {
        c->active = 0;
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    c->host_pid = pid;
    c->state    = STATE_RUNNING;

    /* close write ends in supervisor */
    close(c->pipe_stdout[1]); c->pipe_stdout[1] = -1;
    close(c->pipe_stderr[1]); c->pipe_stderr[1] = -1;

    pthread_mutex_unlock(&table_lock);

    /* register with kernel monitor */
    if (monitor_fd >= 0) {
        struct monitor_request req;
        memset(&req, 0, sizeof(req));
        req.pid              = pid;
        req.soft_limit_bytes = soft_mib * 1024 * 1024;
        req.hard_limit_bytes = hard_mib * 1024 * 1024;
        strncpy(req.container_id, id, MONITOR_NAME_LEN - 1);
        if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
            perror("[supervisor] ioctl MONITOR_REGISTER");
    }

    printf("[supervisor] Started container '%s' pid=%d\n", id, pid);
    return 0;
}

/* ──────────────────────────────────────────────
 * Stop a container
 * ────────────────────────────────────────────── */
static int stop_container(const char *id) {
    pthread_mutex_lock(&table_lock);
    Container *c = find_container(id);
    if (!c || c->state != STATE_RUNNING) {
        pthread_mutex_unlock(&table_lock);
        return -1;
    }
    c->stop_requested = 1;
    kill(c->host_pid, SIGTERM);
    pthread_mutex_unlock(&table_lock);
    return 0;
}

/* ──────────────────────────────────────────────
 * PS command output
 * ────────────────────────────────────────────── */
static void cmd_ps(int out_fd) {
    char buf[4096];
    int  off = 0;
    off += snprintf(buf + off, sizeof(buf) - off,
        "%-12s %-8s %-20s %-8s %-8s %-8s %-8s\n",
        "ID", "PID", "STARTED", "STATE", "SOFT_MiB", "HARD_MiB", "EXIT");
    off += snprintf(buf + off, sizeof(buf) - off,
        "%-12s %-8s %-20s %-8s %-8s %-8s %-8s\n",
        "------------", "--------", "--------------------",
        "--------", "--------", "--------", "--------");
    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].active) continue;
        Container *c = &containers[i];
        char ts[24];
        struct tm *t = localtime(&c->start_time);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
        off += snprintf(buf + off, sizeof(buf) - off,
            "%-12s %-8d %-20s %-8s %-8lu %-8lu %-8d\n",
            c->id, c->host_pid, ts,
            state_str(c->state),
            c->soft_mib, c->hard_mib, c->exit_code);
    }
    pthread_mutex_unlock(&table_lock);
    write(out_fd, buf, off);
}

/* ──────────────────────────────────────────────
 * LOGS command: dump log file to client
 * ────────────────────────────────────────────── */
static void cmd_logs(const char *id, int out_fd) {
    pthread_mutex_lock(&table_lock);
    Container *c = find_container(id);
    char path[MAX_PATH_LEN] = {0};
    if (c) strncpy(path, c->log_path, MAX_PATH_LEN - 1);
    pthread_mutex_unlock(&table_lock);

    if (!path[0]) {
        write(out_fd, "ERROR: container not found\n", 27);
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        write(out_fd, "ERROR: no log file yet\n", 23);
        return;
    }
    char line[LOG_LINE_MAX];
    while (fgets(line, sizeof(line), f))
        write(out_fd, line, strlen(line));
    fclose(f);
}

/* ──────────────────────────────────────────────
 * Parse and dispatch CLI command
 * ────────────────────────────────────────────── */
static void dispatch_command(char *cmd_line, int out_fd) {
    char *tokens[32];
    int  ntok = 0;
    char *p = strtok(cmd_line, " \t\n");
    while (p && ntok < 31) { tokens[ntok++] = p; p = strtok(NULL, " \t\n"); }
    if (ntok == 0) return;

    if (strcmp(tokens[0], "ps") == 0) {
        cmd_ps(out_fd);
        return;
    }
    if (strcmp(tokens[0], "logs") == 0 && ntok >= 2) {
        cmd_logs(tokens[1], out_fd);
        return;
    }
    if (strcmp(tokens[0], "stop") == 0 && ntok >= 2) {
        int r = stop_container(tokens[1]);
        char msg[64];
        snprintf(msg, sizeof(msg), r == 0 ? "OK: stopping %s\n" : "ERROR: cannot stop %s\n", tokens[1]);
        write(out_fd, msg, strlen(msg));
        return;
    }
    if ((strcmp(tokens[0], "start") == 0 || strcmp(tokens[0], "run") == 0) && ntok >= 4) {
        const char *id     = tokens[1];
        const char *rootfs = tokens[2];
        const char *cmd    = tokens[3];
        unsigned long soft = 40, hard = 64;
        int is_run = (strcmp(tokens[0], "run") == 0);
        for (int i = 4; i < ntok - 1; i++) {
            if (strcmp(tokens[i], "--soft-mib") == 0) soft = atol(tokens[i+1]);
            if (strcmp(tokens[i], "--hard-mib") == 0) hard = atol(tokens[i+1]);
        }
        int r = launch_container(id, rootfs, cmd, soft, hard);
        char msg[128];
        if (r == 0) {
            snprintf(msg, sizeof(msg), "OK: container '%s' started\n", id);
            write(out_fd, msg, strlen(msg));
            if (is_run) {
                /* block until container exits */
                pthread_mutex_lock(&table_lock);
                Container *c = find_container(id);
                pid_t pid = c ? c->host_pid : -1;
                pthread_mutex_unlock(&table_lock);
                if (pid > 0) waitpid(pid, NULL, 0);
                /* join threads */
                pthread_mutex_lock(&table_lock);
                c = find_container(id);
                if (c) {
                    pthread_join(c->prod_thread, NULL);
                    pthread_join(c->cons_thread, NULL);
                }
                pthread_mutex_unlock(&table_lock);
                write(out_fd, "OK: container exited\n", 21);
            }
        } else {
            snprintf(msg, sizeof(msg), "ERROR: could not start '%s'\n", id);
            write(out_fd, msg, strlen(msg));
        }
        return;
    }
    write(out_fd, "ERROR: unknown command\n", 23);
}

/* ──────────────────────────────────────────────
 * SUPERVISOR MODE: listen on UNIX socket
 * ────────────────────────────────────────────── */
static void handle_sigterm(int sig) {
    (void)sig;
    supervisor_running = 0;
}
static void run_supervisor(const char *rootfs_base) {
    printf("[supervisor] Starting. Base rootfs: %s\n", rootfs_base);

    /* open kernel monitor if available */
    monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (monitor_fd < 0)
        printf("[supervisor] Warning: cannot open %s (module not loaded?)\n", MONITOR_DEV);
    else
        printf("[supervisor] Kernel monitor opened.\n");

    /* install SIGCHLD handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* create socket */
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    listen(srv, 8);
    printf("[supervisor] Listening on %s\n", SOCK_PATH);

    /* handle SIGTERM for orderly shutdown */
    signal(SIGTERM, handle_sigterm);

    while (supervisor_running) {
        fd_set fds;
        FD_ZERO(&fds); FD_SET(srv, &fds);
        struct timeval tv = {1, 0};
        if (select(srv + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) continue;

        char buf[MAX_CMD_LEN];
        ssize_t n = read(cli, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            dispatch_command(buf, cli);
        }
        close(cli);
    }

    /* orderly shutdown: stop all containers */
    printf("[supervisor] Shutting down...\n");
    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].active && containers[i].state == STATE_RUNNING) {
            containers[i].stop_requested = 1;
            kill(containers[i].host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&table_lock);
    sleep(2);

    /* join all threads */
    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].active) {
            pthread_join(containers[i].prod_thread, NULL);
            pthread_join(containers[i].cons_thread, NULL);
        }
    }
    pthread_mutex_unlock(&table_lock);

    if (monitor_fd >= 0) close(monitor_fd);
    close(srv);
    unlink(SOCK_PATH);
    printf("[supervisor] Exited cleanly.\n");
}

/* ──────────────────────────────────────────────
 * CLI CLIENT MODE: send command to supervisor
 * ────────────────────────────────────────────── */
static void run_cli(int argc, char *argv[]) {
    /* build command string */
    char cmd[MAX_CMD_LEN] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        exit(1);
    }
    write(sock, cmd, strlen(cmd));
    shutdown(sock, SHUT_WR);
    char buf[4096];
    ssize_t n;
    while ((n = read(sock, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, n);
    close(sock);
}

/* ──────────────────────────────────────────────
 * main
 * ────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "  %s run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "  %s ps\n"
            "  %s logs  <id>\n"
            "  %s stop  <id>\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "supervisor needs <base-rootfs>\n"); return 1; }
        run_supervisor(argv[2]);
    } else {
        run_cli(argc, argv);
    }
    return 0;
}
