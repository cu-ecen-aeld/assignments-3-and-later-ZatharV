/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes / ZatharV
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
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

int aesd_major = 0; /* use dynamic major */
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

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *dev;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;
    size_t bytes_to_copy;
    size_t total_copied;
    ssize_t retval;
    loff_t cur_pos;
    char __user *cur_user;

    dev = filp->private_data;
    entry = NULL;
    entry_offset = 0;
    bytes_to_copy = 0;
    total_copied = 0;
    retval = 0;
    cur_pos = *f_pos;
    cur_user = buf;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /*
     * Read may span multiple circular buffer entries.
     * We loop until we either satisfy 'count' bytes or run out of data.
     */
    while (total_copied < count) {
        entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                    &dev->buffer, cur_pos, &entry_offset);

        if (entry == NULL) {
            /* No more data available from this position */
            break;
        }

        bytes_to_copy = entry->size - entry_offset;
        if (bytes_to_copy > (count - total_copied))
            bytes_to_copy = count - total_copied;

        if (copy_to_user(cur_user,
                         entry->buffptr + entry_offset,
                         bytes_to_copy)) {
            retval = -EFAULT;
            goto out;
        }

        cur_pos    += bytes_to_copy;
        cur_user   += bytes_to_copy;
        total_copied += bytes_to_copy;
    }

    *f_pos = cur_pos;
    retval = total_copied;

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

    /* Append incoming data to the partial write buffer */
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

    /*
     * Process all complete commands (each terminated by \n).
     * A single write may contain multiple \n-terminated commands.
     */
    newline_pos = memchr(dev->partial_write_buf, '\n', dev->partial_write_size);

    while (newline_pos != NULL) {
        size_t cmd_len;
        size_t remaining;
        char *cmd_buf;
        const char *to_free;
        struct aesd_buffer_entry new_entry;

        cmd_len  = (size_t)(newline_pos - dev->partial_write_buf) + 1; /* include \n */

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

        /* Shift remaining partial data to front of buffer */
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

        /* Check for another complete command in the remaining data */
        newline_pos = (dev->partial_write_size > 0) ?
                      memchr(dev->partial_write_buf, '\n', dev->partial_write_size) :
                      NULL;
    }

    /*
     * Whether or not we had a \n, we consumed all 'count' bytes from userspace.
     * Partial (no \n) data is buffered and will be flushed on the next write
     * that completes the command.
     */
    retval = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

/**
 * llseek: Allow seeking so drivertest.sh can lseek to 0 after writing
 * and read back from the beginning. Only SEEK_SET and SEEK_CUR are supported.
 * SEEK_END is not meaningful for this device.
 */
loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    loff_t new_pos;
    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = filp->f_pos + offset;
            break;
        default:
            return -EINVAL;
    }
    if (new_pos < 0)
        return -EINVAL;
    filp->f_pos = new_pos;
    return new_pos;
}

struct file_operations aesd_fops = {
    .owner   = THIS_MODULE,
    .llseek  = aesd_llseek,
    .read    = aesd_read,
    .write   = aesd_write,
    .open    = aesd_open,
    .release = aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err;
    int devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops   = &aesd_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
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
    if (result) {
        unregister_chrdev_region(dev, 1);
    }
    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    /* Free all kmalloc'd buffptrs stored in the circular buffer */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree((void *)entry->buffptr);
            entry->buffptr = NULL;
        }
    }

    /* Free any partial (unterminated) write buffer */
    if (aesd_device.partial_write_buf) {
        kfree(aesd_device.partial_write_buf);
        aesd_device.partial_write_buf = NULL;
    }

    mutex_destroy(&aesd_device.lock);
    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
