#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/hashtable.h>
#include <linux/pm_wakeirq.h>
#include <linux/amba/bus.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

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
#define COMENTO_INT_RX_EMPTY    (1 << 2)

#define COMENTO_DMA_BUFFER_SIZE 4096

#define MIN(a,b) ((a)<(b) ? (a):(b))

struct comento_dma_buf {
    struct dma_chan *chan; // DMA 채널
    struct scatterlist sg; // 스캐터 리스트
    dma_cookie_t cookie;   // DMA를 중단하기 위한 핸들
    char *buf;             // GFP_DMA로 할당 받은 영역
    bool queued;           // 현재 DMA 동작 중인지 아닌지
};

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

    struct comento_dma_buf tx;
    struct comento_dma_buf rx;
    struct work_struct rx_dma_work; // 더 이상 수신할 데이터가 없어서
    // 인터럽트를 발생된 경우, DMA를 중단하기 위한 워크 큐 (Bottom half)
};

static unsigned int comento_mmio_major;
static unsigned int comento_mmio_minor;
static struct class *comento_mmio_class;

static DEFINE_HASHTABLE(comento_mmio_table, 4);

static int comento_dma_init(struct device *dev, const char *name,
                struct dma_slave_config *conf, struct comento_dma_buf *dma)
{
    int ret;                                  // 디바이스 트리에 정의된
    dma->chan = dma_request_chan(dev, name);  // DMA 채널 요청
    if (IS_ERR(dma->chan)) {
        ret = PTR_ERR(dma->chan);
        goto err_req;
    }
    printk(KERN_INFO "Comento: DMA%s%s\n", name, dma_chan_name(dma->chan));
    dmaengine_slave_config(dma->chan, conf); // DMA 채널 설정

    //GFP_DMA 타입으로 할당 받은 후, 스캐터 리스트 생성
    dma->buf = devm_kmalloc(dev, COMENTO_DMA_BUFFER_SIZE,
                           GFP_KERNEL | GFP_DMA);
    sg_init_one(&dma->sg, dma->buf, COMENTO_DMA_BUFFER_SIZE);
    dma->queued = false; // DMA 동작 중 아님으로 설정
    return 0;
err_req:
    return ret;
}

static void comento_dma_rx_callback(void *data)
{
    struct comento_mmio_device *cmdev = (struct comento_mmio_device *)data;
    // 스캐터 리스트 주소공간에서 제거
    dma_unmap_sg(cmdev->dev->parent, &cmdev->rx.sg, 1, DMA_FROM_DEVICE);
    // DMA 동작 중이 아님으로 변경
    cmdev->rx.queued = false;
}




// 더 이상 수신 데이터가 없는 경우 DMA 작업을 중단시켜
// DMA가 불필요하게 동작하지 않도록 추가
static void comento_mmio_rx_dma_work_handler(struct work_struct *work)
{
    struct comento_mmio_device *cmdev = container_of(work,
            struct comento_mmio_device, rx_dma_work);
    struct dma_tx_state state;

    // DMA 작업을 일시 정지 시킴
    dmaengine_pause(cmdev->rx.chan);
    // 완전히 중단시키지 않는 이유는
    //여태까지 몇 바이트를 받았는지 상태를 받아오기 위함
    dmaengine_tx_status(cmdev->rx.chan, cmdev->rx.cookie, &state);
    // 여태까지 받은 크기 = 스캐터리스트 크기 – DMA 작업 남은 바이트 수
    cmdev->rx_size = cmdev->rx.sg.length - state.residue;
    // 만약 DMA가 동작 중이라면, DMA 작업을 완전히 중단시킴
    if (cmdev->rx.queued) {
        dmaengine_terminate_sync(cmdev->rx.chan);
        comento_dma_rx_callback(cmdev); // DMA 완료 함수는 직접 호출
    }
}


static int comento_dma_probe(struct amba_device *adev,
                             struct comento_mmio_device *cmdev)
{
    int ret;
    unsigned long flag;
    struct dma_slave_config tx_conf = {             // 송신 부분 설정
       .dst_addr = adev->res.start + COMENTO_DR,
       .dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE,
       .direction = DMA_MEM_TO_DEV,
       .dst_maxburst = 1,           // 한 번에 최대 1바이트만 읽도록 설정
       .device_fc = false,
    };
     struct dma_slave_config rx_conf = {
       .src_addr = adev->res.start + COMENTO_DR,
       .src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE,
       .direction = DMA_DEV_TO_MEM,                 // 수신 부분 설정
       .src_maxburst = 1,
       .device_fc = false,
    };
    ret = comento_dma_init(&adev->dev, "tx", &tx_conf, &cmdev->tx);
    if (ret) {
        return ret;
    }
    ret = comento_dma_init(&adev->dev, "rx", &rx_conf, &cmdev->rx);
    if (ret) {
        dma_release_channel(cmdev->rx.chan);
        return ret;
    }
    // 수신 버퍼가 비었을 때 인터럽트를 받도록 설정
    flag = readl(cmdev->base + COMENTO_IMSC);
    writel(flag | COMENTO_INT_RX_EMPTY, cmdev->base + COMENTO_IMSC);

    INIT_WORK(&cmdev->rx_dma_work, comento_mmio_rx_dma_work_handler);
    return 0;
}


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

static void comento_dma_tx_callback(void *data)
{
    struct comento_mmio_device *cmdev = (struct comento_mmio_device *)data;

    // 스캐터 리스트 주소공간에서 제거
    dma_unmap_sg(cmdev->dev->parent, &cmdev->tx.sg, 1, DMA_TO_DEVICE);
    // DMA 동작 중이 아님으로 변경
    cmdev->tx.queued = false;
    // DMA가 동작 중 아님으로 바뀌길 원하는 대기 큐 깨우기
    wake_up_interruptible(&cmdev->tx_wq);
}

// MMIO 하드웨어 드라이버의 송신부분을 DMA 사용하여 다시 작성해야 함
static ssize_t comento_mmio_write(struct file *fp, const char __user *buf,
                                                size_t len, loff_t *ppos)
{

    struct comento_mmio_device *cmdev;
    ssize_t written_bytes = 0, bytes;
    struct dma_async_tx_descriptor *desc;

    cmdev = comento_mmio_get_device(iminor(fp->f_inode));
    if (cmdev == NULL) {
        return written_bytes;
    }

    while (len > 0) {
        // DMA가 동작중이면 끝날때 까지 대기
        wait_event_interruptible(cmdev->tx_wq, !cmdev->tx.queued);

        bytes = MIN(len, COMENTO_DMA_BUFFER_SIZE);
        // 사용자 공간에 있는 데이터를 커널로 복사
        bytes -= copy_from_user(cmdev->tx.buf, buf, bytes);
        // 스캐터 리스트 주소 공간에 추가
        cmdev->tx.sg.length = bytes;
        if (dma_map_sg(cmdev->dev->parent, &cmdev->tx.sg, 1,
                DMA_TO_DEVICE) != 1) {
            printk(KERN_ERR "Comento: unable to map DMA tx\n");
            break;
        }

        // DMA 컨트롤러에 스캐터 리스트 설정
        desc = dmaengine_prep_slave_sg(cmdev->tx.chan, &cmdev->tx.sg, 1,
                                   DMA_MEM_TO_DEV,
                                   DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
        if (!desc) {
            printk(KERN_ERR "Commento: DMA tx prep error\n");
            dma_unmap_sg(cmdev->dev->parent, &cmdev->tx.sg, 1,
                             DMA_TO_DEVICE);
            break;
        }
        // DMA 작업이 완료되었을 때 호출될 함수 설정
        desc->callback = comento_dma_tx_callback;
        desc->callback_param = cmdev;
       // DMA 동작 중으로 설정
        cmdev->tx.queued = true;

        // DMA 작업 등록
        cmdev->tx.cookie = dmaengine_submit(desc);

        // 등록한 DMA 작업 바로 시작
        dma_async_issue_pending(cmdev->tx.chan);
        buf += bytes;
        len -= bytes;
        written_bytes += bytes;
    }
    // DMA 작업이 완료되어 DMA가 동작 중 아님으로 바뀔 때까지 기다림
    wait_event_interruptible(cmdev->tx_wq, !cmdev->tx.queued);
    *ppos += written_bytes;
    return written_bytes;
}

//MMIO 하드웨어 드라이버의 인터럽트 부분 역시 다시 작성 필요
static irqreturn_t comento_mmio_interrupt(int irq, void *dev_id)
{
    struct comento_mmio_device *cmdev = dev_id;
    unsigned long mis;

    mis = readl(cmdev->base + COMENTO_MIS);
    if (mis & COMENTO_INT_TX) {
        writel(COMENTO_INT_TX, cmdev->base + COMENTO_ICR);
        // 기존 코드에서는 송신 내용이 모두 처리되어 인터럽트가 발생할 때,
        // comento_mmio_write에서 버퍼가 꽉 차서 기다리고 있었다면
        // 해당 대기 큐를 깨우는 코드가 있었음
        // DMA에서는 DMA 동작 중이 아님으로 바뀔 때까지 기다리는 코드가
        // 추가되었으므로 더 이상 필요하지 않음
        return IRQ_HANDLED;
    }
    if (mis & COMENTO_INT_RX) {  // 수신 데이터가 들어와 인터럽트 발생
        writel(COMENTO_INT_RX, cmdev->base + COMENTO_ICR);
        // 워크 큐로 comento_mmio_rx_work_handler 실행
        schedule_work(&cmdev->rx_work);
        return IRQ_HANDLED;
    }
    // 더 이상 수신 데이터가 없는 시점을 잡기 위해 새로 추가한 인터럽트
    if (mis & COMENTO_INT_RX_EMPTY) {
        writel(COMENTO_INT_RX_EMPTY, cmdev->base + COMENTO_ICR);
        // 워크 큐로 comento_mmio_rx_dma_work_handler 실행
        schedule_work(&cmdev->rx_dma_work);
        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

// 수신 작업은 인터럽트가 발생했을 때 워크 큐로 처리됨
// MMIO 하드웨어 드라이버에서는 수신 데이터가 들어와 인터럽트 발생했을 때
// 워크 큐를 사용 하여 수신 작업을 진행하였음 (Bottom half)
// 이러한 수신 부분 역시 다시 작성 필요
static void comento_mmio_rx_work_handler(struct work_struct *work)
{
    struct dma_async_tx_descriptor *desc;
    struct comento_mmio_device *cmdev = container_of(work,
            struct comento_mmio_device, rx_work);

    write_lock(&cmdev->lock);
    // 만약 DMA가 동작 중이라면, 이전 동작은 중단 시킴
    if (cmdev->rx.queued) {
        dmaengine_terminate_sync(cmdev->rx.chan);
        comento_dma_rx_callback(cmdev); // DMA 완료 함수는 직접 호출
    }
    // 스캐터 리스트 주소 공간에 추가
    if (dma_map_sg(cmdev->dev->parent, &cmdev->rx.sg, 1,
            DMA_FROM_DEVICE) != 1) {
        printk(KERN_ERR "Comento: unable to map DMA rx\n");
        goto err;
    }
    // DMA 컨트롤러에 스캐터 리스트 설정
    desc = dmaengine_prep_slave_sg(cmdev->rx.chan, &cmdev->rx.sg, 1,
                               DMA_DEV_TO_MEM,
                               DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
    if (!desc) {
        printk(KERN_ERR "Commento: DMA rx prep error\n");
        dma_unmap_sg(cmdev->dev->parent, &cmdev->rx.sg, 1,
                         DMA_FROM_DEVICE);
        goto err;
    }
    // DMA 작업이 완료되었을 때 호출될 함수 설정
    desc->callback = comento_dma_rx_callback;
    desc->callback_param = cmdev;
    cmdev->rx.queued = true; // DMA 동작 중으로 설정

    cmdev->rx.cookie = dmaengine_submit(desc); // DMA 작업 등록
    // 등록한 DMA 작업 바로 시작
    dma_async_issue_pending(cmdev->rx.chan);
err:
    write_unlock(&cmdev->lock);
}


// MMIO 하드웨어 드라이버와 코드는 거의 동일함
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
    // 유일하게 다른 부분으로
    // 기존의 cmdev->rx_buf를 cmdev->rx.buf로 변경해주면 됨
    read_bytes = len - copy_to_user(buf, cmdev->rx.buf + *ppos, len);
    *ppos += read_bytes;

    read_unlock(&cmdev->lock);

    return read_bytes;
}


static struct file_operations comento_mmio_fops = {
        .read = comento_mmio_read,
        .write = comento_mmio_write,
};

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

    ret = comento_dma_probe(adev, cmdev);
    if (ret) {
        goto out;
    }


    ret = devm_request_irq(&adev->dev, adev->irq[0],
                  comento_mmio_interrupt,
                  0, "mmio-comento", cmdev);
    if (ret) {
        goto out;
    }

    dev_pm_set_wake_irq(&adev->dev, adev->irq[0]);

    init_waitqueue_head(&cmdev->tx_wq);

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

        dma_release_channel(cmdev->rx.chan); // MMIO 장치가 해제될 때
        dma_release_channel(cmdev->tx.chan); // 할당한 DMA 채널도 해제

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

