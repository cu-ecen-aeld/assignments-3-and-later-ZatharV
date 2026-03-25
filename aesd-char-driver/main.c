/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 * Assignment 9: Added aesd_llseek (SEEK_END) and aesd_ioctl (AESDCHAR_IOCSEEKTO)
 *
 * @author Dan Walkes / ZatharV
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "aesdchar.h"
#include "aesd_ioctl.h"

int aesd_major = 0;
int aesd_minor = 0;

MODULE_AUTHOR("ZatharV");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

/**
 * Helper: compute total bytes stored across all valid entries.
 * Caller must hold dev->lock.
 */
static loff_t aesd_total_size(struct aesd_dev *dev)
{
    struct aesd_buffer_entry *entry;
    loff_t total = 0;
    uint8_t idx;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, idx) {
        if (entry->buffptr)
            total += (loff_t)entry->size;
    }
    return total;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset = 0;
    size_t bytes_to_copy;
    size_t copied = 0;
    loff_t pos = *f_pos;
    ssize_t retval = 0;
    loff_t total_size;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    total_size = aesd_total_size(dev);

    if (pos >= total_size) {
        retval = 0;
        goto out;
    }

    while (copied < count) {
        entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                    &dev->buffer, (size_t)pos, &entry_offset);
        if (!entry)
            break;

        bytes_to_copy = entry->size - entry_offset;
        if (bytes_to_copy > (count - copied))
            bytes_to_copy = count - copied;

        if (copy_to_user(buf + copied,
                         entry->buffptr + entry_offset,
                         bytes_to_copy)) {
            retval = -EFAULT;
            goto out;
        }

        copied += bytes_to_copy;
        pos    += bytes_to_copy;
    }

    *f_pos = pos;
    retval = copied;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                   loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *kernel_buf  = NULL;
    char *new_partial = NULL;
    char *newline_pos = NULL;
    ssize_t retval    = -ENOMEM;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    kernel_buf = kmalloc(count, GFP_KERNEL);
    if (!kernel_buf)
        return -ENOMEM;

    if (copy_from_user(kernel_buf, buf, count)) {
        kfree(kernel_buf);
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(kernel_buf);
        return -ERESTARTSYS;
    }

    new_partial = krealloc(dev->partial_write_buf,
                           dev->partial_write_size + count,
                           GFP_KERNEL);
    if (!new_partial) {
        kfree(kernel_buf);
        retval = -ENOMEM;
        goto out;
    }

    dev->partial_write_buf = new_partial;
    memcpy(dev->partial_write_buf + dev->partial_write_size, kernel_buf, count);
    dev->partial_write_size += count;
    kfree(kernel_buf);
    kernel_buf = NULL;

    newline_pos = memchr(dev->partial_write_buf, '\n', dev->partial_write_size);

    while (newline_pos != NULL) {
        size_t cmd_len;
        size_t remaining;
        char *cmd_buf;
        const char *to_free;
        struct aesd_buffer_entry new_entry;

        cmd_len = (size_t)(newline_pos - dev->partial_write_buf) + 1;

        cmd_buf = kmalloc(cmd_len, GFP_KERNEL);
        if (!cmd_buf) {
            retval = -ENOMEM;
            goto out;
        }
        memcpy(cmd_buf, dev->partial_write_buf, cmd_len);

        new_entry.buffptr = cmd_buf;
        new_entry.size    = cmd_len;

        to_free = aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);
        if (to_free)
            kfree((void *)to_free);

        remaining = dev->partial_write_size - cmd_len;
        if (remaining > 0) {
            memmove(dev->partial_write_buf,
                    dev->partial_write_buf + cmd_len,
                    remaining);
            dev->partial_write_size = remaining;
        } else {
            kfree(dev->partial_write_buf);
            dev->partial_write_buf  = NULL;
            dev->partial_write_size = 0;
        }

        newline_pos = (dev->partial_write_size > 0) ?
                      memchr(dev->partial_write_buf, '\n', dev->partial_write_size) :
                      NULL;
    }

    retval = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

/**
 * aesd_llseek - Assignment 9: full SEEK_SET, SEEK_CUR, SEEK_END support
 */
loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t new_pos = -1;
    loff_t total;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    total = aesd_total_size(dev);

    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = filp->f_pos + offset;
            break;
        case SEEK_END:
            new_pos = total + offset;
            break;
        default:
            mutex_unlock(&dev->lock);
            return -EINVAL;
    }

    if (new_pos < 0) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = new_pos;
    mutex_unlock(&dev->lock);
    return new_pos;
}

/**
 * aesd_ioctl - Assignment 9: AESDCHAR_IOCSEEKTO
 *
 * Takes struct aesd_seekto { write_cmd, write_cmd_offset } from userspace.
 * Computes the absolute byte offset in the circular buffer and sets f_pos.
 */
long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    loff_t new_pos = 0;
    uint8_t i;
    uint8_t count;
    uint8_t idx;

    PDEBUG("ioctl cmd=%u", cmd);

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC)
        return -ENOTTY;
    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR)
        return -ENOTTY;

    switch (cmd) {
        case AESDCHAR_IOCSEEKTO:
            if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)))
                return -EFAULT;

            if (mutex_lock_interruptible(&dev->lock))
                return -ERESTARTSYS;

            /* How many entries are currently in the buffer? */
            if (dev->buffer.full) {
                count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
            } else if (dev->buffer.in_offs >= dev->buffer.out_offs) {
                count = dev->buffer.in_offs - dev->buffer.out_offs;
            } else {
                count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED
                        - dev->buffer.out_offs
                        + dev->buffer.in_offs;
            }

            /* write_cmd must be a valid entry index */
            if (seekto.write_cmd >= count) {
                mutex_unlock(&dev->lock);
                return -EINVAL;
            }

            /* Sum sizes of all entries before write_cmd */
            new_pos = 0;
            for (i = 0; i < seekto.write_cmd; i++) {
                idx = (dev->buffer.out_offs + i)
                      % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
                new_pos += (loff_t)dev->buffer.entry[idx].size;
            }

            /* write_cmd_offset must be within the target entry */
            idx = (dev->buffer.out_offs + seekto.write_cmd)
                  % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

            if (seekto.write_cmd_offset >= dev->buffer.entry[idx].size) {
                mutex_unlock(&dev->lock);
                return -EINVAL;
            }

            new_pos += (loff_t)seekto.write_cmd_offset;
            filp->f_pos = new_pos;
            mutex_unlock(&dev->lock);

            PDEBUG("IOCSEEKTO: cmd=%u off=%u -> f_pos=%lld",
                   seekto.write_cmd, seekto.write_cmd_offset, new_pos);
            return 0;

        default:
            return -ENOTTY;
    }
}

struct file_operations aesd_fops = {
    .owner          = THIS_MODULE,
    .llseek         = aesd_llseek,
    .read           = aesd_read,
    .write          = aesd_write,
    .open           = aesd_open,
    .release        = aesd_release,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err;
    int devno = MKDEV(aesd_major, aesd_minor);
    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops   = &aesd_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }

    memset(&aesd_device, 0, sizeof(struct aesd_dev));
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);
    aesd_device.partial_write_buf  = NULL;
    aesd_device.partial_write_size = 0;

    result = aesd_setup_cdev(&aesd_device);
    if (result)
        unregister_chrdev_region(dev, 1);
    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree((void *)entry->buffptr);
            entry->buffptr = NULL;
        }
    }

    if (aesd_device.partial_write_buf) {
        kfree(aesd_device.partial_write_buf);
        aesd_device.partial_write_buf = NULL;
    }

    mutex_destroy(&aesd_device.lock);
    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);