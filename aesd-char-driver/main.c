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
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Daniel A. Winston");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    
    // Grab a pointer to the char device
    struct aesd_dev *dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    // Assign file pointer
    filp->private_data = dev;
    
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    // Nothing to do here, filp is assigned a pointer to device loaded in kernel
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry = NULL;
    size_t entry_offset = 0;
    
    if (count == 0)
        return 0;
    
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    
    // Obtain a lock
    if (mutex_lock_interruptible(&dev->lock) != 0)
        return -ERESTARTSYS;
    
    // Find read position based on f_pos
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);
    // NULL or EOF
    if (!entry)
        goto out;
    
    // Copy partial read to user space
    size_t bytes_unread = entry->size - entry_offset;
    if (count > bytes_unread)
        count = bytes_unread;
    
    if (copy_to_user(buf, entry->buffptr + entry_offset, count))
    {
        retval = -EFAULT;
        goto out;
    }
    
    // Update f_pos
    *f_pos += count;
    retval = count;
    
    // Release lock and cleanup
out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char* buffer = NULL;
    bool locked = false;
    bool end = false;
    
    if (count == 0)
        return 0;
    
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    
    // Allocate a buffer
    buffer = kmalloc(count, GFP_KERNEL);
    if (!buffer)
        return retval;
    
    // Copy from userspace, returns number of bytes that cannot be copied
    if (copy_from_user(buffer, buf, count))
    {
        retval = -EFAULT;
        goto out;
    }
    
    // Obtain a lock
    if (mutex_lock_interruptible(&dev->lock) != 0)
    {
        retval = -ERESTARTSYS;
        goto out;
    }
    locked = true;
    
    // Allocate a bigger buffer to handle the new length
    char *bigger_buffer = krealloc(dev->entry.buffptr, dev->entry.size + count, GFP_KERNEL);
    if (!bigger_buffer)
    {
        goto out;
    }
    
    // Assign the new buffer to the device
    dev->entry.buffptr = bigger_buffer;
    memcpy(dev->entry.buffptr + dev->entry.size, buffer, count);
    dev->entry.size += count;
    
    // Scan for newline
    for (size_t i = 0; i < count; i++)
        if (buffer[i] == '\n')
        {
            end = true;
            break;
        }
    
    if (end)
    {
        if (dev->buffer.full)
            kfree(dev->buffer.entry[dev->buffer.in_offs].buffptr);
        
        aesd_circular_buffer_add_entry(&dev->buffer, &dev->entry);
        
        dev->entry.buffptr = NULL;
        dev->entry.size = 0;
    }
    
    retval = count;

out:
    if (locked)
        mutex_unlock(&dev->lock);
    kfree(buffer);
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
    
    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t index;

    cdev_del(&aesd_device.cdev);

    // Free the circular buffer
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index)
    {
        if (entry->buffptr)
            kfree(entry->buffptr);
    }
    
    // Free the current entry
    if (aesd_device.entry.buffptr)
        kfree(aesd_device.entry.buffptr);
            
    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
