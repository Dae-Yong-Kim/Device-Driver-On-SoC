#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/hashtable.h>
#include <linux/pm_wakeirq.h>
#include <linux/amba/bus.h>

#define COMENTO_DR        0x00    /* Data register */
#define COMENTO_FR        0x04    /* Flag register */
#define COMENTO_RIS       0x08    /* Raw Interrupt Status Register */
#define COMENTO_ICR       0x0c    /* Interrupt Clear Register */
#define COMENTO_IMSC      0x10    /* Interrupt Mask Set/Clear Register */
#define COMENTO_MIS       0x14    /* Masked Interrupt Status Register */
#define COMENTO_FLAG_TXFE (1 << 3)
#define COMENTO_FLAG_TXFF (1 << 2)
#define COMENTO_FLAG_RXFE (1 << 1)
#define COMENTO_FLAG_RXFF (1 << 0)

#define COMENTO_FIFO_SIZE 32
#define COMENTO_INT_TX    (1 << 1)
#define COMENTO_INT_RX    (1 << 0)

#define MIN(a,b) ((a)<(b) ? (a):(b))

struct comento_mmio_device {
        struct device *dev;
        void __iomem *base;
        rwlock_t lock;
        unsigned int minor;
        wait_queue_head_t tx_wq; // 대기 큐의 헤드를 선언
        struct hlist_node hash;
    struct work_struct rx_work; // rx_buf 등 데이터 찾기 쉽게 하기 위해
    char rx_buf[32];            // 같은 데이터 구조체 내 work_struct 선언
    int rx_size;

};

static unsigned int comento_mmio_major;
static unsigned int comento_mmio_minor;
static struct class *comento_mmio_class;

static DEFINE_HASHTABLE(comento_mmio_table, 4);

static struct comento_mmio_device *comento_mmio_get_device(
                                                       unsigned int minor)
 {
    struct comento_mmio_device *node;
    hash_for_each_possible(comento_mmio_table, node, hash, minor) {
        if (node->minor == minor) {
            return node;
        }
    }

    return NULL;
}

static ssize_t comento_mmio_write(struct file *fp, const char __user *buf,
                                                size_t len, loff_t *ppos)
{
    struct comento_mmio_device *cmdev;
    ssize_t written_bytes = 0, bytes;
    char mmio_buf[COMENTO_FIFO_SIZE];
    int i;

    cmdev = comento_mmio_get_device(iminor(fp->f_inode));
    write_lock(&cmdev->lock);
    while (len > 0) {
        bytes = MIN(len, sizeof(mmio_buf));
        bytes -= copy_from_user(mmio_buf, buf, bytes);
        for (i = 0; i < bytes; i++) {
            wait_event_interruptible(cmdev->tx_wq,      // 버퍼가 꽉찼다면
                    !(readl(cmdev->base + COMENTO_FR) & // 대기 큐 들어가
                    COMENTO_FLAG_TXFF));      // 누군가가 깨울 때까지 대기
            writel(mmio_buf[i], cmdev->base + COMENTO_DR);
        }
        buf += bytes;
        len -= bytes;
        written_bytes += bytes;
    }
    *ppos += written_bytes;
    write_unlock(&cmdev->lock);

    return written_bytes;
}

static ssize_t comento_mmio_read(struct file *fp, char __user *buf,
                                        size_t len, loff_t *ppos)
{
    struct comento_mmio_device *cmdev;
    ssize_t read_bytes = 0;

    cmdev = comento_mmio_get_device(iminor(fp->f_inode));

    read_lock(&cmdev->lock);
    if (cmdev->rx_size <= len + *ppos) {
        len = cmdev->rx_size - *ppos;
    }
    read_bytes = len - copy_to_user(buf, cmdev->rx_buf + *ppos, len);
    *ppos += read_bytes;

    read_unlock(&cmdev->lock);

    return read_bytes;
}


static struct file_operations comento_mmio_fops = {
        .read = comento_mmio_read,
        .write = comento_mmio_write,
};

static irqreturn_t comento_mmio_interrupt(int irq, void *dev_id)
{
    struct comento_mmio_device *cmdev = dev_id;
    unsigned long mis;
    mis = readl(cmdev->base + COMENTO_MIS);

    // Masked Interrupt Register를 통해 인터럽트가 발생한 원인을 파악
    if (mis & COMENTO_INT_TX) {
        // 데이터 송신을 완전히 완료하기 전에는 인터럽트가
        // 계속 발생할 것이므로 인터럽트를 Interrupt Clear Register로 끔
        writel(COMENTO_INT_TX, cmdev->base + COMENTO_ICR);
        // 대기큐에서 이벤트를 기다리는 모든 프로세스를 깨움
        wake_up_interruptible(&cmdev->tx_wq);
        // 인터럽트가 성공적으로 처리되었음을 반환
        return IRQ_HANDLED;
    }
    if (mis & COMENTO_INT_RX) { // 인터럽트 Interrupt Clear Register로 끔
        writel(COMENTO_INT_RX, cmdev->base + COMENTO_ICR);
        schedule_work(&cmdev->rx_work); // Bottom-half 실행 예약
        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static void comento_mmio_rx_work_handler(struct work_struct *work)
{ // Bottom half 실행
    // work_struct 포인터로 work_struct가 속한 데이터 구조체를 얻어옴
    struct comento_mmio_device *cmdev = container_of(work,
            struct comento_mmio_device, rx_work);
    unsigned long flag, i;
    write_lock(&cmdev->lock); // rx_buf 다루는 동안은 모든 프로세스 배제
    for (i = 0, flag = readl(cmdev->base + COMENTO_FR);
            !(flag & COMENTO_FLAG_RXFE);
            flag = readl(cmdev->base + COMENTO_FR), i++) {
        cmdev->rx_buf[i] = readl(cmdev->base + COMENTO_DR);
    } // 버퍼가 텅 빌 때까지 Data Register에서 값을 읽어 rx_buf에 저장
    cmdev->rx_size = i;
    write_unlock(&cmdev->lock);
}


static int comento_mmio_probe(struct amba_device *adev,
                              const struct amba_id *id)
{
    int ret;
    struct comento_mmio_device *cmdev;

    ret = amba_request_regions(adev, NULL); // 사용할 물리주소 미리 요청
    if (ret) {
        goto err_req;
    }
    // 주 번호를 할당 받은 적이 없다면, register_chrdev로 할당
    if (!comento_mmio_major) {
        ret = register_chrdev(0, "mmio-comento", &comento_mmio_fops);
        if (ret < 0) {
            goto out;
        }
        comento_mmio_major = ret;
    }
    // 클래스를 할당 받은 적이 없다면 새로 추가, Uevent를 위해 사용
    if (!comento_mmio_class) {
        comento_mmio_class = class_create("comento");
        if (IS_ERR(comento_mmio_class)) {
            ret = PTR_ERR(comento_mmio_class);
            comento_mmio_class = 0;
            goto out;
        }
    }
    // dev_kzalloc을 사용하면 디바이스가 등록 해제 될 때 알아서 free 됨
    cmdev = devm_kzalloc(&adev->dev, sizeof(struct comento_mmio_device),
                 GFP_KERNEL);
    if (!cmdev) {
        ret = -ENOMEM;
        goto out;
    }
    // 디바이스 트리로 명시된 메모리 영역은 물리 주소이므로
    // 주소공간에서 바로 사용이 불가능함
    // 해당 메모리 영역을 주소공간에 매핑하고 가상주소 반환
    cmdev->base = devm_ioremap(&adev->dev, adev->res.start,
                   resource_size(&adev->res));
    if (!cmdev->base) {
        ret = -ENOMEM;
        goto out;
    }
    // Uevent를 사용하여 /dev 밑에 새로운 디바이스 노드를 추가
    cmdev->dev = device_create(comento_mmio_class, &adev->dev,
            MKDEV(comento_mmio_major, comento_mmio_minor), NULL,
            "comento-mmio%d", comento_mmio_minor);
    if (IS_ERR(cmdev->dev)) {
        ret = PTR_ERR(cmdev->dev);
        cmdev->dev = NULL;
        goto out;
    }
    //다음 디바이스를 위해 부번호 증가
    cmdev->minor = comento_mmio_minor++;
    // 부번호를 키로 사용하는 해시테이블에 디바이스 데이터 추가
    hash_add(comento_mmio_table, &cmdev->hash, cmdev->minor);
    // 읽기와 쓰기를 위한 스핀락 초기화
    rwlock_init(&cmdev->lock);

    printk(KERN_INFO "Comento MMIO device driver\n");
    amba_set_drvdata(adev, cmdev);

    device_init_wakeup(&adev->dev, true);

    // 너무 길어서 PPT에는 명시하지 않았으나
    // COMENTO_* 매크로들은 QEMU에서 했던것 처럼
    // drivers/comento/mmio.c에 정의해야 함
    writel(COMENTO_INT_RX | COMENTO_INT_TX, cmdev->base + COMENTO_IMSC);

    ret = devm_request_irq(&adev->dev, adev->irq[0],
                  comento_mmio_interrupt,
                  0, "mmio-comento", cmdev);
    if (ret) {
        goto out;
    }

    dev_pm_set_wake_irq(&adev->dev, adev->irq[0]);

    init_waitqueue_head(&cmdev->tx_wq);

    writel(COMENTO_INT_RX | COMENTO_INT_TX, cmdev->base + COMENTO_IMSC);
    INIT_WORK(&cmdev->rx_work, comento_mmio_rx_work_handler);


    return 0;
out:
    amba_release_regions(adev);
err_req:

    return ret;
}

static void comento_mmio_remove(struct amba_device *adev)
{
    struct comento_mmio_device *cmdev = dev_get_drvdata(&adev->dev);

    if (cmdev->dev) {
        device_destroy(comento_mmio_class, cmdev->dev->devt);
        hash_del(&cmdev->hash);

        device_init_wakeup(&adev->dev, false);
        dev_pm_clear_wake_irq(&adev->dev);
    }

    amba_release_regions(adev);
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

