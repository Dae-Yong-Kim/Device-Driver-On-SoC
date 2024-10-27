#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/hashtable.h>
#include <linux/pm_wakeirq.h>
#include <linux/amba/bus.h>
#ifdef CONFIG_DMA_ENGINE
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#endif
#include <linux/gpio/consumer.h>

#define COMENTO_DR        0x00    /* Data register */
#define COMENTO_FR        0x04    /* Data register */
#define COMENTO_RIS       0x08    /* Raw Interrupt Status Register */
#define COMENTO_ICR       0x0c    /* Interrupt Clear Register */
#define COMENTO_IMSC      0x10    /* Interrupt Mask Set/Clear Register */
#define COMENTO_MIS       0x14    /* Masked Interrupt Status Register */

#define COMENTO_INT_RX_EMPTY (1 << 2)
#define COMENTO_INT_TX       (1 << 1)
#define COMENTO_INT_RX       (1 << 0)

#define COMENTO_FLAG_TXFE (1 << 3)
#define COMENTO_FLAG_TXFF (1 << 2)
#define COMENTO_FLAG_RXFE (1 << 1)
#define COMENTO_FLAG_RXFF (1 << 0)

#define MIN(a,b) ((a)<(b) ? (a):(b))
#define COMENTO_FIFO_SIZE 32

#ifdef CONFIG_DMA_ENGINE
struct comento_dma_buf {
    struct dma_chan *chan;
    struct scatterlist sg;
    dma_cookie_t cookie;
    char *buf;
    bool queued;
};
#define COMENTO_DMA_BUFFER_SIZE 4096
#endif

struct comento_mmio_device {
        struct device *dev;
        struct device *adev;
        void __iomem *base;
	struct work_struct rx_work;
	rwlock_t lock;
	int rx_size;
	wait_queue_head_t tx_wq;
	unsigned int minor;
#ifdef CONFIG_DMA_ENGINE
	struct comento_dma_buf tx;
	struct comento_dma_buf rx;
	struct work_struct rx_dma_work;
#endif
	struct gpio_descs *leds;
	struct gpio_desc *button;
	struct hlist_node hash;
};

static unsigned int comento_mmio_major;
static unsigned int comento_mmio_minor;
static struct class *comento_mmio_class;

static DEFINE_HASHTABLE(comento_mmio_table, 4);

static struct comento_mmio_device *comento_mmio_get_device(unsigned int minor) {
    struct comento_mmio_device *node;
    hash_for_each_possible(comento_mmio_table, node, hash, minor) {
        if (node->minor == minor) {
	    return node;
	}
    }
    return NULL;
}

static ssize_t comento_mmio_read(struct file *fp, char __user *buf,
                                        size_t len, loff_t *ppos)
{
    struct comento_mmio_device *cmdev;
    ssize_t read_bytes = 0;

    cmdev = comento_mmio_get_device(iminor(fp->f_inode));
    if (cmdev == NULL) {
        return read_bytes;
    }

    read_lock(&cmdev->lock);
    if (cmdev->rx_size <= len + *ppos) {
        len = cmdev->rx_size - *ppos;
    }
    read_bytes = len - copy_to_user(buf, cmdev->rx.buf + *ppos, len);
    *ppos += read_bytes;

    read_unlock(&cmdev->lock);

    return read_bytes;
}

#ifdef CONFIG_DMA_ENGINE
static void comento_dma_tx_callback(void *data)
{
    struct comento_mmio_device *cmdev = (struct comento_mmio_device *)data;
    dma_unmap_sg(cmdev->dev->parent, &cmdev->tx.sg, 1, DMA_TO_DEVICE);
    cmdev->tx.queued = false;
    gpiod_set_value(cmdev->leds->desc[0], 0);
    wake_up_interruptible(&cmdev->tx_wq);
}
#endif

static ssize_t comento_mmio_write(struct file *fp, const char __user *buf,
                                                size_t len, loff_t *ppos)
{
    struct comento_mmio_device *cmdev;
    ssize_t written_bytes = 0, bytes;

    cmdev = comento_mmio_get_device(iminor(fp->f_inode));
    if (cmdev == NULL) {
        return written_bytes;
    }

    write_lock(&cmdev->lock);

// split into functions
#ifdef CONFIG_DMA_ENGINE
    struct dma_async_tx_descriptor *desc;

    while (len > 0) {
        wait_event_interruptible(cmdev->tx_wq, !cmdev->tx.queued);

	bytes = MIN(len, COMENTO_DMA_BUFFER_SIZE);
        bytes -= copy_from_user(cmdev->tx.buf, buf, bytes);

	cmdev->tx.sg.length = bytes;
        if (dma_map_sg(cmdev->dev->parent, &cmdev->tx.sg, 1,
                DMA_TO_DEVICE) != 1) {
            printk(KERN_ERR "Comento: unable to map DMA tx\n");
            break;
        }

        desc = dmaengine_prep_slave_sg(cmdev->tx.chan, &cmdev->tx.sg, 1,
                                   DMA_MEM_TO_DEV,
                                   DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
        if (!desc) {
            printk(KERN_ERR "Commento: DMA tx prep error\n");
	    dma_unmap_sg(cmdev->dev->parent, &cmdev->tx.sg, 1,
                             DMA_TO_DEVICE);
	    break;
        }
        desc->callback = comento_dma_tx_callback;
        desc->callback_param = cmdev;
	cmdev->tx.queued = true;
        gpiod_set_value(cmdev->leds->desc[0], 1);

        cmdev->tx.cookie = dmaengine_submit(desc);
        dma_async_issue_pending(cmdev->tx.chan);

	buf += bytes;
	len -= bytes;
	written_bytes += bytes;
    }
    wait_event_interruptible(cmdev->tx_wq, !cmdev->tx.queued);
#else
    char mmio_buf[COMENTO_FIFO_SIZE];
    int i;

    while (len > 0) {
        bytes = MIN(len, sizeof(mmio_buf));
        bytes -= copy_from_user(mmio_buf, buf, bytes);
        for (i = 0; i < bytes; i++) {
            wait_event_interruptible(cmdev->tx_wq,
                    !(readl(cmdev->base + COMENTO_FR) & COMENTO_FLAG_TXFF));
            writel(mmio_buf[i], cmdev->base + COMENTO_DR);
        }
        buf += bytes;
        len -= bytes;
        written_bytes += bytes;
    }
#endif
    *ppos += written_bytes;
/* 
    bytes = MIN(len, sizeof(mmio_buf));
    bytes -= copy_from_user(mmio_buf, buf, bytes);
    for (i = 0; i < bytes; i++) {
        if (readl(cmdev->base + COMENTO_FR) & COMENTO_FLAG_TXFF) {
            break;
        }
        writel(mmio_buf[i], cmdev->base + COMENTO_DR);
    }
    written_bytes += i;
    *ppos += written_bytes;
*/
    write_unlock(&cmdev->lock);

    return written_bytes;
}

static struct file_operations comento_mmio_fops = {
        .read = comento_mmio_read,
        .write = comento_mmio_write,
};

#ifdef CONFIG_DMA_ENGINE
static void comento_dma_rx_callback(void *data)
{
    struct comento_mmio_device *cmdev = (struct comento_mmio_device *)data;
    dma_unmap_sg(cmdev->dev->parent, &cmdev->rx.sg, 1, DMA_FROM_DEVICE);
    cmdev->rx.queued = false;
    gpiod_set_value(cmdev->leds->desc[1], 0);
}
#endif

static void comento_mmio_rx_work_handler(struct work_struct *work)
{
    struct comento_mmio_device *cmdev = container_of(work,
            struct comento_mmio_device, rx_work);

    write_lock(&cmdev->lock);

// split into functions
#ifdef CONFIG_DMA_ENGINE
    struct dma_async_tx_descriptor *desc;

    if (cmdev->rx.queued) {
    	dmaengine_terminate_sync(cmdev->rx.chan);
	comento_dma_rx_callback(cmdev);
    }

    if (dma_map_sg(cmdev->dev->parent, &cmdev->rx.sg, 1,
            DMA_FROM_DEVICE) != 1) {
        printk(KERN_ERR "Comento: unable to map DMA rx\n");
        goto err;
    }

    desc = dmaengine_prep_slave_sg(cmdev->rx.chan, &cmdev->rx.sg, 1,
                               DMA_DEV_TO_MEM,
                               DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
    if (!desc) {
        printk(KERN_ERR "Commento: DMA rx prep error\n");
        dma_unmap_sg(cmdev->dev->parent, &cmdev->rx.sg, 1,
                         DMA_FROM_DEVICE);
        goto err;
    }
    desc->callback = comento_dma_rx_callback;
    desc->callback_param = cmdev;
    cmdev->rx.queued = true;
    gpiod_set_value(cmdev->leds->desc[1], 1);

    cmdev->rx.cookie = dmaengine_submit(desc);
    dma_async_issue_pending(cmdev->rx.chan);

err:
#else
    unsigned long flag;
    int i;
    for (i = 0, flag = readl(cmdev->base + COMENTO_FR);
            !(flag & COMENTO_FLAG_RXFE);
            flag = readl(cmdev->base + COMENTO_FR), i++) {
        cmdev->rx_buf[i] = readl(cmdev->base + COMENTO_DR);
    }
    cmdev->rx_size = i;
#endif
    write_unlock(&cmdev->lock);
}

#ifdef CONFIG_DMA_ENGINE
static void comento_mmio_rx_dma_work_handler(struct work_struct *work)
{
    struct comento_mmio_device *cmdev = container_of(work,
            struct comento_mmio_device, rx_dma_work);
    struct dma_tx_state state;

    dmaengine_pause(cmdev->rx.chan);

    dmaengine_tx_status(cmdev->rx.chan, cmdev->rx.cookie, &state);

    cmdev->rx_size = cmdev->rx.sg.length - state.residue;
    gpiod_set_value(cmdev->leds->desc[2], 1);

    if (cmdev->rx.queued) {
        dmaengine_terminate_sync(cmdev->rx.chan);
	comento_dma_rx_callback(cmdev);
    }
}
#endif

static irqreturn_t comento_mmio_interrupt(int irq, void *dev_id)
{
    struct comento_mmio_device *cmdev = dev_id;
    unsigned long mis;

    mis = readl(cmdev->base + COMENTO_MIS);
    if (mis & COMENTO_INT_TX) {
        writel(COMENTO_INT_TX, cmdev->base + COMENTO_ICR);
#ifndef CONFIG_DMA_ENGINE
        wake_up_interruptible(&cmdev->tx_wq);
#endif
        return IRQ_HANDLED;
    }
    if (mis & COMENTO_INT_RX) {
        writel(COMENTO_INT_RX, cmdev->base + COMENTO_ICR);
	schedule_work(&cmdev->rx_work);
	return IRQ_HANDLED;
    }
#ifdef CONFIG_DMA_ENGINE
    if (mis & COMENTO_INT_RX_EMPTY) {
        writel(COMENTO_INT_RX_EMPTY, cmdev->base + COMENTO_ICR);
	schedule_work(&cmdev->rx_dma_work);
	return IRQ_HANDLED;
    }
#endif

    return IRQ_NONE;
}

static irqreturn_t comento_button_interrupt(int irq, void *dev_id)
{
    struct comento_mmio_device *cmdev = dev_id;
    
    write_lock(&cmdev->lock);
    cmdev->rx_size = 0;
    gpiod_set_value(cmdev->leds->desc[2], 0);
    write_unlock(&cmdev->lock);

    return IRQ_HANDLED;
}

static void comento_mmio_remove(struct amba_device *adev)
{
    struct comento_mmio_device *cmdev = dev_get_drvdata(&adev->dev);

    if (cmdev->dev) {
        device_destroy(comento_mmio_class, cmdev->dev->devt);
	hash_del(&cmdev->hash);

        device_init_wakeup(&adev->dev, false);
        dev_pm_clear_wake_irq(&adev->dev);

#ifdef CONFIG_DMA_ENGINE
	dma_release_channel(cmdev->rx.chan);
	dma_release_channel(cmdev->tx.chan);
#endif
    }

    amba_release_regions(adev);
}

#ifdef CONFIG_DMA_ENGINE
static int comento_dma_init(struct device *dev, const char *name,
                    struct dma_slave_config *conf, struct comento_dma_buf *dma)
{
    int ret;
    dma->chan = dma_request_chan(dev, name);
    if (IS_ERR(dma->chan)) {
	ret = PTR_ERR(dma->chan);
	goto err_req;
    }

    printk(KERN_INFO "Comento: DMA %s %s\n", name, dma_chan_name(dma->chan));

    dmaengine_slave_config(dma->chan, conf);
    dma->buf = devm_kmalloc(dev, COMENTO_DMA_BUFFER_SIZE, GFP_KERNEL | GFP_DMA);
    if (!dma->buf) {
        printk(KERN_ERR "Comento: no memory for DMA %s buffer\n", name);
	ret = -ENOMEM;
	goto out;
    }
    sg_init_one(&dma->sg, dma->buf, COMENTO_DMA_BUFFER_SIZE); 

    dma->queued = false;

    return 0;

out:
    dma_release_channel(dma->chan);
err_req:
    return ret;
}


static int comento_dma_probe(struct amba_device *adev,
		             struct comento_mmio_device *cmdev)
{
    int ret;
    unsigned long flag;
    struct dma_slave_config tx_conf = {
       .dst_addr = adev->res.start + COMENTO_DR,
       .dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE,
       .direction = DMA_MEM_TO_DEV,
       .dst_maxburst = 1,
       .device_fc = false,
    };
    struct dma_slave_config rx_conf = {
       .src_addr = adev->res.start + COMENTO_DR,
       .src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE,
       .direction = DMA_DEV_TO_MEM,
       .src_maxburst = 1,
       .device_fc = false,
    };

    ret = comento_dma_init(&adev->dev, "tx", &tx_conf, &cmdev->tx);
    if (ret) {
        return ret;
    }

    ret = comento_dma_init(&adev->dev, "rx", &rx_conf, &cmdev->rx);
    if (ret) {
        return ret;
    }

    flag = readl(cmdev->base + COMENTO_IMSC);
    writel(flag | COMENTO_INT_RX_EMPTY, cmdev->base + COMENTO_IMSC);

    INIT_WORK(&cmdev->rx_dma_work, comento_mmio_rx_dma_work_handler);

    return 0;
}
#endif

#ifdef CONFIG_DMA_ENGINE
void comento_dma_memcpy(void)
{
    struct dma_chan *chan;
    char *src, *dst;
    dma_addr_t src_dma_addr, dst_dma_addr;
    struct dma_async_tx_descriptor *desc;
    dma_cap_mask_t mask;

    dma_cap_zero(mask);
    dma_cap_set(DMA_MEMCPY, mask);
    chan = dma_request_channel(mask,  NULL, NULL);

    src = kzalloc(256, GFP_KERNEL | GFP_DMA);
    snprintf(src, 256, "Hello world~!!!");
    dst = kzalloc(256, GFP_KERNEL | GFP_DMA);
    printk(KERN_ERR "chan = %x %d\n", chan, IS_ERR(chan));
    printk(KERN_ERR "before dst = %s\n", dst);

    src_dma_addr = dma_map_single(chan->device->dev, src, 256, DMA_BIDIRECTIONAL);
    dst_dma_addr = dma_map_single(chan->device->dev, dst, 256, DMA_BIDIRECTIONAL);

    desc = dmaengine_prep_dma_memcpy(chan, dst_dma_addr, src_dma_addr, 256,                                          DMA_CTRL_ACK | DMA_PREP_INTERRUPT);

    dmaengine_submit(desc);
    dma_async_issue_pending(chan);
    printk(KERN_ERR "after dst = %s\n", dst);

    dmaengine_terminate_sync(chan);

    printk(KERN_ERR "after dst = %s\n", dst);
}

#endif

static int comento_mmio_probe(struct amba_device *adev, const struct amba_id *id)
{
    int ret;
    struct comento_mmio_device *cmdev;

    ret = amba_request_regions(adev, NULL);
    if (ret)
        goto err_req;

    if (!comento_mmio_major) {
        ret = register_chrdev(0, "mmio-comento", &comento_mmio_fops);
        if (ret < 0) {
            goto out;
        }
        comento_mmio_major = ret;
    }

    if (!comento_mmio_class) {
        comento_mmio_class = class_create("comento");
        if (IS_ERR(comento_mmio_class)) {
            ret = PTR_ERR(comento_mmio_class);
            comento_mmio_class = 0;
            goto out;
        }
    }

    cmdev = devm_kzalloc(&adev->dev, sizeof(struct comento_mmio_device),
                 GFP_KERNEL);
    if (!cmdev) {
        ret = -ENOMEM;
        goto out;
    }

    cmdev->base = devm_ioremap(&adev->dev, adev->res.start,
                   resource_size(&adev->res));
    if (!cmdev->base) {
        ret = -ENOMEM;
        goto out;
    }

    cmdev->dev = device_create(comento_mmio_class, &adev->dev,
            MKDEV(comento_mmio_major, comento_mmio_minor), NULL,
            "comento-mmio%d", comento_mmio_minor);
    if (IS_ERR(cmdev->dev)) {
        ret = PTR_ERR(cmdev->dev);
	cmdev->dev = NULL;
	goto out;
    }
    cmdev->minor = comento_mmio_minor++;

    hash_add(comento_mmio_table, &cmdev->hash, cmdev->minor);

    INIT_WORK(&cmdev->rx_work, comento_mmio_rx_work_handler);
    rwlock_init(&cmdev->lock);
    init_waitqueue_head(&cmdev->tx_wq);

    dev_info(&adev->dev, "Initialized\n");

    amba_set_drvdata(adev, cmdev);

    device_init_wakeup(&adev->dev, true);

    writel(COMENTO_INT_RX | COMENTO_INT_TX, cmdev->base + COMENTO_IMSC);

    if (adev->irq[0]) {
        ret = devm_request_irq(&adev->dev, adev->irq[0], comento_mmio_interrupt,
                  0, "mmio-comento", cmdev);
        if (ret) {
            goto out;
	}
        dev_pm_set_wake_irq(&adev->dev, adev->irq[0]);
    }

#ifdef CONFIG_DMA_ENGINE
    ret = comento_dma_probe(adev, cmdev);
    if (ret) {
        goto out;
    }
#endif

#ifdef CONFIG_DMA_ENGINE
    comento_dma_memcpy();
#endif
    int irq;

    cmdev->leds = devm_gpiod_get_array(&adev->dev, "led", GPIOD_OUT_LOW);
    cmdev->button = devm_gpiod_get(&adev->dev, "button", GPIOD_IN);

    irq = gpiod_to_irq(cmdev->button);

    ret = devm_request_irq(&adev->dev, irq,  comento_button_interrupt,
		    	  IRQF_TRIGGER_RISING,
		          "mmio-comento-gpio", cmdev);

    return 0;

out:
    amba_release_regions(adev);
err_req:

    return ret;
}

static const struct amba_id comento_mmio_ids[] = {
    {
        .id = 0x000aa001,
        .mask = 0x000fffff,
        .data = NULL,
    },
    {0, 0},
};

MODULE_DEVICE_TABLE(amba, comento_mmio_ids);

static struct amba_driver comento_mmio_driver = {
    .drv = {
        .name = "mmio-comento",
    },
    .id_table = comento_mmio_ids,
    .probe = comento_mmio_probe,
    .remove = comento_mmio_remove,
};

module_amba_driver(comento_mmio_driver);

MODULE_AUTHOR("DDunAnt<ddunant@comento.com");
MODULE_DESCRIPTION("ARM AMBA Comento MMIO Driver");
MODULE_LICENSE("GPL");
