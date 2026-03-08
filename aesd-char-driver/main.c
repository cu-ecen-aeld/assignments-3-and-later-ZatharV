/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("ZatharV"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *dev;
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset = 0;
    size_t bytes_to_copy;
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer,
                                                             *f_pos,
                                                             &entry_offset);
    if (entry == NULL) {
        /* No data available at this offset */
        retval = 0;
        goto out;
    }

    /* How many bytes available in this entry from entry_offset */
    bytes_to_copy = entry->size - entry_offset;

    /* Limit to what caller requested */
    if (bytes_to_copy > count)
        bytes_to_copy = count;

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */
    struct aesd_dev *dev = filp->private_data;
    char *kernel_buf = NULL;
    char *newline_pos;
    char *new_partial;

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

    /* Append to partial write buffer */
    new_partial = krealloc(dev->partial_write_buf,
                           dev->partial_write_size + count,
                           GFP_KERNEL);
    if (!new_partial) {
        kfree(kernel_buf);
        retval = -ENOMEM;
        goto out2;
    }

    dev->partial_write_buf = new_partial;
    memcpy(dev->partial_write_buf + dev->partial_write_size, kernel_buf, count);
    dev->partial_write_size += count;
    kfree(kernel_buf);
    kernel_buf = NULL;

    /* Check if we have a complete command (terminated by \n) */
    newline_pos = memchr(dev->partial_write_buf, '\n', dev->partial_write_size);

    while (newline_pos != NULL) {
        size_t cmd_len = newline_pos - dev->partial_write_buf + 1; /* include \n */
        struct aesd_buffer_entry new_entry;
        char *cmd_buf;

        cmd_buf = kmalloc(cmd_len, GFP_KERNEL);
        if (!cmd_buf) {
            retval = -ENOMEM;
            goto out2;
        }
        memcpy(cmd_buf, dev->partial_write_buf, cmd_len);

        new_entry.buffptr = cmd_buf;
        new_entry.size = cmd_len;

        /* Save the buffptr of the entry about to be evicted BEFORE add overwrites it */
        {
            const char *to_free = NULL;
            if (dev->buffer.full)
                to_free = dev->buffer.entry[dev->buffer.in_offs].buffptr;

            aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);

            if (to_free)
                kfree((void *)to_free);
        }

        /* Remove the consumed command from partial buffer */
        size_t remaining = dev->partial_write_size - cmd_len;
        if (remaining > 0) {
            memmove(dev->partial_write_buf,
                    dev->partial_write_buf + cmd_len,
                    remaining);
        }
        dev->partial_write_size = remaining;

        /* Check for another \n in the remaining data */
        newline_pos = memchr(dev->partial_write_buf, '\n', dev->partial_write_size);
    }

    retval = count;

out2:
    mutex_unlock(&dev->lock);
    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */

    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);
    aesd_device.partial_write_buf = NULL;
    aesd_device.partial_write_size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    uint8_t index;
    struct aesd_buffer_entry *entry;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree((void *)entry->buffptr);
            entry->buffptr = NULL;
        }
    }

    /* Free any partial write buffer */
    if (aesd_device.partial_write_buf) {
        kfree(aesd_device.partial_write_buf);
        aesd_device.partial_write_buf = NULL;
    }

    mutex_destroy(&aesd_device.lock);
    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
