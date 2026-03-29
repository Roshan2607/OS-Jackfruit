#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include "monitor_ioctl.h"

/* TODO 1: Linked-list node struct */
struct container_node {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit;
    unsigned long hard_limit;
    bool soft_warned;
    struct list_head list;
};

/* TODO 2: Global list and Lock */
static LIST_HEAD(container_list);
static DEFINE_MUTEX(monitor_lock);

static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

static long get_rss_bytes(pid_t pid) {
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) { rcu_read_unlock(); return -1; }
    get_task_struct(task);
    rcu_read_unlock();
    mm = get_task_mm(task);
    if (mm) { rss_pages = get_mm_rss(mm); mmput(mm); }
    put_task_struct(task);
    return rss_pages * PAGE_SIZE;
}

static void log_soft_limit_event(const char *id, pid_t pid, unsigned long lim, long rss) {
    printk(KERN_WARNING "[monitor] SOFT LIMIT: %s (PID %d) RSS %ld > %lu\n", id, pid, rss, lim);
}

static void kill_process(const char *id, pid_t pid, unsigned long lim, long rss) {
    struct task_struct *task;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) send_sig(SIGKILL, task, 1);
    rcu_read_unlock();
    printk(KERN_ERR "[monitor] HARD LIMIT KILL: %s (PID %d) RSS %ld > %lu\n", id, pid, rss, lim);
}

/* TODO 3: Periodic Monitoring */
static void timer_callback(struct timer_list *t) {
    struct container_node *entry, *tmp;
    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        long rss = get_rss_bytes(entry->pid);
        if (rss < 0) { // Process exited
            list_del(&entry->list);
            kfree(entry);
            continue;
        }
        if (rss > entry->hard_limit) {
            kill_process(entry->container_id, entry->pid, entry->hard_limit, rss);
            list_del(&entry->list);
            kfree(entry);
        } else if (rss > entry->soft_limit && !entry->soft_warned) {
            log_soft_limit_event(entry->container_id, entry->pid, entry->soft_limit, rss);
            entry->soft_warned = true;
        }
    }
    mutex_unlock(&monitor_lock);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}

/* TODO 4 & 5: IOCTL Registration */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    struct monitor_request req;
    struct container_node *entry, *tmp;
    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req))) return -EFAULT;

    mutex_lock(&monitor_lock);
    if (cmd == MONITOR_REGISTER) {
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        entry->pid = req.pid;
        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;
        entry->soft_warned = false;
        strncpy(entry->container_id, req.container_id, MONITOR_NAME_LEN);
        list_add(&entry->list, &container_list);
    } else if (cmd == MONITOR_UNREGISTER) {
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
            }
        }
    }
    mutex_unlock(&monitor_lock);
    return 0;
}

static struct file_operations fops = { .owner = THIS_MODULE, .unlocked_ioctl = monitor_ioctl };

static int __init monitor_init(void) {
    alloc_chrdev_region(&dev_num, 0, 1, "container_monitor");
    cl = class_create("container_monitor");
    device_create(cl, NULL, dev_num, NULL, "container_monitor");
    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, dev_num, 1);
    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
    return 0;
}

static void __exit monitor_exit(void) {
    struct container_node *entry, *tmp;
    del_timer_sync(&monitor_timer);
    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitor_lock);
    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
