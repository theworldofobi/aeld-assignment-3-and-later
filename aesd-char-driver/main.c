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
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include "aesdchar.h"
#include "aesd_ioctl.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Obi Obetta");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

static size_t aesd_total_size(struct aesd_dev *dev)
{
    struct aesd_buffer_entry *entry;
    size_t total = 0;
    uint8_t index;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        total += entry->size;
    }
    return total;
}

static loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t retval;

    PDEBUG("llseek offset %lld whence %d",offset,whence);
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;
    retval = fixed_size_llseek(filp, offset, whence, aesd_total_size(dev));
    mutex_unlock(&dev->lock);
    return retval;
}

static int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    filp->private_data = container_of(inode->i_cdev, struct aesd_dev, cdev);
    return 0;
}

static int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

static ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;
    size_t available;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);
    if (!entry)
        goto out;

    available = entry->size - entry_offset;
    if (count > available)
        count = available;

    if (copy_to_user(buf, entry->buffptr + entry_offset, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

static ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *newbuf;

    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    if (count == 0)
        return 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    newbuf = krealloc(dev->working.buffptr, dev->working.size + count, GFP_KERNEL);
    if (!newbuf)
        goto out;
    dev->working.buffptr = newbuf;

    if (copy_from_user(newbuf + dev->working.size, buf, count)) {
        retval = -EFAULT;
        goto out;
    }

    if (memchr(newbuf + dev->working.size, '\n', count)) {
        dev->working.size += count;
        if (dev->buffer.full)
            kfree(dev->buffer.entry[dev->buffer.in_offs].buffptr);
        aesd_circular_buffer_add_entry(&dev->buffer, &dev->working);
        dev->working.buffptr = NULL;
        dev->working.size = 0;
    } else {
        dev->working.size += count;
    }
    retval = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}


static long aesd_adjust_file_offset(struct file *filp, unsigned int write_cmd,
                unsigned int write_cmd_offset)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_circular_buffer *buffer = &dev->buffer;
    loff_t pos = 0;
    long retval = -EINVAL;
    uint8_t count, idx;
    unsigned int i;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (buffer->full)
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    else
        count = (buffer->in_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - buffer->out_offs)
                    % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    if (write_cmd >= count)
        goto out;

    idx = buffer->out_offs;
    for (i = 0; i < write_cmd; i++) {
        pos += buffer->entry[idx].size;
        idx = (idx + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    if (write_cmd_offset >= buffer->entry[idx].size)
        goto out;

    filp->f_pos = pos + write_cmd_offset;
    retval = 0;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_seekto seekto;

    PDEBUG("ioctl cmd %u",cmd);

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC)
        return -ENOTTY;
    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR)
        return -ENOTTY;

    switch (cmd) {
    case AESDCHAR_IOCSEEKTO:
        if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)))
            return -EFAULT;
        return aesd_adjust_file_offset(filp, seekto.write_cmd, seekto.write_cmd_offset);
    default:
        return -ENOTTY;
    }
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .llseek =   aesd_llseek,
    .read =     aesd_read,
    .write =    aesd_write,
    .unlocked_ioctl = aesd_ioctl,
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



static int aesd_init_module(void)
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

    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        mutex_destroy(&aesd_device.lock);
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

static void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t index;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        kfree(entry->buffptr);
    }
    kfree(aesd_device.working.buffptr);
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
