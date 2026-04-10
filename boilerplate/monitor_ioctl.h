#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#define MONITOR_NAME_LEN 32

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitored_entry *entry, *tmp;
    struct monitor_request req;
    (void)f;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    switch (cmd) {
    case MONITOR_REGISTER:
        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid             = req.pid;
        entry->soft_limit      = req.soft_limit_bytes;
        entry->hard_limit      = req.hard_limit_bytes;
        strncpy(entry->container_id, req.container_id, MONITOR_NAME_LEN - 1);
        entry->container_id[MONITOR_NAME_LEN - 1] = '\0';

        mutex_lock(&monitor_lock);
        list_add_tail(&entry->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        pr_info("monitor: registered pid %d (container: %s)\n",
                req.pid, entry->container_id);
        return 0;

    case MONITOR_UNREGISTER:
        mutex_lock(&monitor_lock);
        list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                mutex_unlock(&monitor_lock);
                pr_info("monitor: unregistered pid %d\n", req.pid);
                return 0;
            }
        }
        mutex_unlock(&monitor_lock);
        return -ENOENT;

    default:
        return -ENOTTY;
    }
}

struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[MONITOR_NAME_LEN];
};

#define MONITOR_MAGIC 'M'
#define MONITOR_REGISTER _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

#endif
