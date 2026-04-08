#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h> // This contains del_timer
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CLASS_NAME  "monitor"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit Team");
MODULE_DESCRIPTION("Kernel module for container memory monitoring and enforcement");

static int major_number;
static struct class* monitor_class  = NULL;
static struct device* monitor_device = NULL;

struct container_node {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit;
    unsigned long hard_limit;
    struct list_head list;
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_lock);
static struct timer_list monitor_timer;

static void monitor_timer_callback(struct timer_list *t) {
    struct container_node *entry, *tmp;
    struct task_struct *task;
    unsigned long rss;

    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
        if (!task) {
            printk(KERN_INFO "[container_monitor] container=%s pid=%d exited, removing.\n", 
                   entry->container_id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (task->mm) {
            rss = get_mm_rss(task->mm) << PAGE_SHIFT;

            if (entry->hard_limit > 0 && rss > entry->hard_limit) {
                printk(KERN_INFO "[container_monitor] HARD LIMIT container=%s pid=%d rss=%lu limit=%lu\n",
                       entry->container_id, entry->pid, rss, entry->hard_limit);
                send_sig(SIGKILL, task, 1);
            } else if (entry->soft_limit > 0 && rss > entry->soft_limit) {
                printk(KERN_INFO "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%lu limit=%lu\n",
                       entry->container_id, entry->pid, rss, entry->soft_limit);
            }
        }
    }
    mutex_unlock(&list_lock);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct monitor_request req;
    struct container_node *new_node;

    if (cmd == MONITOR_REGISTER) {
        if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
            return -EFAULT;

        new_node = kmalloc(sizeof(struct container_node), GFP_KERNEL);
        if (!new_node) return -ENOMEM;

        new_node->pid = req.pid;
        new_node->soft_limit = req.soft_limit_bytes;
        new_node->hard_limit = req.hard_limit_bytes;
        
        memset(new_node->container_id, 0, MONITOR_NAME_LEN);
        strncpy(new_node->container_id, req.container_id, MONITOR_NAME_LEN - 1);

        mutex_lock(&list_lock);
        list_add(&new_node->list, &container_list);
        mutex_unlock(&list_lock);

        printk(KERN_INFO "[container_monitor] Registered %s (PID %d)\n", req.container_id, req.pid);
    }
    return 0;
}

static const struct file_operations fops = {
    .unlocked_ioctl = monitor_ioctl,
    .owner = THIS_MODULE,
};

static int __init monitor_init(void) {
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) return major_number;

    monitor_class = class_create(CLASS_NAME);
    if (IS_ERR(monitor_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(monitor_class);
    }

    monitor_device = device_create(monitor_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(monitor_device)) {
        class_destroy(monitor_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(monitor_device);
    }
    
    timer_setup(&monitor_timer, monitor_timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));

    printk(KERN_INFO "[container_monitor] Module loaded\n");
    return 0;
}

static void __exit monitor_exit(void) {
    struct container_node *entry, *tmp;
    
    // We use del_timer_sync to ensure the timer is actually stopped before unloading
    // If your kernel complains about del_timer, del_timer_sync is the standard alternative
    timer_shutdown_sync(&monitor_timer); 

    device_destroy(monitor_class, MKDEV(major_number, 0));
    class_unregister(monitor_class);
    class_destroy(monitor_class);
    unregister_chrdev(major_number, DEVICE_NAME);

    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_lock);
    printk(KERN_INFO "[container_monitor] Module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
