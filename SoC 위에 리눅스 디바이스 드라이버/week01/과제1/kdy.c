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

#define KDY_IRQ_UART1       0
#define KDY_IRQ_UART2       1

#define KDY_BASE_MEM 0x80000000
#define TYPE_KDY_MACHINE MACHINE_TYPE_NAME("kdy")

#define KDY_CPU_TYPE ARM_CPU_TYPE_NAME("cortex-a72")
#define NUM_IRQS 256

#define ARCH_TIMER_NS_EL1_IRQ 14

#define KDY_BASE_GIC_REDIST 0x03000000
#define KDY_BASE_GIC_DIST   0x04000000
#define KDY_SIZE_GIC_REDIST 0x16000000
#define KDY_BASE_UART1      0x04010000
#define KDY_BASE_UART2      0x04020000

struct KDYMachineClass {
	MachineClass parent;
};
struct KDYMachineState {
	MachineState parent;
	struct arm_boot_info bootinfo;
	DeviceState *gic;
};
OBJECT_DECLARE_TYPE(KDYMachineState, KDYMachineClass, \
		KDY_MACHINE)


static void create_gic(KDYMachineState *vms)
{
	MachineState *ms = MACHINE(vms);
	SysBusDevice *gicbusdev;
	unsigned int smp_cpus = ms->smp.cpus;

	vms->gic = qdev_new("arm-gicv3");

	qdev_prop_set_uint32(vms->gic, "revision", 3);
	qdev_prop_set_uint32(vms->gic, "num-cpu", smp_cpus);
	qdev_prop_set_uint32(vms->gic, "num-irq", NUM_IRQS + 32);

	uint32_t redist_capacity = KDY_SIZE_GIC_REDIST / GICV3_REDIST_SIZE;
	uint32_t redist_count = MIN(smp_cpus, redist_capacity);
	qdev_prop_set_uint32(vms->gic, "len-redist-region-count", 1);
	qdev_prop_set_uint32(vms->gic, "redist-region-count[0]", redist_count);
	gicbusdev = SYS_BUS_DEVICE(vms->gic);
	sysbus_realize_and_unref(gicbusdev, &error_fatal);
	sysbus_mmio_map(gicbusdev, 0, KDY_BASE_GIC_DIST);
	sysbus_mmio_map(gicbusdev, 1, KDY_BASE_GIC_REDIST);

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

static void create_uart1(const KDYMachineState *vms, MemoryRegion *mem)
{
	hwaddr base = KDY_BASE_UART1;
	int irq = KDY_IRQ_UART1;
	DeviceState *dev = qdev_new(TYPE_PL011);
	SysBusDevice *s = SYS_BUS_DEVICE(dev);

	qdev_prop_set_chr(dev, "chardev", serial_hd(0));
	sysbus_realize_and_unref(s, &error_fatal);


	memory_region_add_subregion(mem, base, sysbus_mmio_get_region(s, 0));
	sysbus_connect_irq(s, 0, qdev_get_gpio_in(vms->gic, irq));
}

static void create_uart2(const KDYMachineState *vms, MemoryRegion *mem)
{
	hwaddr base = KDY_BASE_UART2;
	int irq = KDY_IRQ_UART2;
	DeviceState *dev = qdev_new(TYPE_PL011);
	SysBusDevice *s = SYS_BUS_DEVICE(dev);

	qdev_prop_set_chr(dev, "chardev", serial_hd(1));
	sysbus_realize_and_unref(s, &error_fatal);


	memory_region_add_subregion(mem, base, sysbus_mmio_get_region(s, 0));
	sysbus_connect_irq(s, 0, qdev_get_gpio_in(vms->gic, irq));
}

static void machkdy_init(MachineState *machine)
{
	KDYMachineState *vms = KDY_MACHINE(machine);
	MemoryRegion *sysmem = get_system_memory();
	memory_region_add_subregion(sysmem, KDY_BASE_MEM, machine->ram);
	vms->bootinfo.ram_size = machine->ram_size;
	vms->bootinfo.loader_start = KDY_BASE_MEM;

	int index;
	unsigned int smp_cpus = machine->smp.cpus;

	for (index = 0; index < smp_cpus; index++) {
		Object *cpuobj;
		CPUState *cs;
		cpuobj = object_new(KDY_CPU_TYPE);
		object_property_set_bool(cpuobj, "has_el3", false, NULL);
		object_property_set_bool(cpuobj, "has_el2", false, NULL);
		if (object_property_find(cpuobj, "reset-cbar")) {
			object_property_set_int(cpuobj, "reset-cbar",
					KDY_BASE_GIC_DIST, &error_abort);
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
	create_uart1(vms, sysmem);
	create_uart2(vms, sysmem);

	arm_load_kernel(ARM_CPU(first_cpu), machine, &vms->bootinfo);
}
static void kdy_machine_class_init(ObjectClass *oc, void *data)
{
	MachineClass *mc = MACHINE_CLASS(oc);
	mc->desc = "KDY board";
	mc->init = machkdy_init;
	mc->max_cpus = 128;
	mc->block_default_type = IF_SD;
	mc->no_cdrom = 1;
	mc->minimum_page_bits = 12;
	mc->default_ram_id = "mach-kdy.ram";
}
static const TypeInfo kdy_machine_info = {
	.name          = "kdy-machine",
	.parent        = TYPE_MACHINE,
	.instance_size = sizeof(KDYMachineState),
	.class_size    = sizeof(KDYMachineClass),
	.class_init    = kdy_machine_class_init,
};
static void machkdy_machine_init(void)
{
	type_register_static(&kdy_machine_info);
}
type_init(machkdy_machine_init);
