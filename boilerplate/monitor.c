/*
 * monitor.c — OS-Jackfruit kernel memory monitor (Person 2)
 * ===========================================================
 * Linux Kernel Module (LKM) that:
 *   - Exposes /dev/container_monitor as a char device
 *   - Accepts container PIDs from user-space via ioctl (MONITOR_IOC_REGISTER)
 *   - Tracks each PID in a mutex-protected linked list
 *   - Polls RSS periodically via a kernel work-queue delayed_work
 *   - On soft-limit breach: logs a warning to dmesg (once per container)
 *   - On hard-limit breach: sends SIGKILL and records the event
 *   - Provides MONITOR_IOC_POLL_EVENT so engine.c can read kill events
 *     without polling dmesg, enabling accurate OOM_KILLED state tracking
 *   - Cleans up stale entries for exited processes automatically
 *   - Frees all kernel memory safely on rmmod
 *
 * Build: see Kbuild / Makefile
 * Load:  sudo insmod monitor.ko
 * Verify: ls -l /dev/container_monitor
 * Unload: sudo rmmod monitor
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/signal.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit Team");
MODULE_DESCRIPTION("Container memory monitor with soft/hard limits");
MODULE_VERSION("1.0");

/* ------------------------------------------------------------------ */
/*  Module parameters                                                   */
/* ------------------------------------------------------------------ */
static unsigned int poll_interval_ms = 500;
module_param(poll_interval_ms, uint, 0644);
MODULE_PARM_DESC(poll_interval_ms, "RSS polling interval in milliseconds (default 500)");

/* ------------------------------------------------------------------ */
/*  Device bookkeeping                                                  */
/* ------------------------------------------------------------------ */
#define DEVICE_NAME "container_monitor"
#define CLASS_NAME  "jackfruit"

static int            g_major;
static struct class  *g_class;
static struct device *g_device;
static struct cdev    g_cdev;

/* ------------------------------------------------------------------ */
/*  Per-container tracking entry                                        */
/* ------------------------------------------------------------------ */
#define MAX_PENDING_EVENTS 64

struct container_entry {
    struct list_head node;

    pid_t  pid;
    long   soft_limit_kb;
    long   hard_limit_kb;
    char   name[64];

    /* State flags (only written under g_list_lock) */
    int    soft_warned;     /* 1 after first soft-limit warning */
    int    hard_killed;     /* 1 after SIGKILL sent             */
    long   last_rss_kb;     /* last RSS reading                 */
};

/* Protected linked list of all monitored containers */
static LIST_HEAD(g_container_list);
static DEFINE_MUTEX(g_list_lock);

/* ------------------------------------------------------------------ */
/*  Event queue (soft-warn + hard-kill events for user-space polling)  */
/* ------------------------------------------------------------------ */
struct pending_event {
    struct monitor_event ev;
};

static struct pending_event g_events[MAX_PENDING_EVENTS];
static int g_event_head = 0;   /* next write position */
static int g_event_tail = 0;   /* next read  position */
static int g_event_count = 0;
static DEFINE_SPINLOCK(g_event_lock);

static void push_event(pid_t pid, int type, long rss_kb)
{
    unsigned long flags;
    spin_lock_irqsave(&g_event_lock, flags);
    if (g_event_count < MAX_PENDING_EVENTS) {
        g_events[g_event_head].ev.pid            = pid;
        g_events[g_event_head].ev.event_type     = type;
        g_events[g_event_head].ev.rss_at_event_kb = rss_kb;
        g_event_head = (g_event_head + 1) % MAX_PENDING_EVENTS;
        g_event_count++;
    }
    /* If full: oldest event is silently dropped (ring behaviour) */
    spin_unlock_irqrestore(&g_event_lock, flags);
}

/* Returns 1 if an event was dequeued into *out, 0 if queue empty */
static int pop_event(struct monitor_event *out)
{
    unsigned long flags;
    int found = 0;
    spin_lock_irqsave(&g_event_lock, flags);
    if (g_event_count > 0) {
        *out = g_events[g_event_tail].ev;
        g_event_tail = (g_event_tail + 1) % MAX_PENDING_EVENTS;
        g_event_count--;
        found = 1;
    }
    spin_unlock_irqrestore(&g_event_lock, flags);
    return found;
}

/* ------------------------------------------------------------------ */
/*  RSS reading helper                                                  */
/*                                                                      */
/*  Read VmRSS from /proc/<pid>/status in kernel space.                */
/*  We use get_mm_rss() on the task's mm, which is the canonical       */
/*  kernel-internal RSS (same value as VmRSS in /proc).               */
/* ------------------------------------------------------------------ */
static long read_rss_kb(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_kb = -1;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;   /* process no longer exists */
    }

    mm = get_task_mm(task);
    rcu_read_unlock();

    if (!mm)
        return 0;   /* kernel thread or exiting — treat as 0 RSS */

    /* get_mm_rss returns pages; convert to KB */
    rss_kb = (long)(get_mm_rss(mm) * (PAGE_SIZE / 1024));
    mmput(mm);
    return rss_kb;
}

/* Send signal to a pid (kernel-space) */
static int send_signal_to_pid(pid_t pid, int sig)
{
    struct pid *p;
    int ret;

    rcu_read_lock();
    p = find_vpid(pid);
    if (!p) {
        rcu_read_unlock();
        return -ESRCH;
    }
    ret = kill_pid(p, sig, 1);
    rcu_read_unlock();
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Periodic RSS poll (delayed_work)                                    */
/* ------------------------------------------------------------------ */
static struct delayed_work g_poll_work;

static void poll_rss(struct work_struct *work)
{
    struct container_entry *entry, *tmp;

    mutex_lock(&g_list_lock);

    list_for_each_entry_safe(entry, tmp, &g_container_list, node) {
        long rss = read_rss_kb(entry->pid);

        if (rss < 0) {
            /* Process gone — remove stale entry */
            pr_info("jackfruit: container '%s' (pid %d) no longer exists, "
                    "removing from monitor\n", entry->name, entry->pid);
            list_del(&entry->node);
            kfree(entry);
            continue;
        }

        entry->last_rss_kb = rss;

        /* --- Hard limit check --- */
        if (!entry->hard_killed && entry->hard_limit_kb > 0 &&
            rss > entry->hard_limit_kb) {
            pr_warn("jackfruit: HARD LIMIT: container '%s' (pid %d) "
                    "RSS=%ldKB > hard_limit=%ldKB — sending SIGKILL\n",
                    entry->name, entry->pid, rss, entry->hard_limit_kb);
            entry->hard_killed = 1;
            push_event(entry->pid, MONITOR_EVENT_HARD_KILL, rss);
            send_signal_to_pid(entry->pid, SIGKILL);
            continue;
        }

        /* --- Soft limit check (warn once) --- */
        if (!entry->soft_warned && entry->soft_limit_kb > 0 &&
            rss > entry->soft_limit_kb) {
            pr_warn("jackfruit: SOFT LIMIT: container '%s' (pid %d) "
                    "RSS=%ldKB > soft_limit=%ldKB — warning\n",
                    entry->name, entry->pid, rss, entry->soft_limit_kb);
            entry->soft_warned = 1;
            push_event(entry->pid, MONITOR_EVENT_SOFT_WARN, rss);
        }
    }

    mutex_unlock(&g_list_lock);

    /* Reschedule self */
    schedule_delayed_work(&g_poll_work,
                          msecs_to_jiffies(poll_interval_ms));
}

/* ------------------------------------------------------------------ */
/*  ioctl handler                                                       */
/* ------------------------------------------------------------------ */
static long monitor_ioctl(struct file *filp, unsigned int cmd,
                          unsigned long arg)
{
    int ret = 0;

    if (_IOC_TYPE(cmd) != MONITOR_MAGIC)        return -ENOTTY;
    if (_IOC_NR(cmd)   >  MONITOR_IOC_MAXNR)    return -ENOTTY;

    switch (cmd) {

    /* ---- REGISTER ---- */
    case MONITOR_IOC_REGISTER: {
        struct monitor_reg reg;
        struct container_entry *entry;

        if (copy_from_user(&reg, (void __user *)arg, sizeof reg))
            return -EFAULT;

        /* Reject nonsensical limits */
        if (reg.soft_limit_kb < 0 || reg.hard_limit_kb < 0)
            return -EINVAL;
        if (reg.hard_limit_kb > 0 && reg.soft_limit_kb > reg.hard_limit_kb)
            return -EINVAL;

        entry = kzalloc(sizeof *entry, GFP_KERNEL);
        if (!entry) return -ENOMEM;

        entry->pid           = reg.pid;
        entry->soft_limit_kb = reg.soft_limit_kb;
        entry->hard_limit_kb = reg.hard_limit_kb;
        strncpy(entry->name, reg.name, sizeof entry->name - 1);
        INIT_LIST_HEAD(&entry->node);

        mutex_lock(&g_list_lock);
        /* Prevent duplicate registration */
        {
            struct container_entry *e;
            list_for_each_entry(e, &g_container_list, node) {
                if (e->pid == reg.pid) {
                    mutex_unlock(&g_list_lock);
                    kfree(entry);
                    pr_warn("jackfruit: pid %d already registered\n", reg.pid);
                    return -EEXIST;
                }
            }
        }
        list_add_tail(&entry->node, &g_container_list);
        mutex_unlock(&g_list_lock);

        pr_info("jackfruit: registered container '%s' pid=%d "
                "soft=%ldKB hard=%ldKB\n",
                entry->name, entry->pid,
                entry->soft_limit_kb, entry->hard_limit_kb);
        break;
    }

    /* ---- UNREGISTER ---- */
    case MONITOR_IOC_UNREGISTER: {
        pid_t pid;
        struct container_entry *entry, *tmp;
        int found = 0;

        if (copy_from_user(&pid, (void __user *)arg, sizeof pid))
            return -EFAULT;

        mutex_lock(&g_list_lock);
        list_for_each_entry_safe(entry, tmp, &g_container_list, node) {
            if (entry->pid == pid) {
                list_del(&entry->node);
                pr_info("jackfruit: unregistered container '%s' pid=%d\n",
                        entry->name, entry->pid);
                kfree(entry);
                found = 1;
                break;
            }
        }
        mutex_unlock(&g_list_lock);

        if (!found) ret = -ENOENT;
        break;
    }

    /* ---- GET_STATUS ---- */
    case MONITOR_IOC_GET_STATUS: {
        struct monitor_status st;
        struct container_entry *entry;
        int found = 0;

        if (copy_from_user(&st, (void __user *)arg, sizeof st))
            return -EFAULT;

        mutex_lock(&g_list_lock);
        list_for_each_entry(entry, &g_container_list, node) {
            if (entry->pid == st.pid) {
                long rss = read_rss_kb(entry->pid);
                st.current_rss_kb = (rss >= 0) ? rss : entry->last_rss_kb;
                st.soft_breached  = entry->soft_warned;
                st.hard_breached  = entry->hard_killed;
                found = 1;
                break;
            }
        }
        mutex_unlock(&g_list_lock);

        if (!found) return -ENOENT;
        if (copy_to_user((void __user *)arg, &st, sizeof st))
            return -EFAULT;
        break;
    }

    /* ---- POLL_EVENT ---- */
    case MONITOR_IOC_POLL_EVENT: {
        struct monitor_event ev;
        int has_event = pop_event(&ev);

        if (!has_event) {
            /* No pending event — zero-fill and return 0 */
            memset(&ev, 0, sizeof ev);
            ev.event_type = MONITOR_EVENT_NONE;
        }

        if (copy_to_user((void __user *)arg, &ev, sizeof ev))
            return -EFAULT;

        /* Return 1 if event present, 0 if not */
        ret = has_event;
        break;
    }

    default:
        return -ENOTTY;
    }

    return ret;
}

/* ------------------------------------------------------------------ */
/*  File operations                                                     */
/* ------------------------------------------------------------------ */
static int monitor_open(struct inode *inode, struct file *filp)
{
    /* Only root (CAP_SYS_ADMIN) should open this device.
     * The engine already runs as root, so this is belt-and-braces. */
    if (!capable(CAP_SYS_ADMIN))
        return -EPERM;
    return 0;
}

static int monitor_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .open           = monitor_open,
    .release        = monitor_release,
    .unlocked_ioctl = monitor_ioctl,
};

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                  */
/* ------------------------------------------------------------------ */
static int __init monitor_init(void)
{
    dev_t dev;
    int ret;

    /* Allocate a dynamic major number */
    ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("jackfruit: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }
    g_major = MAJOR(dev);

    /* Initialise and add the cdev */
    cdev_init(&g_cdev, &monitor_fops);
    g_cdev.owner = THIS_MODULE;
    ret = cdev_add(&g_cdev, dev, 1);
    if (ret < 0) {
        pr_err("jackfruit: cdev_add failed: %d\n", ret);
        unregister_chrdev_region(dev, 1);
        return ret;
    }

    /* Create /sys/class/jackfruit and /dev/container_monitor */
    g_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(g_class)) {
        ret = PTR_ERR(g_class);
        pr_err("jackfruit: class_create failed: %d\n", ret);
        cdev_del(&g_cdev);
        unregister_chrdev_region(dev, 1);
        return ret;
    }

    g_device = device_create(g_class, NULL, dev, NULL, DEVICE_NAME);
    if (IS_ERR(g_device)) {
        ret = PTR_ERR(g_device);
        pr_err("jackfruit: device_create failed: %d\n", ret);
        class_destroy(g_class);
        cdev_del(&g_cdev);
        unregister_chrdev_region(dev, 1);
        return ret;
    }

    /* Start the periodic polling work */
    INIT_DELAYED_WORK(&g_poll_work, poll_rss);
    schedule_delayed_work(&g_poll_work,
                          msecs_to_jiffies(poll_interval_ms));

    pr_info("jackfruit: monitor loaded — /dev/%s (major=%d), "
            "poll_interval=%ums\n",
            DEVICE_NAME, g_major, poll_interval_ms);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct container_entry *entry, *tmp;
    dev_t dev = MKDEV(g_major, 0);

    /* Stop polling */
    cancel_delayed_work_sync(&g_poll_work);

    /* Free every entry in the list */
    mutex_lock(&g_list_lock);
    list_for_each_entry_safe(entry, tmp, &g_container_list, node) {
        pr_info("jackfruit: freeing entry for container '%s' pid=%d\n",
                entry->name, entry->pid);
        list_del(&entry->node);
        kfree(entry);
    }
    mutex_unlock(&g_list_lock);

    /* Tear down device */
    device_destroy(g_class, dev);
    class_destroy(g_class);
    cdev_del(&g_cdev);
    unregister_chrdev_region(dev, 1);

    pr_info("jackfruit: monitor unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
