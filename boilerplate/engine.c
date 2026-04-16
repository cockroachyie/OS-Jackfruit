/*
 * engine.c  —  OS-Jackfruit container runtime (Person 1)
 * ========================================================
 * Build:   gcc -o engine engine.c -lpthread -Wall -Wextra
 * Usage:
 *   sudo ./engine supervisor <rootfs>        # start long-running supervisor
 *   sudo ./engine start  <name> <rootfs> <cmd> [args...]
 *   sudo ./engine run    <name> <rootfs> <cmd> [args...]
 *   sudo ./engine stop   <name>
 *   sudo ./engine ps
 *   sudo ./engine logs   <name>
 *
 * Architecture:
 *   - One long-running supervisor process.
 *   - CLI commands connect via UNIX domain socket (/tmp/jackfruit.sock).
 *   - Container stdout/stderr are captured via pipes into a bounded
 *     producer-consumer log buffer; a logger thread drains it to disk.
 *   - SIGCHLD reaping is async-signal-safe (writes to self-pipe,
 *     handled in main loop via select).
 *   - SIGINT/SIGTERM trigger orderly shutdown: stop all containers,
 *     join logger threads, close fds, unlink socket.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/select.h>

#include <pthread.h>
#include <sched.h>

/* ------------------------------------------------------------------ */
/*  Optional kernel module integration                                  */
/* ------------------------------------------------------------------ */
#ifdef HAVE_MONITOR
#include "monitor_ioctl.h"
#include <sys/ioctl.h>
static int g_monitor_fd = -1;   /* fd to /dev/container_monitor */
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */
#define SOCK_PATH        "/tmp/jackfruit.sock"
#define LOG_DIR          "/tmp/jackfruit-logs"
#define MAX_CONTAINERS   64
#define MAX_CMD_LEN      1024
#define MAX_NAME_LEN     64

/* Bounded log buffer */
#define LOG_BUF_SLOTS    512          /* number of slots in ring buffer */
#define LOG_BUF_SLOT_SZ  4096         /* bytes per slot                 */

/* Self-pipe for SIGCHLD */
static int g_sigchld_pipe[2] = {-1, -1};

/* Shutdown flag (set by SIGTERM/SIGINT handler) */
static volatile sig_atomic_t g_shutdown = 0;

/* ------------------------------------------------------------------ */
/*  Container state                                                     */
/* ------------------------------------------------------------------ */
typedef enum {
    STATE_STARTING = 0,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_OOM_KILLED,          /* killed by kernel monitor hard limit  */
} ContainerState;

static const char *state_str(ContainerState s) {
    switch (s) {
        case STATE_STARTING:   return "starting";
        case STATE_RUNNING:    return "running";
        case STATE_STOPPED:    return "stopped";
        case STATE_KILLED:     return "killed";
        case STATE_OOM_KILLED: return "oom-killed";
        default:               return "unknown";
    }
}

typedef struct {
    int            used;                    /* slot in use?              */
    char           name[MAX_NAME_LEN];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    long           soft_limit_kb;
    long           hard_limit_kb;
    char           log_path[256];
    int            exit_status;            /* waitpid status             */
    int            pipe_rd;               /* supervisor end of log pipe  */
    pthread_t      logger_tid;            /* per-container logger thread */
} Container;

/* Global container table — protected by g_table_mutex */
static Container   g_table[MAX_CONTAINERS];
static pthread_mutex_t g_table_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  Bounded log buffer (shared ring buffer, one per container)         */
/*                                                                      */
/*  Each Container has its own pipe_rd fd. A dedicated logger thread   */
/*  reads from that pipe and writes to the log file. The "bounded      */
/*  buffer" lives between the pipe (kernel pipe buffer acts as the     */
/*  bounded queue) and an explicit userspace ring buffer that decouples */
/*  reading from writing, so a slow disk write never blocks the read-  */
/*  side for other containers.                                          */
/*                                                                      */
/*  We also maintain a per-container shared ring buffer so that the    */
/*  `logs` command can read recent lines without opening the file.     */
/* ------------------------------------------------------------------ */

typedef struct {
    char   data[LOG_BUF_SLOTS][LOG_BUF_SLOT_SZ];
    int    head;                  /* next slot to write (producer)    */
    int    tail;                  /* next slot to read  (consumer)    */
    int    count;                 /* number of filled slots           */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int    done;                  /* set when pipe EOF reached        */
} LogRingBuf;

/* One ring buffer per container, allocated dynamically */
typedef struct {
    int         container_idx;
    int         pipe_rd;          /* read end of container's stdio pipe */
    LogRingBuf *ring;
} LoggerArg;

/* ------------------------------------------------------------------ */
/*  Utility                                                             */
/* ------------------------------------------------------------------ */
static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    exit(EXIT_FAILURE);
}

static void log_info(const char *fmt, ...) {
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    /* timestamp prefix */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(stderr, "[%02d:%02d:%02d] [supervisor] %s\n",
            tm.tm_hour, tm.tm_min, tm.tm_sec, buf);
}

/* Set fd non-blocking */
static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Set fd close-on-exec */
static void set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* Make directory if it doesn't exist */
static void mkdir_p(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ------------------------------------------------------------------ */
/*  Logger thread                                                       */
/*                                                                      */
/*  Reads from the container's stdio pipe, inserts into the ring       */
/*  buffer, and a second path drains the ring buffer to the log file.  */
/*  Both producer (pipe→ring) and consumer (ring→file) run in the same */
/*  thread for simplicity; the ring buffer decouples the two I/O rates.*/
/* ------------------------------------------------------------------ */
static void *logger_thread(void *arg) {
    LoggerArg  *la   = (LoggerArg *)arg;
    LogRingBuf *ring = la->ring;
    int         rfd  = la->pipe_rd;
    int         cidx = la->container_idx;

    /* Open log file */
    char log_path[256];
    pthread_mutex_lock(&g_table_mutex);
    snprintf(log_path, sizeof log_path, "%s", g_table[cidx].log_path);
    pthread_mutex_unlock(&g_table_mutex);

    FILE *logf = fopen(log_path, "a");
    if (!logf) {
        fprintf(stderr, "logger: cannot open %s: %s\n", log_path, strerror(errno));
        /* keep reading but discard */
    }

    char readbuf[LOG_BUF_SLOT_SZ];
    ssize_t n;

    while (1) {
        /* --- PRODUCER: read from pipe into ring buffer --- */
        n = read(rfd, readbuf, sizeof readbuf - 1);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            /* EOF or error: pipe closed (container exited) */
            break;
        }
        readbuf[n] = '\0';

        /* Insert into ring buffer */
        pthread_mutex_lock(&ring->lock);
        while (ring->count == LOG_BUF_SLOTS && !ring->done) {
            /* Buffer full: wait for consumer to drain (but we ARE the
             * consumer too, so this path only matters if we had separate
             * threads; here we just overwrite the oldest slot) */
            /* Overwrite oldest slot (lossy but never blocks) */
            ring->tail = (ring->tail + 1) % LOG_BUF_SLOTS;
            ring->count--;
        }
        { size_t cplen = (size_t)n < (LOG_BUF_SLOT_SZ-1) ? (size_t)n : (LOG_BUF_SLOT_SZ-1); memcpy(ring->data[ring->head], readbuf, cplen); ring->data[ring->head][cplen] = 0; }
        ring->data[ring->head][LOG_BUF_SLOT_SZ - 1] = '\0';
        ring->head  = (ring->head + 1) % LOG_BUF_SLOTS;
        ring->count++;
        pthread_cond_signal(&ring->not_empty);
        pthread_mutex_unlock(&ring->lock);

        /* --- CONSUMER: drain ring buffer to file --- */
        pthread_mutex_lock(&ring->lock);
        while (ring->count > 0) {
            const char *slot = ring->data[ring->tail];
            ring->tail  = (ring->tail + 1) % LOG_BUF_SLOTS;
            ring->count--;
            pthread_mutex_unlock(&ring->lock);

            if (logf) {
                fputs(slot, logf);
                fflush(logf);
            }
            pthread_mutex_lock(&ring->lock);
        }
        pthread_mutex_unlock(&ring->lock);
    }

    /* Flush anything remaining in the ring */
    pthread_mutex_lock(&ring->lock);
    ring->done = 1;
    pthread_cond_broadcast(&ring->not_empty);
    while (ring->count > 0) {
        const char *slot = ring->data[ring->tail];
        ring->tail  = (ring->tail + 1) % LOG_BUF_SLOTS;
        ring->count--;
        pthread_mutex_unlock(&ring->lock);
        if (logf) { fputs(slot, logf); fflush(logf); }
        pthread_mutex_lock(&ring->lock);
    }
    pthread_mutex_unlock(&ring->lock);

    if (logf) fclose(logf);
    close(rfd);
    free(la->ring);
    free(la);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Container lifecycle                                                 */
/* ------------------------------------------------------------------ */

/* Pivot root helper: mount proc inside rootfs, pivot, exec cmd */
static void container_init(const char *rootfs, char *const argv[]) {
    /* Mount proc inside the new rootfs */
    char proc_path[256];
    snprintf(proc_path, sizeof proc_path, "%s/proc", rootfs);
    mkdir_p(proc_path);

    if (mount("proc", proc_path, "proc", 0, NULL) < 0) {
        /* non-fatal if already mounted */
    }

    /* chroot into rootfs */
    if (chroot(rootfs) < 0) {
        perror("chroot");
        exit(EXIT_FAILURE);
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        exit(EXIT_FAILURE);
    }

    /* exec the target command */
    execvp(argv[0], argv);
    perror("execvp");
    exit(EXIT_FAILURE);
}

/*
 * spawn_container: fork+clone with namespaces, wire stdio pipe,
 * update table, register with kernel monitor, start logger thread.
 *
 * Returns container index, or -1 on error.
 * foreground=1: caller will waitpid after this returns.
 */
static int spawn_container(const char *name,
                           const char *rootfs,
                           char *const cmd_argv[],
                           long soft_kb,
                           long hard_kb,
                           int  foreground)
{
    /* Check for name collision */
    pthread_mutex_lock(&g_table_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_table[i].used && strcmp(g_table[i].name, name) == 0) {
            pthread_mutex_unlock(&g_table_mutex);
            fprintf(stderr, "container '%s' already exists\n", name);
            return -1;
        }
    }

    /* Find free slot */
    int idx = -1;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!g_table[i].used) { idx = i; break; }
    }
    if (idx < 0) {
        pthread_mutex_unlock(&g_table_mutex);
        fprintf(stderr, "max containers reached\n");
        return -1;
    }

    /* Prepare log path */
    mkdir_p(LOG_DIR);
    memset(&g_table[idx], 0, sizeof(Container));
    g_table[idx].used = 1;
    strncpy(g_table[idx].name, name, MAX_NAME_LEN - 1);
    g_table[idx].state         = STATE_STARTING;
    g_table[idx].soft_limit_kb = soft_kb;
    g_table[idx].hard_limit_kb = hard_kb;
    g_table[idx].pipe_rd       = -1;
    snprintf(g_table[idx].log_path, sizeof g_table[idx].log_path,
             "%s/%s.log", LOG_DIR, name);
    pthread_mutex_unlock(&g_table_mutex);

    /* Create stdio pipe: child writes, supervisor reads */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        pthread_mutex_lock(&g_table_mutex);
        g_table[idx].used = 0;
        pthread_mutex_unlock(&g_table_mutex);
        return -1;
    }
    set_cloexec(pipefd[0]);  /* read end: close in child after dup2 */

    /* Fork with new namespaces */
    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = syscall(SYS_clone, clone_flags, NULL, NULL, NULL, NULL);

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        pthread_mutex_lock(&g_table_mutex);
        g_table[idx].used = 0;
        pthread_mutex_unlock(&g_table_mutex);
        return -1;
    }

    if (pid == 0) {
        /* ---- CHILD (container) ---- */
        close(pipefd[0]);               /* close supervisor's read end  */

        /* Redirect stdout and stderr into the pipe */
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* Set hostname to container name (UTS namespace) */
        (void)sethostname(name, strlen(name));

        /* Enter rootfs and exec */
        container_init(rootfs, cmd_argv);
        /* never reached */
        exit(EXIT_FAILURE);
    }

    /* ---- PARENT (supervisor) ---- */
    close(pipefd[1]);   /* close write end; child owns it */

    pthread_mutex_lock(&g_table_mutex);
    g_table[idx].host_pid   = pid;
    g_table[idx].start_time = time(NULL);
    g_table[idx].state      = STATE_RUNNING;
    g_table[idx].pipe_rd    = pipefd[0];
    pthread_mutex_unlock(&g_table_mutex);

    log_info("container '%s' started, pid=%d", name, pid);

    /* Register with kernel monitor (if compiled in) */
#ifdef HAVE_MONITOR
    if (g_monitor_fd >= 0) {
        struct monitor_reg reg = {
            .pid           = pid,
            .soft_limit_kb = soft_kb,
            .hard_limit_kb = hard_kb,
        };
        strncpy(reg.name, name, sizeof reg.name - 1);
        if (ioctl(g_monitor_fd, MONITOR_IOC_REGISTER, &reg) < 0) {
            perror("ioctl REGISTER");
        }
    }
#endif

    /* Start logger thread */
    if (!foreground) {
        LoggerArg *la = malloc(sizeof *la);
        if (!la) die("malloc logger arg");
        la->container_idx = idx;
        la->pipe_rd       = pipefd[0];
        la->ring          = calloc(1, sizeof(LogRingBuf));
        if (!la->ring) die("calloc ring");
        pthread_mutex_init(&la->ring->lock, NULL);
        pthread_cond_init(&la->ring->not_empty, NULL);
        pthread_cond_init(&la->ring->not_full,  NULL);

        pthread_mutex_lock(&g_table_mutex);
        g_table[idx].pipe_rd = pipefd[0]; /* logger will close this */
        pthread_mutex_unlock(&g_table_mutex);

        pthread_t tid;
        if (pthread_create(&tid, NULL, logger_thread, la) != 0) {
            die("pthread_create logger");
        }
        pthread_mutex_lock(&g_table_mutex);
        g_table[idx].logger_tid = tid;
        pthread_mutex_unlock(&g_table_mutex);
    }

    return idx;
}

/* ------------------------------------------------------------------ */
/*  SIGCHLD handler — async-signal-safe via self-pipe                  */
/* ------------------------------------------------------------------ */
static void sigchld_handler(int sig) {
    (void)sig;
    char byte = 1;
    /* write is async-signal-safe */
    (void)write(g_sigchld_pipe[1], &byte, 1);
}

/* Drain the self-pipe and reap all children */
static void reap_children(void) {
    /* Drain self-pipe */
    char buf[64];
    while (read(g_sigchld_pipe[0], buf, sizeof buf) > 0);

    int   status;
    pid_t pid;

    /* Reap all exited children without blocking */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_table_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (g_table[i].used && g_table[i].host_pid == pid) {
                g_table[i].exit_status = status;

                if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
                    /* Could be OOM-kill or supervisor stop — check monitor */
#ifdef HAVE_MONITOR
                    if (g_monitor_fd >= 0) {
                        struct monitor_event ev = { .pid = pid };
                        if (ioctl(g_monitor_fd, MONITOR_IOC_POLL_EVENT, &ev) == 0
                            && ev.event_type == MONITOR_EVENT_HARD_KILL) {
                            g_table[i].state = STATE_OOM_KILLED;
                            log_info("container '%s' (pid=%d) killed by hard memory limit",
                                     g_table[i].name, pid);
                            goto next;
                        }
                    }
#endif
                    g_table[i].state = STATE_KILLED;
                } else {
                    g_table[i].state = STATE_STOPPED;
                }
                log_info("container '%s' (pid=%d) exited, status=%d",
                         g_table[i].name, pid,
                         WIFEXITED(status) ? WEXITSTATUS(status) : -1);
#ifdef HAVE_MONITOR
                next:
#endif
                /* Unregister from kernel monitor */
#ifdef HAVE_MONITOR
                if (g_monitor_fd >= 0) {
                    pid_t dead_pid = pid;
                    ioctl(g_monitor_fd, MONITOR_IOC_UNREGISTER, &dead_pid);
                }
#endif
                break;
            }
        }
        pthread_mutex_unlock(&g_table_mutex);
    }
}

/* ------------------------------------------------------------------ */
/*  SIGTERM/SIGINT handler — orderly shutdown                          */
/* ------------------------------------------------------------------ */
static void sigterm_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* ------------------------------------------------------------------ */
/*  CLI command handling                                                */
/*                                                                      */
/*  Protocol: newline-terminated ASCII commands over UNIX socket.      */
/*  Client sends:  "<verb> <args...>\n"                                 */
/*  Supervisor replies: lines of text terminated by ".\n" (end marker) */
/* ------------------------------------------------------------------ */

#define CMD_REPLY_END ".\n"

/* Send a formatted reply over the client socket fd */
static void reply(int fd, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    (void)write(fd, buf, strlen(buf));
}

static void handle_ps(int cfd) {
    reply(cfd, "%-20s %-8s %-12s %-10s %s\n",
          "NAME", "PID", "STATE", "UPTIME(s)", "LOG");
    pthread_mutex_lock(&g_table_mutex);
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!g_table[i].used) continue;
        long uptime = (long)(now - g_table[i].start_time);
        reply(cfd, "%-20s %-8d %-12s %-10ld %s\n",
              g_table[i].name,
              g_table[i].host_pid,
              state_str(g_table[i].state),
              uptime,
              g_table[i].log_path);
    }
    pthread_mutex_unlock(&g_table_mutex);
    reply(cfd, CMD_REPLY_END);
}

static void handle_logs(int cfd, const char *name) {
    char log_path[256] = "";
    pthread_mutex_lock(&g_table_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_table[i].used && strcmp(g_table[i].name, name) == 0) {
            strncpy(log_path, g_table[i].log_path, sizeof log_path - 1);
            break;
        }
    }
    pthread_mutex_unlock(&g_table_mutex);

    if (!log_path[0]) {
        reply(cfd, "error: no container named '%s'\n" CMD_REPLY_END, name);
        return;
    }

    FILE *f = fopen(log_path, "r");
    if (!f) {
        reply(cfd, "error: cannot open log: %s\n" CMD_REPLY_END, strerror(errno));
        return;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        (void)write(cfd, line, strlen(line));
    }
    fclose(f);
    reply(cfd, CMD_REPLY_END);
}

static void handle_stop(int cfd, const char *name) {
    pid_t target_pid = -1;
    pthread_mutex_lock(&g_table_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_table[i].used &&
            strcmp(g_table[i].name, name) == 0 &&
            g_table[i].state == STATE_RUNNING) {
            target_pid = g_table[i].host_pid;
            break;
        }
    }
    pthread_mutex_unlock(&g_table_mutex);

    if (target_pid < 0) {
        reply(cfd, "error: no running container named '%s'\n" CMD_REPLY_END, name);
        return;
    }

    /* Graceful: SIGTERM first, then SIGKILL after 3s */
    kill(target_pid, SIGTERM);
    reply(cfd, "sent SIGTERM to '%s' (pid=%d)\n" CMD_REPLY_END, name, target_pid);

    /* Reaping happens asynchronously via SIGCHLD */
}

/* Parse and dispatch a command received on client fd */
static void dispatch_command(int cfd, char *line) {
    /* Tokenise */
    char *tokens[32];
    int   ntok = 0;
    char *saveptr;
    char *tok = strtok_r(line, " \t\r\n", &saveptr);
    while (tok && ntok < 31) {
        tokens[ntok++] = tok;
        tok = strtok_r(NULL, " \t\r\n", &saveptr);
    }
    if (ntok == 0) { reply(cfd, CMD_REPLY_END); return; }

    const char *verb = tokens[0];

    if (strcmp(verb, "ps") == 0) {
        handle_ps(cfd);

    } else if (strcmp(verb, "logs") == 0 && ntok >= 2) {
        handle_logs(cfd, tokens[1]);

    } else if (strcmp(verb, "stop") == 0 && ntok >= 2) {
        handle_stop(cfd, tokens[1]);

    } else if (strcmp(verb, "start") == 0 && ntok >= 4) {
        /* start <name> <rootfs> <cmd> [args...] */
        const char *name   = tokens[1];
        const char *rootfs = tokens[2];
        char *cmd_argv[32];
        int na = 0;
        for (int i = 3; i < ntok && na < 31; i++)
            cmd_argv[na++] = tokens[i];
        cmd_argv[na] = NULL;

        int idx = spawn_container(name, rootfs, cmd_argv, 0, 0, 0);
        if (idx >= 0)
            reply(cfd, "started '%s' (pid=%d)\n" CMD_REPLY_END,
                  name, g_table[idx].host_pid);
        else
            reply(cfd, "error: failed to start container\n" CMD_REPLY_END);

    } else if (strcmp(verb, "run") == 0 && ntok >= 4) {
        /* run <name> <rootfs> <cmd> [args...] — foreground, wait */
        const char *name   = tokens[1];
        const char *rootfs = tokens[2];
        char *cmd_argv[32];
        int na = 0;
        for (int i = 3; i < ntok && na < 31; i++)
            cmd_argv[na++] = tokens[i];
        cmd_argv[na] = NULL;

        int idx = spawn_container(name, rootfs, cmd_argv, 0, 0, 1);
        if (idx < 0) {
            reply(cfd, "error: failed to start container\n" CMD_REPLY_END);
            return;
        }
        pid_t cpid = g_table[idx].host_pid;

        /* Stream output directly to client */
        int pipefd = g_table[idx].pipe_rd;
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd, buf, sizeof buf)) > 0) {
            (void)write(cfd, buf, n);
        }
        close(pipefd);

        int status;
        waitpid(cpid, &status, 0);

        pthread_mutex_lock(&g_table_mutex);
        g_table[idx].exit_status = status;
        g_table[idx].state       = STATE_STOPPED;
        pthread_mutex_unlock(&g_table_mutex);

        reply(cfd, "\n[container '%s' exited, code=%d]\n" CMD_REPLY_END,
              name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    } else {
        reply(cfd, "unknown command: %s\n"
              "commands: ps | logs <name> | stop <name> | "
              "start <name> <rootfs> <cmd> [args] | "
              "run <name> <rootfs> <cmd> [args]\n"
              CMD_REPLY_END, verb);
    }
}

/* ------------------------------------------------------------------ */
/*  Supervisor main loop                                                */
/* ------------------------------------------------------------------ */
static void run_supervisor(void) {
    /* Create self-pipe for SIGCHLD */
    if (pipe(g_sigchld_pipe) < 0) die("pipe sigchld");
    set_nonblock(g_sigchld_pipe[0]);
    set_nonblock(g_sigchld_pipe[1]);
    set_cloexec(g_sigchld_pipe[0]);
    set_cloexec(g_sigchld_pipe[1]);

    /* Install signal handlers */
    struct sigaction sa_chld = { .sa_handler = sigchld_handler, .sa_flags = SA_RESTART };
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = { .sa_handler = sigterm_handler, .sa_flags = 0 };
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* Open kernel monitor device (optional) */
#ifdef HAVE_MONITOR
    g_monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (g_monitor_fd < 0) {
        fprintf(stderr, "[supervisor] kernel monitor not available: %s\n",
                strerror(errno));
    } else {
        log_info("kernel monitor connected");
    }
#endif

    /* Create UNIX domain socket */
    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv_fd < 0) die("socket");

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);
    unlink(SOCK_PATH);
    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof addr) < 0)
        die("bind");
    if (listen(srv_fd, 8) < 0) die("listen");
    set_nonblock(srv_fd);

    log_info("supervisor ready, socket=%s", SOCK_PATH);

    /* ---- Main select loop ---- */
    while (!g_shutdown) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_sigchld_pipe[0], &rfds);
        FD_SET(srv_fd, &rfds);
        int maxfd = (srv_fd > g_sigchld_pipe[0]) ? srv_fd : g_sigchld_pipe[0];

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* SIGCHLD notification */
        if (FD_ISSET(g_sigchld_pipe[0], &rfds)) {
            reap_children();
        }

        /* Incoming CLI connection */
        if (FD_ISSET(srv_fd, &rfds)) {
            int cfd = accept(srv_fd, NULL, NULL);
            if (cfd >= 0) {
                /* Read one command line */
                char cmdbuf[MAX_CMD_LEN] = {0};
                ssize_t n = read(cfd, cmdbuf, sizeof cmdbuf - 1);
                if (n > 0) {
                    cmdbuf[n] = '\0';
                    dispatch_command(cfd, cmdbuf);
                }
                close(cfd);
            }
        }
    }

    /* ---- Orderly shutdown ---- */
    log_info("shutting down...");

    /* Stop all running containers */
    pthread_mutex_lock(&g_table_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_table[i].used && g_table[i].state == STATE_RUNNING) {
            kill(g_table[i].host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_table_mutex);

    /* Give containers up to 3s to exit, then SIGKILL */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 3;

    int running;
    do {
        reap_children();
        running = 0;
        pthread_mutex_lock(&g_table_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (g_table[i].used && g_table[i].state == STATE_RUNNING) {
                running++;
            }
        }
        pthread_mutex_unlock(&g_table_mutex);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec >= deadline.tv_sec) {
            /* Force kill remaining */
            pthread_mutex_lock(&g_table_mutex);
            for (int i = 0; i < MAX_CONTAINERS; i++) {
                if (g_table[i].used && g_table[i].state == STATE_RUNNING) {
                    kill(g_table[i].host_pid, SIGKILL);
                }
            }
            pthread_mutex_unlock(&g_table_mutex);
            break;
        }
        if (running) usleep(100000); /* 100ms */
    } while (running);

    /* Join all logger threads */
    pthread_mutex_lock(&g_table_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_table[i].used && g_table[i].logger_tid) {
            pthread_t tid = g_table[i].logger_tid;
            pthread_mutex_unlock(&g_table_mutex);
            pthread_join(tid, NULL);
            pthread_mutex_lock(&g_table_mutex);
            g_table[i].logger_tid = 0;
        }
    }
    pthread_mutex_unlock(&g_table_mutex);

    /* Clean up kernel monitor */
#ifdef HAVE_MONITOR
    if (g_monitor_fd >= 0) close(g_monitor_fd);
#endif

    close(srv_fd);
    unlink(SOCK_PATH);
    log_info("supervisor exited cleanly");
}

/* ------------------------------------------------------------------ */
/*  CLI client — connects to supervisor socket, sends command, prints  */
/* ------------------------------------------------------------------ */
static void run_client(int argc, char *argv[]) {
    /* Build command string from argv */
    char cmdbuf[MAX_CMD_LEN] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(cmdbuf, " ", sizeof cmdbuf - strlen(cmdbuf) - 1);
        strncat(cmdbuf, argv[i], sizeof cmdbuf - strlen(cmdbuf) - 1);
    }
    strncat(cmdbuf, "\n", sizeof cmdbuf - strlen(cmdbuf) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) die("socket");

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "error: cannot connect to supervisor (%s)\n"
                "Is the supervisor running? Try: sudo ./engine supervisor <rootfs>\n",
                strerror(errno));
        close(fd);
        exit(EXIT_FAILURE);
    }

    /* Send command */
    (void)write(fd, cmdbuf, strlen(cmdbuf));

    /* Read response until end marker */
    char buf[4096];
    ssize_t n;
    char accum[MAX_CMD_LEN * 4] = {0};
    size_t acc_len = 0;

    while ((n = read(fd, buf, sizeof buf)) > 0) {
        /* Write to stdout */
        (void)write(STDOUT_FILENO, buf, n);

        /* Check if end marker received */
        if (acc_len + n < sizeof accum) {
            memcpy(accum + acc_len, buf, n);
            acc_len += n;
        }
        if (acc_len >= 2 &&
            accum[acc_len-2] == '.' && accum[acc_len-1] == '\n') {
            break;
        }
    }

    close(fd);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <rootfs>              # start supervisor\n"
        "  %s start  <name> <rootfs> <cmd> [args]\n"
        "  %s run    <name> <rootfs> <cmd> [args]\n"
        "  %s stop   <name>\n"
        "  %s ps\n"
        "  %s logs   <name>\n",
        prog, prog, prog, prog, prog, prog);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    if (argc < 2) usage(argv[0]);

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) usage(argv[0]);
        /* rootfs arg is informational for supervisor mode;
         * individual start/run commands specify it per container */
        run_supervisor();
    } else {
        /* All other verbs are CLI client commands forwarded to supervisor */
        if (strcmp(argv[1], "start")  != 0 &&
            strcmp(argv[1], "run")    != 0 &&
            strcmp(argv[1], "stop")   != 0 &&
            strcmp(argv[1], "ps")     != 0 &&
            strcmp(argv[1], "logs")   != 0) {
            usage(argv[0]);
        }
        run_client(argc, argv);
    }

    return 0;
}
