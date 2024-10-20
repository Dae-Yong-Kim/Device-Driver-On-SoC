#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/option.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/arm/boot.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"
#include "target/arm/cpu.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/char/pl011.h"
#include "hw/sd/sd.h"
#include "monitor/qdev.h"

#define NUM_IRQS 256

#define ARCH_TIMER_NS_EL1_IRQ 14

#define COMENTO_BASE_MEM        0x40000000
#define COMENTO_BASE_GIC_REDIST 0x08000000
#define COMENTO_BASE_GIC_DIST   0x09000000
#define COMENTO_BASE_UART      0x09010000
#define COMENTO_BASE_MMIO      0x09011000
#define COMENTO_BASE_DMA       0x09012000
#define COMENTO_BASE_MMC       0x09013000

#define COMENTO_IRQ_UART       0
#define COMENTO_IRQ_MMIO       1
#define COMENTO_IRQ_MMC       2
#define COMENTO_IRQ_DMA        15

#define COMENTO_SIZE_GIC_REDIST 0x01000000

#define TYPE_COMENTO_MACHINE MACHINE_TYPE_NAME("comento")
#define COMENTO_CPU_TYPE ARM_CPU_TYPE_NAME("cortex-a72")


struct ComentoMachineClass {
    MachineClass parent;
};
struct ComentoMachineState {
    MachineState parent;
    struct arm_boot_info bootinfo;
    DeviceState *gic;
    DeviceState *dma;
};
OBJECT_DECLARE_TYPE(ComentoMachineState, ComentoMachineClass, \
                    COMENTO_MACHINE)

static void create_uart(const ComentoMachineState *vms, MemoryRegion *mem)
{
    hwaddr base = COMENTO_BASE_UART;
    int irq = COMENTO_IRQ_UART;
    DeviceState *dev = qdev_new(TYPE_PL011);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);

    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(s, &error_fatal);


    memory_region_add_subregion(mem, base, sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, irq, qdev_get_gpio_in(vms->gic, irq));
}

static void create_mmio(const ComentoMachineState *vms, MemoryRegion *mem)
{
    hwaddr base = COMENTO_BASE_MMIO;
    int irq = COMENTO_IRQ_MMIO;
    char name[] = "mmio-comento";
    DeviceState *dev = qdev_new(name);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);


    // qmp에서 사용할 이름 지정
    // 이름을 지정하지 않으면 /machine/unattached/device[번호]로
    // 사용해야 하며, 수동으로 번호를 찾아야 함
    qdev_set_id(dev, name, NULL);
    sysbus_realize_and_unref(s, &error_fatal);
    memory_region_add_subregion(mem, base, sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(vms->gic, irq));

    sysbus_connect_irq(s, 1, qdev_get_gpio_in(vms->dma, 0));
    sysbus_connect_irq(s, 2, qdev_get_gpio_in(vms->dma, 1));
}

static void create_dma(ComentoMachineState *vms, MemoryRegion *mem)
{
    hwaddr base = COMENTO_BASE_DMA;
    int irq = COMENTO_IRQ_DMA;
    SysBusDevice * busdev;
    int i;

    // ARM PL330 DMA 컨트롤러 장치 추가
    vms->dma = qdev_new("pl330");

    // DMA 컨트롤러가 접근할 메모리를 설정
    object_property_set_link(OBJECT(vms->dma), "memory", OBJECT(mem),
                             &error_fatal);

    busdev = SYS_BUS_DEVICE(vms->dma);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, base);
    // DMA 컨트롤러에서 오류가 발생했을 때 통지할 인터럽트 할당
    sysbus_connect_irq(busdev, 0,  qdev_get_gpio_in(vms->gic, irq));

    // DMA 컨트롤러에서 발생하는 이벤트를 처리하기 위한 인터럽트 할당
    for (i = 0; i < 16; i++) {
        sysbus_connect_irq(busdev, i + 1,
                            qdev_get_gpio_in(vms->gic, irq + i + 1));
    }
    //DMA 컨트롤러에는 총 1 + 16, 즉 17개의 인터럽트가 할당되게 됨
}

static void create_mmc(const ComentoMachineState *vms) {
    hwaddr base = COMENTO_BASE_MMC;
    int irq = COMENTO_IRQ_MMC;
    DeviceState *dev;
    DriveInfo *dinfo;
    // ARM PL181 MMC 인터페이스 장치 추가
    dev = sysbus_create_varargs("pl181", base,
                   // 송신과 수신 위한 인터럽트 추가
                   qdev_get_gpio_in(vms->gic, irq),
                   qdev_get_gpio_in(vms->gic, irq + 1),
                   NULL);
    // QEMU의 -drive format=raw,file=<이미지 이름>,if=sd 옵션으로 지정된
    // SD 카드 이미지 정보 얻기
    dinfo = drive_get(IF_SD, 0, 0);
    if (dinfo) {
        DeviceState *card;
        card = qdev_new(TYPE_SD_CARD); // SD 카드 장치 추가
        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
        qdev_realize_and_unref(card, qdev_get_child_bus(dev, "sd-bus"),
                               &error_fatal); // SD 카드 삽입
    }
}


static void create_gic(ComentoMachineState *vms)
{
    MachineState *ms = MACHINE(vms);
    SysBusDevice *gicbusdev;
    unsigned int smp_cpus = ms->smp.cpus;

    vms->gic = qdev_new("arm-gicv3");

    qdev_prop_set_uint32(vms->gic, "revision", 3);
    qdev_prop_set_uint32(vms->gic, "num-cpu", smp_cpus);
    qdev_prop_set_uint32(vms->gic, "num-irq", NUM_IRQS + 32);

    uint32_t redist_capacity = COMENTO_SIZE_GIC_REDIST / GICV3_REDIST_SIZE;
    uint32_t redist_count = MIN(smp_cpus, redist_capacity);
    qdev_prop_set_uint32(vms->gic, "len-redist-region-count", 1);
    qdev_prop_set_uint32(vms->gic, "redist-region-count[0]", redist_count);
    gicbusdev = SYS_BUS_DEVICE(vms->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, COMENTO_BASE_GIC_DIST);
    sysbus_mmio_map(gicbusdev, 1, COMENTO_BASE_GIC_REDIST);

    int i;
    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int ppibase = NUM_IRQS + i * GIC_INTERNAL + GIC_NR_SGIS;

        qdev_connect_gpio_out(cpudev, 0,
            qdev_get_gpio_in(vms->gic, ppibase + ARCH_TIMER_NS_EL1_IRQ));

        sysbus_connect_irq(gicbusdev, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    }
}

static void machcomento_init(MachineState *machine)
{
    ComentoMachineState *vms = COMENTO_MACHINE(machine);
    int index;
    unsigned int smp_cpus = machine->smp.cpus;


    MemoryRegion *sysmem = get_system_memory();
    memory_region_add_subregion(sysmem, COMENTO_BASE_MEM, machine->ram);

    vms->bootinfo.ram_size = machine->ram_size;
    vms->bootinfo.loader_start = COMENTO_BASE_MEM;

    for (index = 0; index < smp_cpus; index++) {
        Object *cpuobj;
        CPUState *cs;
        cpuobj = object_new(COMENTO_CPU_TYPE);
        object_property_set_bool(cpuobj, "has_el3", false, NULL);
        object_property_set_bool(cpuobj, "has_el2", false, NULL);
        if (object_property_find(cpuobj, "reset-cbar")) {
            object_property_set_int(cpuobj, "reset-cbar",
                                    COMENTO_BASE_GIC_DIST, &error_abort);
        }
        object_property_set_link(cpuobj, "memory", OBJECT(sysmem),
                                 &error_abort);
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        cs = CPU(cpuobj);
        cs->cpu_index = index;
        object_unref(cpuobj);
    }
    vms->bootinfo.psci_conduit = QEMU_PSCI_CONDUIT_HVC;
    create_gic(vms);
    create_uart(vms, sysmem);
    create_dma(vms, sysmem);
    create_mmio(vms, sysmem);
    create_mmc(vms);
    

    arm_load_kernel(ARM_CPU(first_cpu), machine, &vms->bootinfo);
}
static void comento_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Comento board";
    mc->init = machcomento_init;
    mc->max_cpus = 128;
    mc->block_default_type = IF_SD;
    mc->no_cdrom = 1;
    mc->minimum_page_bits = 12;
    mc->default_ram_id = "mach-comento.ram";
}
static const TypeInfo comento_machine_info = {
    .name          = "comento-machine",
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(ComentoMachineState),
    .class_size    = sizeof(ComentoMachineClass),
    .class_init    = comento_machine_class_init,
};
static void machcomento_machine_init(void)
{
    type_register_static(&comento_machine_info);
}
type_init(machcomento_machine_init);

