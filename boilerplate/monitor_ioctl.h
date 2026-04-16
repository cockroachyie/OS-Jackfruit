#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

/*
 * monitor_ioctl.h
 * ---------------
 * Shared interface between the user-space supervisor (engine.c) and the
 * kernel-space memory monitor (monitor.c / monitor.ko).
 *
 * Device node: /dev/container_monitor
 *
 * Workflow:
 *   1. Supervisor opens /dev/container_monitor
 *   2. After launching a container, supervisor calls MONITOR_IOC_REGISTER
 *      with the container's HOST pid and configured memory limits.
 *   3. The kernel module begins periodic RSS checks on that pid.
 *   4. On soft-limit breach: kernel logs a warning via dmesg.
 *      Optionally, if MONITOR_IOC_POLL_EVENT is implemented, user space
 *      can read kill/warn events without polling dmesg.
 *   5. On hard-limit breach: kernel sends SIGKILL to the process.
 *   6. Supervisor calls MONITOR_IOC_UNREGISTER when container stops
 *      (or the kernel auto-removes stale entries for exited pids).
 */

#define MONITOR_MAGIC  'M'

/* ------------------------------------------------------------------ */
/*  Structures passed through ioctl                                    */
/* ------------------------------------------------------------------ */

/*
 * Used by MONITOR_IOC_REGISTER.
 * Pass the host PID of the container's init process plus memory limits.
 * Limits are in kilobytes (matching /proc/<pid>/status VmRSS units).
 */
struct monitor_reg {
    pid_t  pid;               /* host PID of container init process   */
    long   soft_limit_kb;     /* warn when RSS exceeds this (KB)       */
    long   hard_limit_kb;     /* kill when RSS exceeds this (KB)       */
    char   name[64];          /* human-readable container name         */
};

/*
 * Used by MONITOR_IOC_GET_STATUS.
 * Supervisor can query current RSS and limit-breach state for a pid.
 */
struct monitor_status {
    pid_t  pid;               /* in: which pid to query               */
    long   current_rss_kb;    /* out: current RSS in KB               */
    int    soft_breached;     /* out: 1 if soft limit was ever hit    */
    int    hard_breached;     /* out: 1 if hard limit was triggered   */
};

/*
 * Used by MONITOR_IOC_POLL_EVENT (optional for Person 2).
 * Supervisor can poll this to learn when the LKM killed a container,
 * so metadata can be updated without relying solely on SIGCHLD.
 * Returns 0 if no pending event, 1 if event is valid.
 */
#define MONITOR_EVENT_NONE      0
#define MONITOR_EVENT_SOFT_WARN 1
#define MONITOR_EVENT_HARD_KILL 2

struct monitor_event {
    pid_t  pid;               /* which pid triggered the event        */
    int    event_type;        /* MONITOR_EVENT_*                      */
    long   rss_at_event_kb;   /* RSS reading that triggered the event */
};

/* ------------------------------------------------------------------ */
/*  ioctl command numbers                                              */
/* ------------------------------------------------------------------ */

/* Register a container pid for monitoring */
#define MONITOR_IOC_REGISTER    _IOW(MONITOR_MAGIC, 1, struct monitor_reg)

/* Unregister a container pid (supervisor-initiated stop) */
#define MONITOR_IOC_UNREGISTER  _IOW(MONITOR_MAGIC, 2, pid_t)

/* Query current RSS and breach state for a pid */
#define MONITOR_IOC_GET_STATUS  _IOWR(MONITOR_MAGIC, 3, struct monitor_status)

/* Poll for the next pending kill/warn event (non-blocking) */
#define MONITOR_IOC_POLL_EVENT  _IOR(MONITOR_MAGIC, 4, struct monitor_event)

#define MONITOR_IOC_MAXNR       4

#endif /* MONITOR_IOCTL_H */
