/*
 * ARM Versatile Express emulation.
 *
 * Copyright (c) 2010 - 2011 B Labs Ltd.
 * Copyright (c) 2011 Linaro Limited
 * Written by Bahadir Balban, Amit Mahajan, Peter Maydell
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *  Contributions after 2012-01-13 are licensed under the terms of the
 *  GNU GPL, version 2 or (at your option) any later version.
 */

#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "hw/arm/primecell.h"
#include "hw/devices.h"
#include "net/net.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "exec/address-spaces.h"
#include "sysemu/blockdev.h"
#include "hw/block/flash.h"
#include "sysemu/device_tree.h"
#include <libfdt.h>

#define VEXPRESS_BOARD_ID 0x8e0
#define VEXPRESS_FLASH_SIZE (64 * 1024 * 1024)
#define VEXPRESS_FLASH_SECT_SIZE (256 * 1024)

/* Number of virtio transports to create (0..8; limited by
 * number of available IRQ lines).
 */
#define NUM_VIRTIO_TRANSPORTS 4

/* Address maps for peripherals:
 * the Versatile Express motherboard has two possible maps,
 * the "legacy" one (used for A9) and the "Cortex-A Series"
 * map (used for newer cores).
 * Individual daughterboards can also have different maps for
 * their peripherals.
 */

enum {
    VE_SYSREGS,
    VE_SP810,
    VE_SERIALPCI,
    VE_PL041,
    VE_MMCI,
    VE_KMI0,
    VE_KMI1,
    VE_UART0,
    VE_UART1,
    VE_UART2,
    VE_UART3,
    VE_WDT,
    VE_TIMER01,
    VE_TIMER23,
    VE_SERIALDVI,
    VE_RTC,
    VE_COMPACTFLASH,
    VE_CLCD,
    VE_NORFLASH0,
    VE_NORFLASH1,
    VE_NORFLASHALIAS,
    VE_SRAM,
    VE_VIDEORAM,
    VE_ETHERNET,
    VE_USB,
    VE_DAPROM,
    VE_VIRTIO,
};

static hwaddr motherboard_legacy_map[] = {
    /* CS7: 0x10000000 .. 0x10020000 */
    [VE_SYSREGS] = 0x10000000,
    [VE_SP810] = 0x10001000,
    [VE_SERIALPCI] = 0x10002000,
    [VE_PL041] = 0x10004000,
    [VE_MMCI] = 0x10005000,
    [VE_KMI0] = 0x10006000,
    [VE_KMI1] = 0x10007000,
    [VE_UART0] = 0x10009000,
    [VE_UART1] = 0x1000a000,
    [VE_UART2] = 0x1000b000,
    [VE_UART3] = 0x1000c000,
    [VE_WDT] = 0x1000f000,
    [VE_TIMER01] = 0x10011000,
    [VE_TIMER23] = 0x10012000,
    [VE_VIRTIO] = 0x10013000,
    [VE_SERIALDVI] = 0x10016000,
    [VE_RTC] = 0x10017000,
    [VE_COMPACTFLASH] = 0x1001a000,
    [VE_CLCD] = 0x1001f000,
    /* CS0: 0x40000000 .. 0x44000000 */
    [VE_NORFLASH0] = 0x40000000,
    /* CS1: 0x44000000 .. 0x48000000 */
    [VE_NORFLASH1] = 0x44000000,
    /* CS2: 0x48000000 .. 0x4a000000 */
    [VE_SRAM] = 0x48000000,
    /* CS3: 0x4c000000 .. 0x50000000 */
    [VE_VIDEORAM] = 0x4c000000,
    [VE_ETHERNET] = 0x4e000000,
    [VE_USB] = 0x4f000000,
    [VE_NORFLASHALIAS] = -1, /* not present */
};

static hwaddr motherboard_aseries_map[] = {
    [VE_NORFLASHALIAS] = 0,
    /* CS0: 0x08000000 .. 0x0c000000 */
    [VE_NORFLASH0] = 0x08000000,
    /* CS4: 0x0c000000 .. 0x10000000 */
    [VE_NORFLASH1] = 0x0c000000,
    /* CS5: 0x10000000 .. 0x14000000 */
    /* CS1: 0x14000000 .. 0x18000000 */
    [VE_SRAM] = 0x14000000,
    /* CS2: 0x18000000 .. 0x1c000000 */
    [VE_VIDEORAM] = 0x18000000,
    [VE_ETHERNET] = 0x1a000000,
    [VE_USB] = 0x1b000000,
    /* CS3: 0x1c000000 .. 0x20000000 */
    [VE_DAPROM] = 0x1c000000,
    [VE_SYSREGS] = 0x1c010000,
    [VE_SP810] = 0x1c020000,
    [VE_SERIALPCI] = 0x1c030000,
    [VE_PL041] = 0x1c040000,
    [VE_MMCI] = 0x1c050000,
    [VE_KMI0] = 0x1c060000,
    [VE_KMI1] = 0x1c070000,
    [VE_UART0] = 0x1c090000,
    [VE_UART1] = 0x1c0a0000,
    [VE_UART2] = 0x1c0b0000,
    [VE_UART3] = 0x1c0c0000,
    [VE_WDT] = 0x1c0f0000,
    [VE_TIMER01] = 0x1c110000,
    [VE_TIMER23] = 0x1c120000,
    [VE_VIRTIO] = 0x1c130000,
    [VE_SERIALDVI] = 0x1c160000,
    [VE_RTC] = 0x1c170000,
    [VE_COMPACTFLASH] = 0x1c1a0000,
    [VE_CLCD] = 0x1c1f0000,
};

/* Structure defining the peculiarities of a specific daughterboard */

typedef struct VEDBoardInfo VEDBoardInfo;

typedef void DBoardInitFn(const VEDBoardInfo *daughterboard,
                          ram_addr_t ram_size,
                          const char *cpu_model,
                          qemu_irq *pic);

struct VEDBoardInfo {
    struct arm_boot_info bootinfo;
    const hwaddr *motherboard_map;
    hwaddr loader_start;
    const hwaddr gic_cpu_if_addr;
    uint32_t proc_id;
    uint32_t num_voltage_sensors;
    const uint32_t *voltages;
    uint32_t num_clocks;
    const uint32_t *clocks;
    DBoardInitFn *init;
};

static void a9_daughterboard_init(const VEDBoardInfo *daughterboard,
                                  ram_addr_t ram_size,
                                  const char *cpu_model,
                                  qemu_irq *pic)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *lowram = g_new(MemoryRegion, 1);
    DeviceState *dev;
    SysBusDevice *busdev;
    int n;
    qemu_irq cpu_irq[4];
    qemu_irq cpu_fiq[4];
    ram_addr_t low_ram_size;

    if (!cpu_model) {
        cpu_model = "cortex-a9";
    }

    for (n = 0; n < smp_cpus; n++) {
        ARMCPU *cpu = cpu_arm_init(cpu_model);
        if (!cpu) {
            fprintf(stderr, "Unable to find CPU definition\n");
            exit(1);
        }
        cpu_irq[n] = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ);
        cpu_fiq[n] = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ);
    }

    if (ram_size > 0x40000000) {
        /* 1GB is the maximum the address space permits */
        fprintf(stderr, "vexpress-a9: cannot model more than 1GB RAM\n");
        exit(1);
    }

    memory_region_init_ram(ram, NULL, "vexpress.highmem", ram_size);
    vmstate_register_ram_global(ram);
    low_ram_size = ram_size;
    if (low_ram_size > 0x4000000) {
        low_ram_size = 0x4000000;
    }
    /* RAM is from 0x60000000 upwards. The bottom 64MB of the
     * address space should in theory be remappable to various
     * things including ROM or RAM; we always map the RAM there.
     */
    memory_region_init_alias(lowram, NULL, "vexpress.lowmem", ram, 0, low_ram_size);
    memory_region_add_subregion(sysmem, 0x0, lowram);
    memory_region_add_subregion(sysmem, 0x60000000, ram);

    /* 0x1e000000 A9MPCore (SCU) private memory region */
    dev = qdev_create(NULL, "a9mpcore_priv");
    qdev_prop_set_uint32(dev, "num-cpu", smp_cpus);
    qdev_init_nofail(dev);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_mmio_map(busdev, 0, 0x1e000000);
    /* NOTE: TrustZone: Hook up IRQ and FIQ lines */
    for (n = 0; n < smp_cpus; n++) {
        sysbus_connect_irq(busdev, 2*n, cpu_irq[n]);
        sysbus_connect_irq(busdev, 2*n + 1, cpu_fiq[n]);
    }
    /* Interrupts [42:0] are from the motherboard;
     * [47:43] are reserved; [63:48] are daughterboard
     * peripherals. Note that some documentation numbers
     * external interrupts starting from 32 (because the
     * A9MP has internal interrupts 0..31).
     */
    for (n = 0; n < 64; n++) {
        pic[n] = qdev_get_gpio_in(dev, n);
    }

    /* Daughterboard peripherals : 0x10020000 .. 0x20000000 */

    /* 0x10020000 PL111 CLCD (daughterboard) */
    sysbus_create_simple("pl111", 0x10020000, pic[44]);

    /* 0x10060000 AXI RAM */
    /* 0x100e0000 PL341 Dynamic Memory Controller */
    /* 0x100e1000 PL354 Static Memory Controller */
    /* 0x100e2000 System Configuration Controller */

    sysbus_create_simple("sp804", 0x100e4000, pic[48]);
    /* 0x100e5000 SP805 Watchdog module */
    /* 0x100e6000 BP147 TrustZone Protection Controller */
    dev = qdev_create(NULL, "bp147");
    qdev_prop_set_uint32(dev, "len-decprot", 3);
    qdev_prop_set_uint32(dev, "decprot-mask", 0x0000ffff);
    qdev_prop_set_uint32(dev, "decprot[0]", 0x000000000);
    qdev_prop_set_uint32(dev, "decprot[1]", 0x000000000);
    qdev_prop_set_uint32(dev, "decprot[2]", 0x000000000);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x100e6000);

    /* 0x100e9000 PL301 'Fast' AXI matrix */
    /* 0x100ea000 PL301 'Slow' AXI matrix */
    /* 0x100ec000 TrustZone Address Space Controller */
    dev = qdev_create(NULL, "tzc380");
    qdev_prop_set_uint32(dev, "num-regions", 16);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x100ec000);

    /* 0x10200000 CoreSight debug APB */
    /* 0x1e00a000 PL310 L2 Cache Controller */
    sysbus_create_varargs("l2x0", 0x1e00a000, NULL);
}

/* Voltage values for SYS_CFG_VOLT daughterboard registers;
 * values are in microvolts.
 */
static const uint32_t a9_voltages[] = {
    1000000, /* VD10 : 1.0V : SoC internal logic voltage */
    1000000, /* VD10_S2 : 1.0V : PL310, L2 cache, RAM, non-PL310 logic */
    1000000, /* VD10_S3 : 1.0V : Cortex-A9, cores, MPEs, SCU, PL310 logic */
    1800000, /* VCC1V8 : 1.8V : DDR2 SDRAM, test chip DDR2 I/O supply */
    900000, /* DDR2VTT : 0.9V : DDR2 SDRAM VTT termination voltage */
    3300000, /* VCC3V3 : 3.3V : local board supply for misc external logic */
};

/* Reset values for daughterboard oscillators (in Hz) */
static const uint32_t a9_clocks[] = {
    45000000, /* AMBA AXI ACLK: 45MHz */
    23750000, /* daughterboard CLCD clock: 23.75MHz */
    66670000, /* Test chip reference clock: 66.67MHz */
};

static VEDBoardInfo a9_daughterboard = {
    .motherboard_map = motherboard_legacy_map,
    .loader_start = 0x60000000,
    .gic_cpu_if_addr = 0x1e000100,
    .proc_id = 0x0c000191,
    .num_voltage_sensors = ARRAY_SIZE(a9_voltages),
    .voltages = a9_voltages,
    .num_clocks = ARRAY_SIZE(a9_clocks),
    .clocks = a9_clocks,
    .init = a9_daughterboard_init,
};

static void a15_daughterboard_init(const VEDBoardInfo *daughterboard,
                                   ram_addr_t ram_size,
                                   const char *cpu_model,
                                   qemu_irq *pic)
{
    int n;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    qemu_irq cpu_irq[4];
    qemu_irq cpu_fiq[4];
    DeviceState *dev;
    SysBusDevice *busdev;

    if (!cpu_model) {
        cpu_model = "cortex-a15";
    }

    for (n = 0; n < smp_cpus; n++) {
        ARMCPU *cpu;

        cpu = cpu_arm_init(cpu_model);
        if (!cpu) {
            fprintf(stderr, "Unable to find CPU definition\n");
            exit(1);
        }
        cpu_irq[n] = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ);
        cpu_fiq[n] = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ);
    }

    {
        /* We have to use a separate 64 bit variable here to avoid the gcc
         * "comparison is always false due to limited range of data type"
         * warning if we are on a host where ram_addr_t is 32 bits.
         */
        uint64_t rsz = ram_size;
        if (rsz > (30ULL * 1024 * 1024 * 1024)) {
            fprintf(stderr, "vexpress-a15: cannot model more than 30GB RAM\n");
            exit(1);
        }
    }

    memory_region_init_ram(ram, NULL, "vexpress.highmem", ram_size);
    vmstate_register_ram_global(ram);
    /* RAM is from 0x80000000 upwards; there is no low-memory alias for it. */
    memory_region_add_subregion(sysmem, 0x80000000, ram);

    /* 0x2c000000 A15MPCore private memory region (GIC) */
    dev = qdev_create(NULL, "a15mpcore_priv");
    qdev_prop_set_uint32(dev, "num-cpu", smp_cpus);
    qdev_init_nofail(dev);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_mmio_map(busdev, 0, 0x2c000000);
    /* NOTE: TrustZone: Hook up IRQ and FIQ lines */
    for (n = 0; n < smp_cpus; n++) {
        sysbus_connect_irq(busdev, 2*n, cpu_irq[n]);
        sysbus_connect_irq(busdev, 2*n+1, cpu_fiq[n]);
    }
    /* Interrupts [42:0] are from the motherboard;
     * [47:43] are reserved; [63:48] are daughterboard
     * peripherals. Note that some documentation numbers
     * external interrupts starting from 32 (because there
     * are internal interrupts 0..31).
     */
    for (n = 0; n < 64; n++) {
        pic[n] = qdev_get_gpio_in(dev, n);
    }

    /* A15 daughterboard peripherals: */

    /* 0x20000000: CoreSight interfaces: not modelled */
    /* 0x2a000000: PL301 AXI interconnect: not modelled */
    /* 0x2a420000: SCC: not modelled */
    /* 0x2a430000: system counter: not modelled */
    /* 0x2b000000: HDLCD controller: not modelled */
    /* 0x2b060000: SP805 watchdog: not modelled */
    /* 0x2b0a0000: PL341 dynamic memory controller: not modelled */
    /* 0x2e000000: system SRAM */
    memory_region_init_ram(sram, NULL, "vexpress.a15sram", 0x10000);
    vmstate_register_ram_global(sram);
    memory_region_add_subregion(sysmem, 0x2e000000, sram);

    /* 0x7ffb0000: DMA330 DMA controller: not modelled */
    /* 0x7ffd0000: PL354 static memory controller: not modelled */
}

static const uint32_t a15_voltages[] = {
    900000, /* Vcore: 0.9V : CPU core voltage */
};

static const uint32_t a15_clocks[] = {
    60000000, /* OSCCLK0: 60MHz : CPU_CLK reference */
    0, /* OSCCLK1: reserved */
    0, /* OSCCLK2: reserved */
    0, /* OSCCLK3: reserved */
    40000000, /* OSCCLK4: 40MHz : external AXI master clock */
    23750000, /* OSCCLK5: 23.75MHz : HDLCD PLL reference */
    50000000, /* OSCCLK6: 50MHz : static memory controller clock */
    60000000, /* OSCCLK7: 60MHz : SYSCLK reference */
    40000000, /* OSCCLK8: 40MHz : DDR2 PLL reference */
};

static VEDBoardInfo a15_daughterboard = {
    .motherboard_map = motherboard_aseries_map,
    .loader_start = 0x80000000,
    .gic_cpu_if_addr = 0x2c002000,
    .proc_id = 0x14000237,
    .num_voltage_sensors = ARRAY_SIZE(a15_voltages),
    .voltages = a15_voltages,
    .num_clocks = ARRAY_SIZE(a15_clocks),
    .clocks = a15_clocks,
    .init = a15_daughterboard_init,
};

static int add_virtio_mmio_node(void *fdt, uint32_t acells, uint32_t scells,
                                hwaddr addr, hwaddr size, uint32_t intc,
                                int irq)
{
    /* Add a virtio_mmio node to the device tree blob:
     *   virtio_mmio@ADDRESS {
     *       compatible = "virtio,mmio";
     *       reg = <ADDRESS, SIZE>;
     *       interrupt-parent = <&intc>;
     *       interrupts = <0, irq, 1>;
     *   }
     * (Note that the format of the interrupts property is dependent on the
     * interrupt controller that interrupt-parent points to; these are for
     * the ARM GIC and indicate an SPI interrupt, rising-edge-triggered.)
     */
    int rc;
    char *nodename = g_strdup_printf("/virtio_mmio@%" PRIx64, addr);

    rc = qemu_devtree_add_subnode(fdt, nodename);
    rc |= qemu_devtree_setprop_string(fdt, nodename,
                                      "compatible", "virtio,mmio");
    rc |= qemu_devtree_setprop_sized_cells(fdt, nodename, "reg",
                                           acells, addr, scells, size);
    qemu_devtree_setprop_cells(fdt, nodename, "interrupt-parent", intc);
    qemu_devtree_setprop_cells(fdt, nodename, "interrupts", 0, irq, 1);
    g_free(nodename);
    if (rc) {
        return -1;
    }
    return 0;
}

static uint32_t find_int_controller(void *fdt)
{
    /* Find the FDT node corresponding to the interrupt controller
     * for virtio-mmio devices. We do this by scanning the fdt for
     * a node with the right compatibility, since we know there is
     * only one GIC on a vexpress board.
     * We return the phandle of the node, or 0 if none was found.
     */
    const char *compat = "arm,cortex-a9-gic";
    int offset;

    offset = fdt_node_offset_by_compatible(fdt, -1, compat);
    if (offset >= 0) {
        return fdt_get_phandle(fdt, offset);
    }
    return 0;
}

static void vexpress_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    uint32_t acells, scells, intc;
    const VEDBoardInfo *daughterboard = (const VEDBoardInfo *)info;

    acells = qemu_devtree_getprop_cell(fdt, "/", "#address-cells");
    scells = qemu_devtree_getprop_cell(fdt, "/", "#size-cells");
    intc = find_int_controller(fdt);
    if (!intc) {
        /* Not fatal, we just won't provide virtio. This will
         * happen with older device tree blobs.
         */
        fprintf(stderr, "QEMU: warning: couldn't find interrupt controller in "
                "dtb; will not include virtio-mmio devices in the dtb.\n");
    } else {
        int i;
        const hwaddr *map = daughterboard->motherboard_map;

        /* We iterate backwards here because adding nodes
         * to the dtb puts them in last-first.
         */
        for (i = NUM_VIRTIO_TRANSPORTS - 1; i >= 0; i--) {
            add_virtio_mmio_node(fdt, acells, scells,
                                 map[VE_VIRTIO] + 0x200 * i,
                                 0x200, intc, 40 + i);
        }
    }
}

static void vexpress_common_init(VEDBoardInfo *daughterboard,
                                 QEMUMachineInitArgs *args)
{
    DeviceState *dev, *sysctl, *pl041;
    qemu_irq pic[64];
    uint32_t sys_id;
    DriveInfo *dinfo;
    pflash_t *pflash0;
    ram_addr_t vram_size, sram_size;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *vram = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MemoryRegion *flashalias = g_new(MemoryRegion, 1);
    MemoryRegion *flash0mem;
    const hwaddr *map = daughterboard->motherboard_map;
    int i;

    daughterboard->init(daughterboard, args->ram_size, args->cpu_model, pic);

    /* Motherboard peripherals: the wiring is the same but the
     * addresses vary between the legacy and A-Series memory maps.
     */

    sys_id = 0x1190f500;

    sysctl = qdev_create(NULL, "realview_sysctl");
    qdev_prop_set_uint32(sysctl, "sys_id", sys_id);
    qdev_prop_set_uint32(sysctl, "proc_id", daughterboard->proc_id);
    qdev_prop_set_uint32(sysctl, "len-db-voltage",
                         daughterboard->num_voltage_sensors);
    for (i = 0; i < daughterboard->num_voltage_sensors; i++) {
        char *propname = g_strdup_printf("db-voltage[%d]", i);
        qdev_prop_set_uint32(sysctl, propname, daughterboard->voltages[i]);
        g_free(propname);
    }
    qdev_prop_set_uint32(sysctl, "len-db-clock",
                         daughterboard->num_clocks);
    for (i = 0; i < daughterboard->num_clocks; i++) {
        char *propname = g_strdup_printf("db-clock[%d]", i);
        qdev_prop_set_uint32(sysctl, propname, daughterboard->clocks[i]);
        g_free(propname);
    }
    qdev_init_nofail(sysctl);
    sysbus_mmio_map(SYS_BUS_DEVICE(sysctl), 0, map[VE_SYSREGS]);

    /* VE_SP810: not modelled */
    /* VE_SERIALPCI: not modelled */

    pl041 = qdev_create(NULL, "pl041");
    qdev_prop_set_uint32(pl041, "nc_fifo_depth", 512);
    qdev_init_nofail(pl041);
    sysbus_mmio_map(SYS_BUS_DEVICE(pl041), 0, map[VE_PL041]);
    sysbus_connect_irq(SYS_BUS_DEVICE(pl041), 0, pic[11]);

    dev = sysbus_create_varargs("pl181", map[VE_MMCI], pic[9], pic[10], NULL);
    /* Wire up MMC card detect and read-only signals */
    qdev_connect_gpio_out(dev, 0,
                          qdev_get_gpio_in(sysctl, ARM_SYSCTL_GPIO_MMC_WPROT));
    qdev_connect_gpio_out(dev, 1,
                          qdev_get_gpio_in(sysctl, ARM_SYSCTL_GPIO_MMC_CARDIN));

    sysbus_create_simple("pl050_keyboard", map[VE_KMI0], pic[12]);
    sysbus_create_simple("pl050_mouse", map[VE_KMI1], pic[13]);

    sysbus_create_simple("pl011", map[VE_UART0], pic[5]);
    sysbus_create_simple("pl011", map[VE_UART1], pic[6]);
    sysbus_create_simple("pl011", map[VE_UART2], pic[7]);
    sysbus_create_simple("pl011", map[VE_UART3], pic[8]);

    sysbus_create_simple("sp804", map[VE_TIMER01], pic[2]);
    sysbus_create_simple("sp804", map[VE_TIMER23], pic[3]);

    /* VE_SERIALDVI: not modelled */

    sysbus_create_simple("pl031", map[VE_RTC], pic[4]); /* RTC */

    /* VE_COMPACTFLASH: not modelled */

    sysbus_create_simple("pl111", map[VE_CLCD], pic[14]);

    dinfo = drive_get_next(IF_PFLASH);
    pflash0 = pflash_cfi01_register(map[VE_NORFLASH0], NULL, "vexpress.flash0",
            VEXPRESS_FLASH_SIZE, dinfo ? dinfo->bdrv : NULL,
            VEXPRESS_FLASH_SECT_SIZE,
            VEXPRESS_FLASH_SIZE / VEXPRESS_FLASH_SECT_SIZE, 4,
            0x00, 0x89, 0x00, 0x18, 0);
    if (!pflash0) {
        fprintf(stderr, "vexpress: error registering flash 0.\n");
        exit(1);
    }

    if (map[VE_NORFLASHALIAS] != -1) {
        /* Map flash 0 as an alias into low memory */
        flash0mem = sysbus_mmio_get_region(SYS_BUS_DEVICE(pflash0), 0);
        memory_region_init_alias(flashalias, NULL, "vexpress.flashalias",
                                 flash0mem, 0, VEXPRESS_FLASH_SIZE);
        memory_region_add_subregion(sysmem, map[VE_NORFLASHALIAS], flashalias);
    }

    dinfo = drive_get_next(IF_PFLASH);
    if (!pflash_cfi01_register(map[VE_NORFLASH1], NULL, "vexpress.flash1",
            VEXPRESS_FLASH_SIZE, dinfo ? dinfo->bdrv : NULL,
            VEXPRESS_FLASH_SECT_SIZE,
            VEXPRESS_FLASH_SIZE / VEXPRESS_FLASH_SECT_SIZE, 4,
            0x00, 0x89, 0x00, 0x18, 0)) {
        fprintf(stderr, "vexpress: error registering flash 1.\n");
        exit(1);
    }

    sram_size = 0x2000000;
    memory_region_init_ram(sram, NULL, "vexpress.sram", sram_size);
    vmstate_register_ram_global(sram);
    memory_region_add_subregion(sysmem, map[VE_SRAM], sram);

    vram_size = 0x800000;
    memory_region_init_ram(vram, NULL, "vexpress.vram", vram_size);
    vmstate_register_ram_global(vram);
    memory_region_add_subregion(sysmem, map[VE_VIDEORAM], vram);

    /* 0x4e000000 LAN9118 Ethernet */
    if (nd_table[0].used) {
        lan9118_init(&nd_table[0], map[VE_ETHERNET], pic[15]);
    }

    /* VE_USB: not modelled */

    /* VE_DAPROM: not modelled */

    /* Create mmio transports, so the user can create virtio backends
     * (which will be automatically plugged in to the transports). If
     * no backend is created the transport will just sit harmlessly idle.
     */
    for (i = 0; i < NUM_VIRTIO_TRANSPORTS; i++) {
        sysbus_create_simple("virtio-mmio", map[VE_VIRTIO] + 0x200 * i,
                             pic[40 + i]);
    }

    daughterboard->bootinfo.ram_size = args->ram_size;
    daughterboard->bootinfo.kernel_filename = args->kernel_filename;
    daughterboard->bootinfo.kernel_cmdline = args->kernel_cmdline;
    daughterboard->bootinfo.initrd_filename = args->initrd_filename;
    daughterboard->bootinfo.nb_cpus = smp_cpus;
    daughterboard->bootinfo.board_id = VEXPRESS_BOARD_ID;
    daughterboard->bootinfo.loader_start = daughterboard->loader_start;
    daughterboard->bootinfo.smp_loader_start = map[VE_SRAM];
    daughterboard->bootinfo.smp_bootreg_addr = map[VE_SYSREGS] + 0x30;
    daughterboard->bootinfo.gic_cpu_if_addr = daughterboard->gic_cpu_if_addr;
    daughterboard->bootinfo.modify_dtb = vexpress_modify_dtb;
    arm_load_kernel(ARM_CPU(first_cpu), &daughterboard->bootinfo);
}

static void vexpress_a9_init(QEMUMachineInitArgs *args)
{
    vexpress_common_init(&a9_daughterboard, args);
}

static void vexpress_a15_init(QEMUMachineInitArgs *args)
{
    vexpress_common_init(&a15_daughterboard, args);
}

static QEMUMachine vexpress_a9_machine = {
    .name = "vexpress-a9",
    .desc = "ARM Versatile Express for Cortex-A9",
    .init = vexpress_a9_init,
    .block_default_type = IF_SCSI,
    .max_cpus = 4,
};

static QEMUMachine vexpress_a15_machine = {
    .name = "vexpress-a15",
    .desc = "ARM Versatile Express for Cortex-A15",
    .init = vexpress_a15_init,
    .block_default_type = IF_SCSI,
    .max_cpus = 4,
};

static void vexpress_machine_init(void)
{
    qemu_register_machine(&vexpress_a9_machine);
    qemu_register_machine(&vexpress_a15_machine);
}

machine_init(vexpress_machine_init);
