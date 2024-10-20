#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"

#define TYPE_MMIO_COMENTO "mmio-comento"

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
// Perpheral ID – 0x001A_A001, Cell ID 0xB105_F00D
static const unsigned char comento_mmio_id[] = {0x01, 0xa0, 0x1a, 0x00,
	0x0d, 0xf0, 0x05, 0xb1};

#define COMENTO_FIFO_SIZE 32
struct fifo {
    int front, rear;
    uint8_t buf[COMENTO_FIFO_SIZE];
};

#define COMENTO_INT_TX    (1 << 1)
#define COMENTO_INT_RX    (1 << 0)
#define COMENTO_INT_RX_EMPTY    (1 << 2)

struct MMIO_COMENTO_State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t flag, ris, imsc;
    struct fifo transmit, receive;
    qemu_irq dma_tx_busy;
    qemu_irq dma_rx_busy;

};

OBJECT_DECLARE_SIMPLE_TYPE(MMIO_COMENTO_State, MMIO_COMENTO)

static void comento_mmio_update(MMIO_COMENTO_State *s)
{
    uint32_t flags = s->ris & s->imsc;
    // 원인 충족(ris) & 활성화되어있는 인터럽트(mis) 가 하나라도 있다면
    // 인터럽트 발생!
    qemu_set_irq(s->irq, flags? 1 : 0);
}
static void comento_fifo_enqueue(struct fifo *fifo, uint8_t data)
{
    fifo->buf[fifo->rear++] = data;
    fifo->rear %= COMENTO_FIFO_SIZE;
}
static uint8_t comento_fifo_dequeue(struct fifo *fifo)
{
    uint8_t ret = fifo->buf[fifo->front++];
    fifo->front %= COMENTO_FIFO_SIZE;
    return ret;
}
static bool comento_fifo_is_full(struct fifo *fifo)
{
    return fifo->front == ((fifo->rear + 1) % COMENTO_FIFO_SIZE);
}
static bool comento_fifo_is_empty(struct fifo *fifo)
{
    return fifo->front == fifo->rear;
}

static uint64_t comento_mmio_read(void *opaque, hwaddr offset,
                           unsigned size)
{
    MMIO_COMENTO_State *s = (MMIO_COMENTO_State *)opaque;
    uint64_t r = 0;
    switch (offset) {
    case COMENTO_DR:
	if (!comento_fifo_is_empty(&s->receive)) {
            r = comento_fifo_dequeue(&s->receive);
        }
        if (comento_fifo_is_empty(&s->receive)) {
            s->flag &= ~COMENTO_FLAG_RXFF;
            s->flag |= COMENTO_FLAG_RXFE;
        } 
        if (comento_fifo_is_empty(&s->receive)) {
            s->ris &= ~COMENTO_INT_RX;
            s->ris |= COMENTO_INT_RX_EMPTY;
            qemu_set_irq(s->dma_rx_busy, 1);
        } // 리눅스가 받을게 없으면 인터럽트 해제
        comento_mmio_update(s);
        break;
    case COMENTO_FR:
        r = s->flag;
        break;
    case COMENTO_RIS:
        r = s->ris;
        break;
    case COMENTO_IMSC:
        r = s->imsc;
        break;
    case COMENTO_MIS:
        r = s->ris & s->imsc;
        break;
    case 0xfe0 ... 0xfff:
        r = comento_mmio_id[(offset - 0xfe0) >> 2];
        break;
    }
    return r;

}

static void comento_mmio_write(void * opaque, hwaddr offset,
                        uint64_t value, unsigned size)
{
    MMIO_COMENTO_State *s = (MMIO_COMENTO_State *)opaque;
    switch (offset) {
    case COMENTO_DR:
        if (!comento_fifo_is_full(&s->transmit)) {
            comento_fifo_enqueue(&s->transmit, value);
        }
        s->flag &= ~COMENTO_FLAG_TXFE; // 더 이상 비어있지 않으므로 세팅
        if (comento_fifo_is_full(&s->transmit)) {
            s->flag |= COMENTO_FLAG_TXFF;
        }
	if (comento_fifo_is_full(&s->transmit)) {
            s->ris &= ~COMENTO_INT_TX;
            qemu_set_irq(s->dma_tx_busy, 1);
        } // 버퍼가 꽉차서 리눅스가 보낼 수 없다면 인터럽트 해제
        comento_mmio_update(s);
        break;
    case COMENTO_ICR:
        s->ris ^= value;
	comento_mmio_update(s);
        break;
    case COMENTO_IMSC:
        s->imsc = value;
	comento_mmio_update(s);
        break;
    }

}

static char *comento_get_data(Object *obj, Error **errp)
{
    MMIO_COMENTO_State *s = MMIO_COMENTO(obj);
    char *ret = g_malloc(COMENTO_FIFO_SIZE + 1);
    int i;

    for (i = 0; !comento_fifo_is_empty(&s->transmit); i++) {
        ret[i] = comento_fifo_dequeue(&s->transmit);
    } // 버퍼에 있는 내용을 모두 비움
    ret[i] = '\0';
    s->flag &= ~COMENTO_FLAG_TXFF; // 버퍼가 비었으므로
    s->flag |= COMENTO_FLAG_TXFE; // Flag Register에 업데이트 함

    s->ris |= COMENTO_INT_TX; // 리눅스가 보낸 내용이 모두 처리되었으므로
    comento_mmio_update(s);   // 인터럽트 발생

    qemu_set_irq(s->dma_tx_busy, 0);

    return ret;
}
// data 속성에 값을 설정할 때 호출되는 함수, 파라미터로 설정할 값이 전달됨
static void comento_set_data(Object *obj, const char *value, Error **errp)
{
    MMIO_COMENTO_State *s = MMIO_COMENTO(obj);
    int count = strlen(value);
    int i;

    if (count == 0) {
        return;
    }
    for (i = 0; i < count && !comento_fifo_is_full(&s->receive); i++) {
        comento_fifo_enqueue(&s->receive, value[i]);
    }
    s->flag &= ~COMENTO_FLAG_RXFE; // 버퍼에 데이터가 들어와서
    // 더 이상 비어있지 않음을 Flag Register에 설정
    if (comento_fifo_is_full(&s->receive)) {
        s->flag |= COMENTO_FLAG_RXFF;
    } // 버퍼가 꽉찼다면 이를 Flag Register에 설정
    s->ris |= COMENTO_INT_RX; // 리눅스가 받을게 생겼으므로 인터럽트 발생

    s->ris &= ~COMENTO_INT_RX_EMPTY;
    qemu_set_irq(s->dma_rx_busy, 0);


    comento_mmio_update(s);
}

static const MemoryRegionOps comento_mmio_ops = {
    .read = comento_mmio_read,
    .write = comento_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void comento_mmio_init(Object *obj)
{
    MMIO_COMENTO_State *s = MMIO_COMENTO(obj);
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &comento_mmio_ops,
                          s, "mmio-comento", 0x1000);

    sysbus_init_mmio(dev, &s->iomem);
    sysbus_init_irq(dev, &s->irq);

    sysbus_init_irq(dev, &s->dma_tx_busy);
    sysbus_init_irq(dev, &s->dma_rx_busy);

    s->flag = COMENTO_FLAG_TXFE | COMENTO_FLAG_RXFE;
    
    object_property_add_str(obj, "data", comento_get_data,
                                         comento_set_data);
}

static const TypeInfo comento_mmio_info = {
    .name          = TYPE_MMIO_COMENTO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MMIO_COMENTO_State),
    .instance_init = comento_mmio_init,
};

static void comento_mmio_register_types(void)
{
    type_register_static(&comento_mmio_info);
}

type_init(comento_mmio_register_types)

