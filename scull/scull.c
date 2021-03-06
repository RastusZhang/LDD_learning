#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#include "scull.h"

/*
 * Our parameters which can be set at load time.
 */

MODULE_LICENSE("GPL");

int scull_major = SCULL_MAJOR;
int scull_minor=0;
int scull_nr_devs = SCULL_NR_DEVS; // number of bare scull devices
int scull_quantum = SCULL_QUANTUM;
int scull_qset = SCULL_QSET;

struct scull_dev *scull_devices;    /* allocated in scull_init_module */

/*
 * Empty out the scull device; must be called with the device 
 * semaphore held.
 */
int scull_trim(struct scull_dev *dev)
{
    struct scull_qset *next, *dptr;
    int qset = dev->qset;   /* "dev" is not-null */
    int i;
    
    for(dptr = dev->data; dptr; dptr = next) {
        if(dptr->data) {
            for (i = 0; i < qset; i++)
                kfree(dptr->data[i]);
            kfree(dptr->data);
            dptr->data = NULL;
        }
        next = dptr->next;
        kfree(dptr);
    }
    dev->size = 0;
    dev->quantum = scull_quantum;
    dev->qset = scull_qset;
    dev->data = NULL;
    return 0;
}

int scull_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev;  /* device information */

    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev;   /* for other methods */

    /* now trim to o the lenght of the device if open was write-only */
    if((filp->f_flags & O_ACCMODE) == O_WRONLY){
        scull_trim(dev);    /* ignore errors */
    }
    return 0;
}

int scull_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * Follow the list
 */
struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
    struct scull_qset *qs = dev->data;

    /* Allocate first qset explicitly if need be */
    if(!qs) {
        qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
        if (qs == NULL)
            return NULL;    /* Never mind */
        memset(qs, 0, sizeof(struct scull_qset));
    }

    /* Then follow the list */
    while (n--) {
        if (!qs->next) {
            qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
            if (qs->next == NULL)
                return NULL;    /* Never mind */
            memset(qs->next, 0, sizeof(struct scull_qset));
        }
        qs = qs->next;
        continue;
    }
    return qs;
}

ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;    /* 第一个链表项 */
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;  /* 该链表中有多少个字节 */
    int item, s_pos, q_pos, rest;   
    ssize_t retval = 0;

    if(mutex_lock_interruptible(&dev->mutex))
        return -ERESTARTSYS;
    if(*f_pos >= dev->size)
        goto out;
    if(*f_pos + count > dev->size)
        count = dev->size - *f_pos;

    /* 在量子集中寻找链表项、qset索引以及偏移量 */
    item = (long) *f_pos / itemsize;
    rest = (long) *f_pos % itemsize;
    s_pos = rest / quantum; q_pos = rest % quantum;

    /* 沿该链表前行，直到正确的位置（在其他地方定义）*/
    dptr = scull_follow(dev, item);

    if(dptr == NULL || !dptr->data || !dptr->data[s_pos])
        goto out;   /* don't fill holes */

    /* 读取该量子的数据直到结尾 */
    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

out:
    mutex_unlock(&dev->mutex);
    return retval;
}

ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;   /* “goto out” 语句使用的值 */

    if (mutex_lock_interruptible(&dev->mutex))
        return -ERESTARTSYS;

    /* 在量子集中寻找链表项、qset索引以及偏移量 */
    item = (long) *f_pos / itemsize;
    rest = (long) *f_pos % itemsize;
    s_pos = rest / quantum; q_pos = rest % quantum;

    /* 沿该链表前行，直到正确的位置（在其他地方定义）*/
    dptr = scull_follow(dev, item);
    if (dptr == NULL)
        goto out;
    if (!dptr->data) {
        dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
        if (!dptr->data)
            goto out;
        memset(dptr->data, 0, qset * sizeof(char *));
    }
    if (!dptr->data[s_pos]) {
        dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
        if (!dptr->data[s_pos])
            goto out;
    }
    /* 将数据写入该量子，直到结尾*/
    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_from_user(dptr->data[s_pos]+q_pos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

    /* 更新文件大小 */
    if (dev->size < *f_pos)
        dev->size = *f_pos;

out:
    mutex_unlock(&dev->mutex);
    return retval;
}

struct file_operations scull_fops = {
    .owner = THIS_MODULE,
    .read = scull_read,
    .write = scull_write,
    .open = scull_open,
    .release = scull_release,
};

/*
 * The cleanup function is used to handle initialization failures as well.
 * Thefore, it must be careful to work correctly even if some of the items
 * have not been initialized.
 */
void scull_cleanup_module(void)
{
    int i;
    dev_t devno = MKDEV(scull_major, scull_minor);

    /* Get rid of our char dev entries */
    if (scull_devices){
        for(i = 0; i < scull_nr_devs; i++) {
            scull_trim(scull_devices + i);
            cdev_del(&scull_devices[i].cdev);
        }
        kfree(scull_devices);
    }

    /* cleanup_module is never called if registering failed */
    unregister_chrdev_region(devno, scull_nr_devs);

    /* and call the cleanup functions for friend devices */
}


/* 
 * Set up the char_dev structure for this device.
 */
static void scull_setup_cdev(struct scull_dev *dev, int index)
{
    int err, devno = MKDEV(scull_major, scull_minor + index);

    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &scull_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    /* Fail gracefully if need be */
    if(err)
        printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

static int scull_init(void)
{
    int result, i;
    dev_t dev = 0;

    /*
     * Get a range of minor numbers to work with, asking for a dynamic
     * major unless directed otherwise at load time.
     */
    if(scull_major){
        dev = MKDEV(scull_major, scull_minor);
        result = register_chrdev_region(dev, scull_nr_devs, "scull");
    }
    else{
        result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, "scull");
        scull_major = MAJOR(dev);
    }
    if(result<0){
        printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
        return result;
    }

    /*
     * allocate the devices -- we can't have them static, as the number
     * can be specified at load time.
     */
    scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
    if(!scull_devices){
        result = -ENOMEM;
        goto fail;  /* Make this more graceful */
    }
    memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));

    /* Initialize each device. */
    for (i=0; i<scull_nr_devs; i++){
        scull_devices[i].quantum = scull_quantum;
        scull_devices[i].qset = scull_qset;
        mutex_init(&scull_devices[i].mutex);
        scull_setup_cdev(&scull_devices[i], i);
    }

    return 0; /* succeed */

fail:
    scull_cleanup_module();
    return result;
}

module_init(scull_init);
module_exit(scull_cleanup_module);

MODULE_AUTHOR("Qomo Liao");
MODULE_DESCRIPTION("Simple character Utility for Loading Localities.\n");
