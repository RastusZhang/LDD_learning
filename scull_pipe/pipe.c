/*************************************************************************
    > File Name: pipe.c
    > Author: 
    > Mail: 
    >i Created Time: 2015年02月09日 星期一 17时02分23秒
 ************************************************************************/

#include "scull.h"
#include <linux/cdev.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/fcntl.h>

struct scull_pipe {
    wait_queue_head_t inq, outq;        /* read and write queues */
    char *buffer, *end;                 /* begin of buf, end of buf */
    int buffersize;                     /* used in pointer arithmetic */
    char *rp, *wp;                      /* where to read, where to write */
    int nreaers, nwriters;              /* number of openings for r/w */
    struct fasync_struct *async_queue;  /* asynchronous readers */
    struct mutex mutex;                 /* mutual exclusion semaphore */
    struct cdev cdev;
}

/* parameters */
static scull_p_nr_devs = SCULL_P_NR_DEVS;   /* number of pipe devices */

/*
 * The file operations for the pipe device
 * (some are overlayed with bare scull)
 */
struct file_operations scull_pipe_fops = {
    .owner =    THIS_MODULE,
//    .llseek =   no_llseek,
    .read =     scull_p_read,
    .write =    scull_p_write,
//    .poll =     scull_p_poll,
//    .unlocked_ioctl =   scull_ioctl,
//    .open =     scull_p_open,
//    .release =  scull_p_release,
//    .fasync =   scull_p_fasync,
};

/*
 * Data managment: read and write
 */
static ssize_t scull_p_read(struct file *filp, char __user *buf, size_t count, loff_t *f_ops)
{
    struct scull_pipe *dev = filp->private_data;

    if (mutex_lock_interruptible(&dev->mutex))
        return -ERESTARTSYS;

    while (dev->rp == dev->wp) { // nothing to read
        mutex_unlock(&dev->mutex); // release the lock
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        PDEBUG("\"%s\" reading: going to sleep\n", current->comm);
        if (wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))
            return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
        /* otherwise loop, but first reacquire the lock */
        if (mutex_lock_interruptible(&dev->mutex))
            return -ERESTARTSYS;
    }
    /* ok, data is there, return something */
    if (dev->wp > dev->rp)
        count = min(count, (size_t)(dev->wp - dev->rp));
    else /* the write pointer has wrapped, return data up to dev->end */
        count = min(count, (size_t)(dev->end - dev->rp));
    if (copy_to_user(buf, dev->rp, count)) {
        mutex_unlock(&dev->mutex);
        return -EFAULT;
    }
    dev->rp += count;
    if (dev->rp == dev->end)
        dev->rp = dev->buffer; /* wrapped */
    mutex_unlock(&dev->mutex);

    /* finally, awake any writers and return */
    wake_up_interruptible(&dev->outq);
    PDEBUG("\"%s\" did read %li bytes\n", current->comm, (long)count);
    return count;
}

/* Wait for space for writing; caller must hold device semaphore. On
 * error the semaphore will be release before returning. */
static int scull_getwritespace(struct scull_pipe *dev, struct file *filp)
{
    while (spacefree(dev) == 0) { // full
        DEFINE_WAIT(wait);

        mutex_unlock(&dev->mutex);
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        PDEBUG("\"%s\" writing: going to sleep\n", current->comm);
        prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
        if (spacefree(dev) == 0)
            schedule();
        finish_wait(&dev->outq, &wait);
        if (signal_pending(current))
            return -ERESTARTSYS;    /* signal: tell the fs layer to handle */
        if (mutex_lock_interruptible(&dev->mutex))
            return -ERESTARTSYS;
    }
    return 0;
}

/* How much space is free? */
static int spacefree(struct scull_pipe *dev)
{
    if (dev->rp == dev->wp)
        return dev->buffersize - 1;
    return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

static ssize_t scull_p_write(struct file *filp, char __user *buf, size_t count, loff_t *f_ops)
{
    struct scull_pipe *dev = filp->private_data;
    int result;

    if (mutex_lock_interruptible(&dev->mutex))
        return -ERESTARTSYS;
    
    /* Make sure there's space to write */
    result = scull_getwritespace(dev, filp);
    if (result)
        return result;  /* scull_getwritespace called mutex_unlock(&dev->mutex) */

    /* ok, space is there, accept something */
    count = min(count, (size_t)spacefree(dev));
    if (dev->wp >= dev->rp)
        count = min(count, (size_t)(dev->end - dev->wp));   /* to end-of-buf */
    else /* the write pointer has wrapped, fill up to rp-1 */
        count = min(count, (size_t)(dev->rp - dev->wp -1);
    PDEBUG("Going to accept %li bytes to %p from %p\n", (long)count, dev->wp, buf);
    if (copy_from_user(dev->wp, buf, count){
        mutex_unlock(&dev->mutex);
        return -EFAULT;
    }
    dev->wp += count;
    if (dev->wp == dev->end)
        dev->wp = dev->buffer;  /* wrapped */
    mutex_unlock(&dev->mutex);

    /* finally, awake any reader */
    wake_up_interruptible(&dev->inq);   /* blocked in read() and select() */

    /* and signal asynchronous readers, explained late in chapter 5 */
    if (dev->async_queue)
        kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
    PDEBUG("\"%s\" did write %li bytes\n", current->comm, (long)count);
    return count;
}


/* 
 * Set up a cdev entry.
 */
static void scull_p_setup_cdev(struct scull_pipe *dev, int index)
{
    int err, devno = scull_p_devno + index;
    
    cdev_init(&dev->cdev, &scull_pipe_fops);
    dev->cdev.owner = THIS_MODULE;
    err = cdev_add(&dev->cdev, devno, 1);
    /* Fail gracefully if need be */
    if (err)
        printk(KERN_NOTICE "Error %d adding scullpipe%d", err, index);
}

/*
 * Initialize the pipe devs; return how many we did.
 */
int scull_p_init(dev_t firstdev)
{
    int i, result;

    result = register_chrdev_region(firstdev, scull_p_nr_devs, "scullp");
    if (result < 0) {
        printk(KERN_NOTICE "Unable to get scullp region, error %d\n", result);
        return 0;
    }
    scull_p_devno = firstdev;
    scull_p_devices = kmalloc(scull_p_nr_devs * sizeof(struct scull_pipe), GFP_KERNEL);
    if (scull_p_devices == NULL) {
        unregister_chrdev_region(firstdev, scull_p_nr_devs);
        return 0;
    }
    memset(scull_p_devices, 0, scull_p_nr_devs * sizeof(struct scull_pipe));
    for (i = 0; i < scull_p_nr_devs; i++) {
        init_waitqueue_head(&(scull_p_devices[i].inq));
        init_waitqueue_head(&(scull_p_devices[i].outq));
        mutex_init(&scull_p_devices[i].mutex);
        scull_p_setup_cdev(scull_p_devices + i, i);
    }

#ifdef SCULL_DEBUG
    create_proc_read_entry("scullpipe", 0, NULL, scull_read_p_mem, NULL);
#endif 
    return scull_p_nr_devs;
}
