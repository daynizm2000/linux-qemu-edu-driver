// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/ioctl.h>
#include <linux/poll.h>
#include "edu.h"

#define EDU_NAME "edu"
#define EDU_VID 0x1234
#define EDU_DID 0x11e8

#define EDU_DEV_COUNT 255
#define EDU_MINORS_PER_DEV 3
#define EDU_TOTAL_MINORS (EDU_DEV_COUNT * EDU_MINORS_PER_DEV)

#define EDU_FAC_IRQ_ON 0x80

#define EDU_DMA_START_CMDF 0x01
#define EDU_DMA_READ_SOURCE_CMDF 0x02
#define EDU_DMA_IRQ_ON_CMDF 0x04

#define EDU_FAC_IRQ 0x01
#define EDU_DMA_IRQ 0x100

#define EDU_MMIO_BAR 0
#define EDU_DMA_BITS 28

enum edu_wait_status {
        EDU_WAIT_NONE,
        EDU_WAIT_DONE,
        EDU_WAIT_STOP,
};

struct edu_client {
        void *client;
        
        const struct edu_client_ops *ops;
        
        spinlock_t lock;
        wait_queue_head_t queue;

        bool dead;
};

struct edu_client_ops {
        bool (*is_free)(struct edu_client*);
        void (*wake)(struct edu_client*, enum edu_wait_status status);
};

struct edu_hw {
        struct pci_dev *pdev;
        void __iomem *mmio;

        struct edu_client dma;
        struct edu_client fac;

        int irqv;
};

struct edu_cdev {
        struct cdev cdev;
        struct edu_dev *edu;
        struct device dev;
};

struct edu_fac {
        struct kref kref;
        wait_queue_head_t queue;
        enum edu_wait_status status;

        u32 val;

        bool processing;

        struct edu_fac_dev *dev;

        struct mutex lock;
};

struct edu_fac_dev {
        struct edu_cdev dev;
};

enum edu_dma_state {
        EDU_DMA_NONE_STATE,
        EDU_DMA_WRITE_STATE,
        EDU_DMA_READ_STATE,
};

struct edu_dma {
        struct kref kref;
        wait_queue_head_t queue;
        enum edu_wait_status status;
        
        dma_addr_t bus_dma;
        void *cpu_dma;
        struct device *phys_dev;

        enum edu_dma_state state;

        struct edu_dma_dev *dev;

        struct mutex lock;
};

struct edu_dma_dev {
        struct edu_cdev dev;
};

struct edu_mmio_dev {
        struct edu_cdev dev;
};

struct edu_dev {
        struct edu_hw __rcu *hw;
        struct device *phys_dev;

        unsigned int minor;

        struct edu_fac_dev fac;
        struct edu_dma_dev dma;
        struct edu_mmio_dev mmio;

        struct srcu_struct hw_srcu;

        struct kref kref;
};

static struct class *edu_class;
static dev_t edu_devt;
static DEFINE_IDA(edu_ida_minors);

static int edu_alloc_minor(void)
{
        return ida_alloc_max(&edu_ida_minors, EDU_DEV_COUNT - 1, GFP_KERNEL);
}

static void edu_free_minor(unsigned int minor)
{
        ida_free(&edu_ida_minors, minor);
}

static int edu_mutex_lock(struct mutex *m, bool nonblock)
{
        if (nonblock)
                return mutex_trylock(m) ? 0 : -EAGAIN;

        return mutex_lock_interruptible(m) ? -ERESTARTSYS : 0;
}

static void edu_waitqueue_sync(wait_queue_head_t *wq)
{
        unsigned long flags;

        spin_lock_irqsave(&wq->lock, flags);
        spin_unlock_irqrestore(&wq->lock, flags);
}

static void edu_client_init(struct edu_client *cl, const struct edu_client_ops *ops)
{
        cl->ops = ops;
        cl->client = NULL;
        cl->dead = false;
        spin_lock_init(&cl->lock);
        init_waitqueue_head(&cl->queue);
}

static void edu_client_stop(struct edu_client *cl)
{        
        scoped_guard(spinlock_irqsave, &cl->lock) {
                if (cl->client) {
                        cl->ops->wake(cl, EDU_WAIT_STOP);
                        cl->client = NULL;
                }
        }

        WRITE_ONCE(cl->dead, true);
        wake_up_all(&cl->queue);
}

static int edu_client_wait_free(struct edu_client *cl, bool nonblock)
{
        int ret;

        if (nonblock) {
                if (cl->ops->is_free(cl))
                        return 0;

                return READ_ONCE(cl->dead) ? -ENODEV : -EAGAIN;
        }

        ret = wait_event_interruptible(cl->queue,
                cl->ops->is_free(cl) || READ_ONCE(cl->dead));

        if (ret)
                return -ERESTARTSYS;

        return READ_ONCE(cl->dead) ? -ENODEV : 0;
}

static struct edu_dev *edu_dev_get(struct edu_dev *edu)
{
        if (edu)
                kref_get(&edu->kref);

        return edu;
}

static void edu_dev_release(struct kref *kref)
{
        struct edu_dev *edu = container_of(kref, struct edu_dev, kref);

        put_device(edu->phys_dev);
        edu_free_minor(edu->minor);
        cleanup_srcu_struct(&edu->hw_srcu);
        kfree(edu);
}

static void edu_dev_put(struct edu_dev *edu)
{
        if (edu)
                kref_put(&edu->kref, edu_dev_release);
}

static struct edu_hw *edu_dev_hw_enter(struct edu_dev *edu, int *idx)
{
        struct edu_hw *hw;

        *idx = srcu_read_lock(&edu->hw_srcu);

        hw = srcu_dereference(edu->hw, &edu->hw_srcu);

        if (!hw)
                srcu_read_unlock(&edu->hw_srcu, *idx);

        return hw;
}

static void edu_dev_hw_exit(struct edu_dev *edu, int idx)
{
        srcu_read_unlock(&edu->hw_srcu, idx);
}

static void edu_cdev_release(struct device *dev)
{
        struct edu_cdev *cdev = container_of(dev, struct edu_cdev, dev);

        edu_dev_put(cdev->edu);
}

static int edu_cdev_init(struct edu_cdev *c, struct edu_dev *edu,
        const struct file_operations *fops, const char *name, unsigned int idx)
{
        int ret;
        
        c->edu = edu_dev_get(edu);
        
        cdev_init(&c->cdev, fops);
        c->cdev.owner = THIS_MODULE;

        c->dev.devt = MKDEV(MAJOR(edu_devt),
                edu->minor * EDU_MINORS_PER_DEV + idx);
        c->dev.release = edu_cdev_release;
        c->dev.class = edu_class;
        c->dev.parent = edu->phys_dev;

        ret = dev_set_name(&c->dev, "%s%u/%s",
                EDU_NAME, edu->minor, name);

        if (ret)
                goto dev_put_ret;

        device_initialize(&c->dev);

        ret = cdev_device_add(&c->cdev, &c->dev);

        if (ret)
                goto dev_put_ret;
        
        return 0;
dev_put_ret:
        put_device(&c->dev);
        return ret;
}

static void edu_cdev_destroy(struct edu_cdev *cdev)
{
        cdev_device_del(&cdev->cdev, &cdev->dev);
        put_device(&cdev->dev);
}

static struct edu_fac *edu_fac_get(struct edu_fac *fac)
{
        if (fac)
                kref_get(&fac->kref);

        return fac;
}

static void edu_fac_release(struct kref *kref)
{
        struct edu_fac *fac = container_of(kref, struct edu_fac, kref);

        edu_waitqueue_sync(&fac->queue);
        edu_dev_put(fac->dev->dev.edu);
        kfree(fac);
}

static void edu_fac_put(struct edu_fac *fac)
{
        if (fac)
                kref_put(&fac->kref, edu_fac_release);
}

static int edu_fac_wait(struct edu_fac *fac, bool nonblock)
{
        int ret;

        if (!READ_ONCE(fac->processing))
                return 0;

        if (nonblock) {
                if (READ_ONCE(fac->status) == EDU_WAIT_NONE)
                        return -EAGAIN;

                goto success;
        }

        ret = wait_event_interruptible(fac->queue, READ_ONCE(fac->status) != EDU_WAIT_NONE);

        if (ret)
                return ret;

success:
        edu_fac_put(fac);

        return READ_ONCE(fac->status) == EDU_WAIT_STOP ? -ENODEV : 0;
}

static void edu_fac_wait_last(struct edu_fac *fac)
{
        if (!READ_ONCE(fac->processing))
                return;

        wait_event(fac->queue, READ_ONCE(fac->status) != EDU_WAIT_NONE);
        edu_fac_put(fac);
}

static struct edu_fac *edu_fac_alloc(struct edu_fac_dev *dev)
{
        struct edu_fac *fac = kzalloc_obj(*fac);

        if (!fac)
                return NULL;

        kref_init(&fac->kref);
        init_waitqueue_head(&fac->queue);
        fac->processing = false;
        fac->dev = dev;
        fac->status = EDU_WAIT_NONE;
        mutex_init(&fac->lock);
        edu_dev_get(dev->dev.edu);

        return fac;
}

static bool edu_fac_client_is_free(struct edu_client *cl)
{
        struct edu_hw *hw = container_of(cl, struct edu_hw, fac);

        return !(ioread32(hw->mmio + EDU_STATUS_REG) & 0x01) && !READ_ONCE(cl->client);
}

static void edu_fac_wake(struct edu_client *cl, enum edu_wait_status status)
{
        struct edu_hw *hw = container_of(cl, struct edu_hw, fac);
        struct edu_fac *fac = cl->client;

        guard(spinlock)(&fac->queue.lock);

        if (status == EDU_WAIT_DONE)
                WRITE_ONCE(fac->val, ioread32(hw->mmio + EDU_FAC_COMP_REG));

        WRITE_ONCE(fac->status, status);
        WRITE_ONCE(fac->processing, false);

        if (status == EDU_WAIT_STOP)
                wake_up_all_locked(&fac->queue);
        else
                wake_up_locked(&fac->queue);
}

static const struct edu_client_ops edu_fac_client_ops = {
        .is_free = edu_fac_client_is_free,
        .wake = edu_fac_wake
};

static int edu_fac_submit(struct edu_fac *fac, struct edu_hw *hw, u32 val, bool nonblock)
{
        int ret;
        struct edu_client *cl = &hw->fac;

        if (READ_ONCE(fac->processing))
                return -EBUSY;

        while (1) {
                ret = edu_client_wait_free(cl, nonblock);

                if (ret)
                        return ret;

                scoped_guard(spinlock_irqsave, &cl->lock) {
                        if (READ_ONCE(cl->dead))
                                return -ENODEV;
                        
                        if (cl->ops->is_free(cl)) {
                                cl->client = fac;

                                fac->val = val;
                                WRITE_ONCE(fac->processing, true);
                                WRITE_ONCE(fac->status, EDU_WAIT_NONE);
                                edu_fac_get(fac);

                                iowrite32(EDU_FAC_IRQ_ON, hw->mmio + EDU_STATUS_REG);
                                iowrite32(val, hw->mmio + EDU_FAC_COMP_REG);

                                return 0;
                        }
                }
        }
}

static int edu_fac_dev_open(struct inode *inode, struct file *filp)
{
        struct edu_fac *fac;
        struct edu_fac_dev *facdev = container_of(container_of(inode->i_cdev, struct edu_cdev, cdev), struct edu_fac_dev, dev);

        fac = edu_fac_alloc(facdev);

        if (!fac)
                return -ENOMEM;

        filp->private_data = fac;

        return 0;
}

static ssize_t edu_fac_dev_read(struct file *filp,
        char __user *ubuf, size_t count, loff_t *ppos)
{
        int ret;
        struct edu_fac *fac = filp->private_data;
        bool nonblock = filp->f_flags & O_NONBLOCK;

        if (count < 4)
                return -EINVAL;

        ret = edu_mutex_lock(&fac->lock, nonblock);

        if (ret)
                return ret;

        ret = edu_fac_wait(fac, nonblock);

        if (ret)
                goto out;

        if (copy_to_user(ubuf, &fac->val, 4)) {
                ret = -EFAULT;
                goto out;
        }

        ret = 4;
out:
        mutex_unlock(&fac->lock);
        return ret;
}

static ssize_t edu_fac_dev_write(struct file *filp,
        const char __user *ubuf, size_t count, loff_t *ppos)
{
        int ret;
        struct edu_fac *fac = filp->private_data;
        struct edu_hw *hw;
        int idx;
        u32 val;
        bool nonblock = filp->f_flags & O_NONBLOCK;

        if (count < 4)
                return -EINVAL;

        if (copy_from_user(&val, ubuf, 4))
                return -EFAULT;

        ret = edu_mutex_lock(&fac->lock, nonblock);

        if (ret)
                return ret;

        hw = edu_dev_hw_enter(fac->dev->dev.edu, &idx);

        if (!hw) {
                ret = -ENODEV;
                goto out;
        }

        ret = edu_fac_submit(fac, hw, val, nonblock);

        edu_dev_hw_exit(fac->dev->dev.edu, idx);

        if (ret)
                goto out;

        ret = 4;
out:
        mutex_unlock(&fac->lock);
        return ret;
}

static __poll_t edu_fac_dev_poll(struct file *filp, struct poll_table_struct *pt)
{
        struct edu_fac *fac = filp->private_data;
        enum edu_wait_status st;

        poll_wait(filp, &fac->queue, pt);

        st = READ_ONCE(fac->status);

        if (st == EDU_WAIT_STOP)
                return POLLERR | POLLHUP;

        if (READ_ONCE(fac->processing))
                return 0;

        if (st == EDU_WAIT_DONE)
                return POLLIN | POLLOUT;

        return POLLIN;
}

static int edu_fac_dev_release(struct inode *inode, struct file *filp)
{
        struct edu_fac *fac = filp->private_data;

        edu_fac_wait_last(fac);
        edu_fac_put(fac);

        return 0;
}

static const struct file_operations edu_fac_fops = {
        .owner = THIS_MODULE,
        .open = edu_fac_dev_open,
        .read = edu_fac_dev_read,
        .write = edu_fac_dev_write,
        .poll = edu_fac_dev_poll,
        .release = edu_fac_dev_release
};

static int edu_fac_dev_init(struct edu_fac_dev *fac,
        struct edu_dev *edu, unsigned int idx)
{
        return edu_cdev_init(&fac->dev, edu, &edu_fac_fops, "fac", idx);
}

static void edu_fac_dev_destroy(struct edu_fac_dev *fac)
{
        edu_cdev_destroy(&fac->dev);
}

static struct edu_dma *edu_dma_get(struct edu_dma *dma)
{
        if (dma)
                kref_get(&dma->kref);

        return dma;
}

static void edu_dma_release(struct kref *kref)
{
        struct edu_dma *dma = container_of(kref, struct edu_dma, kref);

        dma_free_coherent(dma->phys_dev, EDU_DMA_SIZE,
                dma->cpu_dma, dma->bus_dma);

        edu_waitqueue_sync(&dma->queue);
        put_device(dma->phys_dev);
        edu_dev_put(dma->dev->dev.edu);
        kfree(dma);
}

static void edu_dma_put(struct edu_dma *dma)
{
        if (dma)
                kref_put(&dma->kref, edu_dma_release);
}

static int edu_dma_wait(struct edu_dma *dma, bool nonblock)
{
        int ret;

        if (READ_ONCE(dma->state) == EDU_DMA_NONE_STATE)
                return 0;

        if (nonblock) {
                if (READ_ONCE(dma->status) == EDU_WAIT_NONE)
                        return -EAGAIN;

                goto success;
        }

        ret = wait_event_interruptible(dma->queue, READ_ONCE(dma->status) != EDU_WAIT_NONE);

        if (ret)
                return ret;

success:
        edu_dma_put(dma);

        return READ_ONCE(dma->status) == EDU_WAIT_STOP ? -ENODEV : 0;
}

static void edu_dma_wait_last(struct edu_dma *dma)
{
        if (READ_ONCE(dma->state) == EDU_DMA_NONE_STATE)
                return;
        
        wait_event(dma->queue, READ_ONCE(dma->status) != EDU_WAIT_NONE);
        edu_dma_put(dma);
}

static struct edu_dma *edu_dma_alloc(struct edu_dma_dev *dev, struct device *phys_dev)
{
        struct edu_dma *dma = kzalloc_obj(*dma);

        if (!dma)
                return NULL;

        dma->cpu_dma = dma_alloc_coherent(phys_dev, EDU_DMA_SIZE,
                &dma->bus_dma, GFP_KERNEL);

        if (!dma->cpu_dma) {
                kfree(dma);
                return NULL;
        }

        kref_init(&dma->kref);
        init_waitqueue_head(&dma->queue);
        dma->state = EDU_DMA_NONE_STATE;
        dma->dev = dev;
        dma->phys_dev = get_device(phys_dev);
        mutex_init(&dma->lock);

        edu_dev_get(dev->dev.edu);

        return dma;
}

static bool edu_dma_client_is_free(struct edu_client *cl)
{
        struct edu_hw *hw = container_of(cl, struct edu_hw, dma);

        return !(ioread32(hw->mmio + EDU_DMA_CMD_REG) & 0x01) && !READ_ONCE(cl->client);
}

static void edu_dma_wake(struct edu_client *cl, enum edu_wait_status status)
{
        struct edu_dma *dma = cl->client;

        guard(spinlock)(&dma->queue.lock);

        WRITE_ONCE(dma->status, status);
        WRITE_ONCE(dma->state, EDU_DMA_NONE_STATE);
        
        if (status == EDU_WAIT_STOP)
                wake_up_all_locked(&dma->queue);
        else
                wake_up_locked(&dma->queue);
}

static const struct edu_client_ops edu_dma_client_ops = {
        .is_free = edu_dma_client_is_free,
        .wake = edu_dma_wake
};

static bool edu_dma_validate_request(size_t *count, size_t offset)
{
        if (!*count || offset >= EDU_DMA_SIZE)
                return false;

        if (*count > EDU_DMA_SIZE - offset)
                *count = EDU_DMA_SIZE - offset;

        return true;
}

static int __edu_dma_mmio_submit(struct edu_dma *dma, struct edu_hw *hw,
        size_t count, size_t offset)
{
        u32 src;
        u32 dest;
        u32 flags = EDU_DMA_START_CMDF | EDU_DMA_IRQ_ON_CMDF;
        enum edu_dma_state state = READ_ONCE(dma->state);

        switch (state) {
                case EDU_DMA_READ_STATE: {
                        src = EDU_DMA_BASE + offset;
                        dest = dma->bus_dma + offset;
                        flags |= EDU_DMA_READ_SOURCE_CMDF;

                        break;
                }
                case EDU_DMA_WRITE_STATE: {
                        src = dma->bus_dma + offset;
                        dest = EDU_DMA_BASE + offset;
                
                        break;
                }
                default: {
                        return -EINVAL;
                }
        }

        iowrite32(src, hw->mmio + EDU_DMA_SOURCE_REG);
        iowrite32(dest, hw->mmio + EDU_DMA_DEST_REG);
        iowrite32(count, hw->mmio + EDU_DMA_COUNT_REG);
        iowrite32(flags, hw->mmio + EDU_DMA_CMD_REG);

        return 0;
}

static int edu_dma_submit(struct edu_dma *dma,
        enum edu_dma_state state, struct edu_hw *hw,
        size_t count, size_t offset, bool nonblock)
{
        int ret;
        struct edu_client *cl = &hw->dma;

        if (READ_ONCE(dma->state) != EDU_DMA_NONE_STATE)
                return -EBUSY;

        while (1) {
                ret = edu_client_wait_free(cl, nonblock);

                if (ret)
                        return ret;

                scoped_guard(spinlock_irqsave, &cl->lock) {
                        if (READ_ONCE(cl->dead))
                                return -ENODEV;

                        if (cl->ops->is_free(cl)) {
                                cl->client = dma;

                                WRITE_ONCE(dma->state, state);
                                WRITE_ONCE(dma->status, EDU_WAIT_NONE);
                                edu_dma_get(dma);

                                ret = __edu_dma_mmio_submit(dma, hw, count, offset);

                                if (ret) {
                                        WRITE_ONCE(dma->state, EDU_DMA_NONE_STATE);
                                        cl->client = NULL;
                                        edu_dma_put(dma);
                                }

                                return ret;
                        }
                }
        }
}

static int edu_dma_dev_open(struct inode *inode, struct file *filp)
{
        struct edu_dma *dma;
        struct edu_dma_dev *dmadev = container_of(container_of(inode->i_cdev, struct edu_cdev, cdev), struct edu_dma_dev, dev);

        dma = edu_dma_alloc(dmadev, dmadev->dev.edu->phys_dev);

        if (!dma)
                return -ENOMEM;

        filp->private_data = dma;

        return 0;
}

static int edu_dma_dev_release(struct inode *inode, struct file *filp)
{
        struct edu_dma *dma = filp->private_data;

        edu_dma_wait_last(dma);
        edu_dma_put(dma);

        return 0;
}

static ssize_t edu_dma_dev_read(struct file *filp,
        char __user *ubuf, size_t count, loff_t *ppos)
{
        int ret;
        struct edu_hw *hw;
        int idx;
        bool nonblock = filp->f_flags & O_NONBLOCK;
        struct edu_dma *dma = filp->private_data;

        ret = edu_mutex_lock(&dma->lock, nonblock);

        if (ret)
                return ret;

        if (!edu_dma_validate_request(&count, *ppos))
                goto out;

        if (READ_ONCE(dma->state) == EDU_DMA_WRITE_STATE) {
                ret = edu_dma_wait(dma, nonblock);

                if (ret)
                        goto out;
        }
        
        if (READ_ONCE(dma->state) == EDU_DMA_NONE_STATE) {
                hw = edu_dev_hw_enter(dma->dev->dev.edu, &idx);

                if (!hw) {
                        ret = -ENODEV;
                        goto out;
                }

                ret = edu_dma_submit(dma, EDU_DMA_READ_STATE,
                        hw, count, *ppos, nonblock);

                edu_dev_hw_exit(dma->dev->dev.edu, idx);

                if (ret)
                        goto out;
        }

        ret = edu_dma_wait(dma, nonblock);

        if (ret)
                goto out;

        if (copy_to_user(ubuf, dma->cpu_dma + *ppos, count)) {
                ret = -EFAULT;
                goto out;
        }

        *ppos += count;
        ret = count;
out:
        mutex_unlock(&dma->lock);
        return ret;
}

static ssize_t edu_dma_dev_write(struct file *filp,
        const char __user *ubuf, size_t count, loff_t *ppos)
{
        int ret;
        struct edu_hw *hw;
        int idx;
        bool nonblock = filp->f_flags & O_NONBLOCK;
        struct edu_dma *dma = filp->private_data;

        ret = edu_mutex_lock(&dma->lock, nonblock);

        if (ret)
                return ret;

        if (!edu_dma_validate_request(&count, *ppos))
                goto out;

        if (READ_ONCE(dma->state) != EDU_DMA_NONE_STATE) {
                ret = edu_dma_wait(dma, nonblock);

                if (ret)
                        goto out;
        }

        if (copy_from_user(dma->cpu_dma + *ppos, ubuf, count)) { 
                ret = -EFAULT;
                goto out;
        }

        hw = edu_dev_hw_enter(dma->dev->dev.edu, &idx);

        if (!hw) {
                ret = -ENODEV;
                goto out;
        }

        ret = edu_dma_submit(dma, EDU_DMA_WRITE_STATE, 
                hw, count, *ppos, nonblock);

        edu_dev_hw_exit(dma->dev->dev.edu, idx);

        if (ret)
                goto out;

        *ppos += count;
        ret = count;
out:
        mutex_unlock(&dma->lock);
        return ret;
}

static __poll_t edu_dma_dev_poll(struct file *filp, struct poll_table_struct *pt)
{
        struct edu_dma *dma = filp->private_data;
        enum edu_wait_status st;

        poll_wait(filp, &dma->queue, pt);

        st = READ_ONCE(dma->status);

        if (st == EDU_WAIT_STOP)
                return POLLERR | POLLHUP;
        
        if (READ_ONCE(dma->state) != EDU_DMA_NONE_STATE)
                return 0;

        if (st == EDU_WAIT_DONE)
                return POLLOUT | POLLIN;

        return POLLIN;
}

static const struct file_operations edu_dma_fops = {
        .owner = THIS_MODULE,
        .open = edu_dma_dev_open,
        .read = edu_dma_dev_read,
        .write = edu_dma_dev_write,
        .poll = edu_dma_dev_poll,
        .llseek = default_llseek,
        .release = edu_dma_dev_release
};

static int edu_dma_dev_init(struct edu_dma_dev *dma,
        struct edu_dev *edu, unsigned int idx)
{
        return edu_cdev_init(&dma->dev, edu, &edu_dma_fops, "dma", idx);
}

static void edu_dma_dev_destroy(struct edu_dma_dev *dma)
{
        edu_cdev_destroy(&dma->dev);
}

static int edu_mmio_dev_open(struct inode *inode, struct file *filp)
{
        struct edu_mmio_dev *mmio = container_of(container_of(inode->i_cdev, struct edu_cdev, cdev), struct edu_mmio_dev, dev);

        filp->private_data = mmio;
        edu_dev_get(mmio->dev.edu);

        return 0;
}

static int edu_mmio_dev_release(struct inode *inode, struct file *filp)
{
        struct edu_mmio_dev *mmio = filp->private_data;

        edu_dev_put(mmio->dev.edu);

        return 0;
}

static int edu_mmio_dev_mmap(struct file *filp, struct vm_area_struct *vma)
{
        struct edu_mmio_dev *dev = filp->private_data;
        struct edu_dev *edu = dev->dev.edu;
        struct edu_hw *hw;
        int idx;
        int ret;
        size_t offset;
        size_t rsize = vma->vm_end - vma->vm_start;
        size_t plen;

        if (check_shl_overflow(vma->vm_pgoff, PAGE_SHIFT, &offset))
                return -EINVAL;

        hw = edu_dev_hw_enter(edu, &idx);

        if (!hw)
                return -ENODEV;

        plen = pci_resource_len(hw->pdev, EDU_MMIO_BAR);

        if (offset >= plen || rsize > plen - offset) {
                edu_dev_hw_exit(edu, idx);
                return -EINVAL;
        }

        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

        ret = io_remap_pfn_range(vma, vma->vm_start,
                (pci_resource_start(hw->pdev, EDU_MMIO_BAR) >> PAGE_SHIFT) + vma->vm_pgoff,
                rsize, vma->vm_page_prot);

        edu_dev_hw_exit(edu, idx);

        return ret;
}

static int edu_mmio_dev_ioctl_size(struct edu_dev *edu, __u32 __user *dest)
{
        struct edu_hw *hw;
        int idx;
        __u32 size;

        hw = edu_dev_hw_enter(edu, &idx);

        if (!hw)
                return -ENODEV;

        size = pci_resource_len(hw->pdev, EDU_MMIO_BAR);

        edu_dev_hw_exit(edu, idx);

        if (copy_to_user(dest, &size, sizeof(*dest)))
                return -EFAULT;

        return 0;
}

static long edu_mmio_dev_ioctl(struct file *filp, unsigned int cmd,
        unsigned long addr)
{
        struct edu_cdev *cdev = &((struct edu_mmio_dev*)filp->private_data)->dev;

        switch (cmd) {
                case EDU_MMIO_SIZE: {
                        return edu_mmio_dev_ioctl_size(cdev->edu, (__u32 __user *)addr);
                }
        }

        return -ENOTTY;
}

static const struct file_operations edu_mmio_fops = {
        .owner = THIS_MODULE,
        .open = edu_mmio_dev_open,
        .mmap = edu_mmio_dev_mmap,
        .unlocked_ioctl = edu_mmio_dev_ioctl,
        .release = edu_mmio_dev_release
};

static int edu_mmio_dev_init(struct edu_mmio_dev *mmio,
        struct edu_dev *edu, unsigned int idx)
{
        return edu_cdev_init(&mmio->dev, edu, &edu_mmio_fops, "mmio", idx);
}

static void edu_mmio_dev_destroy(struct edu_mmio_dev *mmio)
{
        edu_cdev_destroy(&mmio->dev);
}

static void edu_irq_client_handle(struct edu_client *cl)
{
        scoped_guard(spinlock, &cl->lock) {
                if (cl->client) {
                        cl->ops->wake(cl, EDU_WAIT_DONE);
                        cl->client = NULL;
                }
        }

        wake_up(&cl->queue);
}

static irqreturn_t edu_irq_handler(int irqv, void *arg)
{
        struct edu_hw *hw = arg;
        struct device *dev = &hw->pdev->dev;
        u32 status = ioread32(hw->mmio + EDU_INTERRUPT_STATUS_REG);

        if (!status) {
                dev_dbg(dev, "Interrupt received when edu interrupt status register (0x%x) = 0x0\n",
                        EDU_INTERRUPT_STATUS_REG);
                
                return IRQ_NONE;
        }

        iowrite32(status, hw->mmio + EDU_INTERRUPT_ACKNOW_REG);

        if (!(status & (EDU_FAC_IRQ | EDU_DMA_IRQ))) {
                dev_warn(dev, "Interrupt received with unknown interrupt status = 0x%x\n",
                        status);

                return IRQ_HANDLED;
        }

        if (status & EDU_FAC_IRQ)
                edu_irq_client_handle(&hw->fac);

        if (status & EDU_DMA_IRQ)
                edu_irq_client_handle(&hw->dma);

        return IRQ_HANDLED;
}

static int edu_hw_irq_init(struct edu_hw *hw)
{
        struct device *dev = &hw->pdev->dev;
        int ret;

        ret = pci_alloc_irq_vectors(hw->pdev, 1, 1,
                PCI_IRQ_ALL_TYPES);

        if (ret < 0) {
                dev_err(dev, "Failed to allocate interrupt vectors for the physical device, "
                                "pci_alloc_irq_vectors(%p, 1, 1, %d): %d\n",
                        hw->pdev, PCI_IRQ_ALL_TYPES, ret);

                return ret;
        }

        hw->irqv = pci_irq_vector(hw->pdev, 0);

        ret = request_irq(hw->irqv, edu_irq_handler, IRQF_SHARED,
                EDU_NAME, hw);

        if (ret) {
                dev_err(dev, "Failed to register interrupt handler for the physical device, "
                                "request_irq(%d, %p, %d, %s, %p): %d\n",
                        hw->irqv, edu_irq_handler, IRQF_SHARED, EDU_NAME, hw, ret);

                pci_free_irq_vectors(hw->pdev);
                return ret;
        }

        return 0;
}

static void edu_hw_irq_free(struct edu_hw *hw)
{
        free_irq(hw->irqv, hw);
        pci_free_irq_vectors(hw->pdev);
}

static void edu_hw_clients_stop(struct edu_hw *hw)
{
        edu_client_stop(&hw->fac);
        edu_client_stop(&hw->dma);
}

static bool edu_hw_version_validate(struct edu_hw *hw)
{
        u32 ver = ioread32(hw->mmio + EDU_VERSION_ID_REG);

        if (ver != EDU_VERSION) {
                dev_err(&hw->pdev->dev, "0x%x version is not supported by this driver\n",
                        ver);
                
                return false;
        }

        return true;
}

static bool edu_hw_liveness_check(struct edu_hw *hw)
{
        u32 status;

        iowrite32(0x10, hw->mmio + EDU_LIVENESS_CHECK_REG);
        status = ioread32(hw->mmio + EDU_LIVENESS_CHECK_REG);

        if (status != ~0x10u) {
                dev_err(&hw->pdev->dev, "Device check failed: liveness check register (0x%x) returned %u for 0x10 instead of ~0x10\n",
                        EDU_LIVENESS_CHECK_REG, status);

                return false;
        }

        dev_dbg(&hw->pdev->dev, "Device check passed, physical device is ready\n");

        return true;
}

static bool edu_hw_validate(struct edu_hw *hw)
{
        if (!edu_hw_version_validate(hw))
                return false;

        return edu_hw_liveness_check(hw);
}

static int edu_hw_init(struct edu_hw *hw, struct pci_dev *pdev)
{
        int ret;
        struct device *dev = &pdev->dev;

        hw->pdev = pdev;
        edu_client_init(&hw->fac, &edu_fac_client_ops);
        edu_client_init(&hw->dma, &edu_dma_client_ops);

        ret = pcim_enable_device(pdev);

        if (ret) {
                dev_err(dev, "Unable to initialize the hw device for use with the driver, "
                                "pcim_enable_device(%p): %d\n",
                        pdev, ret);

                return ret;
        }

        pci_set_master(pdev);

        hw->mmio = pcim_iomap_region(pdev, EDU_MMIO_BAR, EDU_NAME);

        if (IS_ERR(hw->mmio)) {
                ret = PTR_ERR(hw->mmio);

                dev_err(dev, "Failed to allocate and map BAR%d (registers) of the physical device into kernel virtual memory, "
                                "pcim_iomap_region(%p, %d, %s): %d\n",
                        EDU_MMIO_BAR, pdev, EDU_MMIO_BAR, EDU_NAME, ret);

                return ret;
        }

        if (!edu_hw_validate(hw))
                return -ENODEV;

        ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(EDU_DMA_BITS));

        if (ret) {
                dev_err(dev, "Failed to set mask for streaming and coherent DMA memory of the physical device, "
                                "dma_set_mask_and_coherent(%p, %llu): %d\n",
                        dev, DMA_BIT_MASK(EDU_DMA_BITS), ret);

                return ret;
        }

        ret = edu_hw_irq_init(hw);

        if (ret)
                return ret;

        return 0;
}

static void edu_hw_destroy(struct edu_hw *hw)
{
        edu_hw_irq_free(hw);
}

static struct edu_dev *edu_dev_alloc(struct edu_hw *hw)
{
        int ret;
        struct edu_dev *edu;
        
        edu = kzalloc_obj(*edu);

        if (!edu)
                return NULL;

        ret = init_srcu_struct(&edu->hw_srcu);

        if (ret) {
                kfree(edu);
                return NULL;
        }

        ret = edu_alloc_minor();

        if (ret < 0) {
                cleanup_srcu_struct(&edu->hw_srcu);
                kfree(edu);
                return NULL;
        }

        edu->minor = ret;
        kref_init(&edu->kref);
        edu->phys_dev = get_device(&hw->pdev->dev);
        rcu_assign_pointer(edu->hw, hw);

        ret = edu_fac_dev_init(&edu->fac, edu, 0);

        if (ret)
                goto put_null;

        ret = edu_dma_dev_init(&edu->dma, edu, 1);

        if (ret)
                goto err;

        ret = edu_mmio_dev_init(&edu->mmio, edu, 2);

        if (ret)
                goto err1;

        return edu;
err1:
        edu_dma_dev_destroy(&edu->dma);
err:
        edu_fac_dev_destroy(&edu->fac);
put_null:
        edu_dev_put(edu);
        return NULL;
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
        int ret;
        struct edu_hw *hw;
        struct edu_dev *edu;

        hw = devm_kzalloc(&pdev->dev, sizeof(*hw), GFP_KERNEL);

        if (!hw)
                return -ENOMEM;

        ret = edu_hw_init(hw, pdev);

        if (ret)
                return ret;

        edu = edu_dev_alloc(hw);

        if (!edu) {
                edu_hw_destroy(hw);
                return -ENOMEM;
        }

        pci_set_drvdata(pdev, edu);

        dev_info(edu->phys_dev, "device has added, edu version: 0x%x\n",
                EDU_VERSION);

        return 0;
}

static void edu_cdevs_remove(struct edu_dev *edu)
{
        edu_fac_dev_destroy(&edu->fac);
        edu_dma_dev_destroy(&edu->dma);
        edu_mmio_dev_destroy(&edu->mmio);
}

static void edu_remove(struct pci_dev *pdev)
{
        struct edu_dev *edu = pci_get_drvdata(pdev);
        struct edu_hw *hw = rcu_dereference_raw(edu->hw);

        edu_cdevs_remove(edu);

        pci_clear_master(hw->pdev);

        edu_hw_clients_stop(hw);

        rcu_assign_pointer(edu->hw, NULL);
        synchronize_srcu(&edu->hw_srcu);

        edu_dev_put(edu);
        
        edu_hw_destroy(hw);
}

static const struct pci_device_id edu_pci_table[] = {
        { PCI_DEVICE(EDU_VID, EDU_DID) },
        { 0 }
};

MODULE_DEVICE_TABLE(pci, edu_pci_table);

static struct pci_driver edu_pci_driver = {
        .name = EDU_NAME,
        .id_table = edu_pci_table,
        .probe = edu_probe,
        .remove = edu_remove
};

static int __init mod_init(void)
{
        int ret;

        ret = alloc_chrdev_region(&edu_devt, 0, EDU_TOTAL_MINORS, EDU_NAME);

        if (ret) {
                pr_err("Failed to dynamically allocate region for %d character devices, "
                                "failed to alloc_chrdev_region(%p, 0, %d, %s): %d\n",
                        EDU_TOTAL_MINORS, &edu_devt, EDU_TOTAL_MINORS, EDU_NAME, ret);

                return ret;
        }

        edu_class = class_create(EDU_NAME);

        if (IS_ERR(edu_class)) {
                ret = PTR_ERR(edu_class);

                pr_err("Failed to create device class, failed to class_create(%s): %d\n", EDU_NAME, ret);

                goto err;
        }

        ret = pci_register_driver(&edu_pci_driver);

        if (ret) {
                pr_err("Failed to register PCI driver, failed to pci_register_driver(%p): %d\n",
                        &edu_pci_driver, ret);

                goto err1;
        }
        
        pr_info("PCI driver successfully initialized and added\n");

        return 0;
err1:
        class_destroy(edu_class);
err:
        unregister_chrdev_region(edu_devt, EDU_TOTAL_MINORS);
        return ret;
}

static void __exit mod_exit(void)
{
        pci_unregister_driver(&edu_pci_driver);
        class_destroy(edu_class);
        unregister_chrdev_region(edu_devt, EDU_TOTAL_MINORS);

        pr_info("PCI driver uninitialized and removed\n");
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_AUTHOR("DAYNIZM2000");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QEMU PCI EDU driver");
