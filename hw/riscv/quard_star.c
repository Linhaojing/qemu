#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/char/serial.h"
#include "target/riscv/cpu.h"

#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/quard_star.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/riscv_aplic.h"
#include "hw/intc/sifive_plic.h"

#include "chardev/char.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "sysemu/tpm.h"

static const MemMapEntry quard_star_memmap[] = {
    [QUARD_STAR_MROM]  = {        0x0,        0x8000 },
    [QUARD_STAR_SRAM]  = {     0x8000,        0x8000 },
    [QUARD_STAR_CLINT] = {  0x2000000,       0x10000 },
    [QUARD_STAR_PLIC]  = {  0xc000000,     0x4000000 },
    [QUARD_STAR_UART0] = { 0x10000000,         0x100 },
    [QUARD_STAR_UART1] = { 0x10001000,         0x100 },
    [QUARD_STAR_UART2] = { 0x10002000,         0x100 },
    [QUARD_STAR_FLASH] = { 0x20000000,     0x2000000 },
    [QUARD_STAR_DRAM]  = { 0x80000000,          0x0 },
};
/* 创建CPU */
static void quard_star_cpu_create(MachineState *machine)
{
    int i, base_hartid, hart_count;
    char *soc_name;
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);

    if (QUARD_STAR_SOCKETS_MAX < riscv_socket_count(machine)) {
        error_report("number of sockets/nodes should be less than %d",
            QUARD_STAR_SOCKETS_MAX);
        exit(1);
    }

    for (i = 0; i < riscv_socket_count(machine); i++) {
        if (!riscv_socket_check_hartids(machine, i)) {
            error_report("discontinuous hartids in socket%d", i);
            exit(1);
        }

        base_hartid = riscv_socket_first_hartid(machine, i);
        if (base_hartid < 0) {
            error_report("can't find hartid base for socket%d", i);
            exit(1);
        }

        hart_count = riscv_socket_hart_count(machine, i);
        if (hart_count < 0) {
            error_report("can't find hart count for socket%d", i);
            exit(1);
        }

        soc_name = g_strdup_printf("soc%d", i);
        object_initialize_child(OBJECT(machine), soc_name, &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        g_free(soc_name);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_abort);
    }
}

static void quard_star_interrupt_controller_create(MachineState *machine)
{
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    char *plic_hart_config;
    
    riscv_aclint_swi_create(
            quard_star_memmap[QUARD_STAR_CLINT].base, 0, machine->smp.cpus, false);
    riscv_aclint_mtimer_create(quard_star_memmap[QUARD_STAR_CLINT].base + RISCV_ACLINT_SWI_SIZE,
            RISCV_ACLINT_DEFAULT_MTIMER_SIZE, 0, machine->smp.cpus,
            RISCV_ACLINT_DEFAULT_MTIMECMP, RISCV_ACLINT_DEFAULT_MTIME,
            RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ, true);
    
    plic_hart_config = riscv_plic_hart_config_string(machine->smp.cpus);
    s->plic = sifive_plic_create(
        quard_star_memmap[QUARD_STAR_PLIC].base,
        plic_hart_config,  machine->smp.cpus, 0,
        QUARD_STAR_PLIC_NUM_SOURCES,
        QUARD_STAR_PLIC_NUM_PRIORITIES,
        QUARD_STAR_PLIC_PRIORITY_BASE,
        QUARD_STAR_PLIC_PENDING_BASE,
        QUARD_STAR_PLIC_ENABLE_BASE,
        QUARD_STAR_PLIC_ENABLE_STRIDE,
        QUARD_STAR_PLIC_CONTEXT_BASE,
        QUARD_STAR_PLIC_CONTEXT_STRIDE,
        quard_star_memmap[QUARD_STAR_PLIC].size);
    g_free(plic_hart_config);
}

static void quard_star_serial_create(MachineState *machine)
{
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();

    serial_mm_init(system_memory, quard_star_memmap[QUARD_STAR_UART0].base,
        0, qdev_get_gpio_in(DEVICE(s->plic), QUARD_STAR_UART0_IRQ), 399193,
        serial_hd(0), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(system_memory, quard_star_memmap[QUARD_STAR_UART1].base,
        0, qdev_get_gpio_in(DEVICE(s->plic), QUARD_STAR_UART1_IRQ), 399193,
        serial_hd(1), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(system_memory, quard_star_memmap[QUARD_STAR_UART2].base,
        0, qdev_get_gpio_in(DEVICE(s->plic), QUARD_STAR_UART2_IRQ), 399193,
        serial_hd(2), DEVICE_LITTLE_ENDIAN);
}

static void quard_star_flash_create(MachineState *machine)
{ 
    MemoryRegion *system_memory = get_system_memory();
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    uint64_t flash_sector_size = 256 * KiB;
    DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01);

    qdev_prop_set_uint64(dev, "sector-length", flash_sector_size);
    qdev_prop_set_uint8(dev, "width", 4);
    qdev_prop_set_uint8(dev, "device-width", 2);
    qdev_prop_set_bit(dev, "big-endian", false);
    qdev_prop_set_uint16(dev, "id0", 0x89);
    qdev_prop_set_uint16(dev, "id1", 0x18);
    qdev_prop_set_uint16(dev, "id2", 0x00);
    qdev_prop_set_uint16(dev, "id3", 0x00);
    qdev_prop_set_string(dev, "name", "quard-star.flash0");
    object_property_add_child(OBJECT(s), "quard-star.flash0", OBJECT(dev));
    object_property_add_alias(OBJECT(s), "pflash0",
                              OBJECT(dev), "drive");
    s->flash = PFLASH_CFI01(dev);
    pflash_cfi01_legacy_drive(s->flash, drive_get(IF_PFLASH, 0, 0));

    assert(QEMU_IS_ALIGNED(quard_star_memmap[QUARD_STAR_FLASH].size, 
                                flash_sector_size));
    assert(quard_star_memmap[QUARD_STAR_FLASH].size/flash_sector_size <= UINT32_MAX);
    qdev_prop_set_uint32(dev, "num-blocks", 
                    quard_star_memmap[QUARD_STAR_FLASH].size / flash_sector_size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    memory_region_add_subregion(system_memory, 
                            quard_star_memmap[QUARD_STAR_FLASH].base,
                            sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
}

static void quard_star_setup_rom_reset_vec(MachineState *machine, 
                                RISCVHartArrayState *harts, hwaddr start_addr,
                                hwaddr rom_base, hwaddr rom_size,
                                uint64_t kernel_entry, uint32_t fdt_load_addr)
{
    // QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    uint32_t start_addr_hi32 = 0x00000000;

    if (!riscv_is_32bit(harts)) {
        start_addr_hi32 = start_addr >> 32;
    }
    /* reset vector */
    uint32_t reset_vec[10] = {
        0x00000297,                  /* 1:  auipc  t0, %pcrel_hi(fw_dyn) */
        0x02828613,                  /*     addi   a2, t0, %pcrel_lo(1b) */
        0xf1402573,                  /*     csrr   a0, mhartid  */
        0,
        0,
        0x00028067,                  /*     jr     t0 */
        start_addr,                  /* start: .dword */
        start_addr_hi32,
        fdt_load_addr,               /* fdt_laddr: .dword */
        0x00000000,
                                     /* fw_dyn: */
    };
    if (riscv_is_32bit(harts)) {
        reset_vec[3] = 0x0202a583;   /*     lw     a1, 32(t0) */
        reset_vec[4] = 0x0182a283;   /*     lw     t0, 24(t0) */
    } else {
        reset_vec[3] = 0x0202b583;   /*     ld     a1, 32(t0) */
        reset_vec[4] = 0x0182b283;   /*     ld     t0, 24(t0) */
    }

    /* copy in the reset vector in little_endian byte order */
    for (int i = 0; i < ARRAY_SIZE(reset_vec); i++) {
        reset_vec[i] = cpu_to_le32(reset_vec[i]);
    }

    
    // if(s->mask_rom_path){
    //     int image_size = load_image_targphys_as(s->mask_rom_path, rom_base,
    //                                         rom_size, &address_space_memory);
    //     if (image_size < 0) {
    //         error_report("Could not load mrom '%s'", s->mask_rom_path);
    //         exit(1);
    //     }
    // } else {
        rom_add_blob_fixed_as("mrom.reset", reset_vec, sizeof(reset_vec),
                          rom_base, &address_space_memory);
    // }
}

/*  创建内存 */
static void quard_star_memory_create(MachineState *machine)
{
    QuardStarState *s = RISCV_VIRT_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    //分配三片存储空间 dram sram mrom
    MemoryRegion *dram_mem = g_new(MemoryRegion, 1);  //DRAM
    MemoryRegion *sram_mem = g_new(MemoryRegion, 1);  //SRAM
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);  //MROM  


    memory_region_init_ram(dram_mem, NULL, "riscv_quard_star_board.dram",
                           machine->ram_size, &error_fatal);
    memory_region_add_subregion(system_memory, 
                                quard_star_memmap[QUARD_STAR_DRAM].base, dram_mem);

    memory_region_init_ram(sram_mem, NULL, "riscv_quard_star_board.sram",
                           quard_star_memmap[QUARD_STAR_SRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory, 
                                quard_star_memmap[QUARD_STAR_SRAM].base, sram_mem);

    memory_region_init_rom(mask_rom, NULL, "riscv_quard_star_board.mrom",
                           quard_star_memmap[QUARD_STAR_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, 
                                quard_star_memmap[QUARD_STAR_MROM].base, mask_rom);

    quard_star_setup_rom_reset_vec(machine, &s->soc[0],
                              quard_star_memmap[QUARD_STAR_FLASH].base,
                              quard_star_memmap[QUARD_STAR_MROM].base,
                              quard_star_memmap[QUARD_STAR_MROM].size,
                              0x0, 0x0);
}
/* quard-star 初始化各种硬件 */

static void quard_star_machine_init(MachineState *machine)
{
   //创建CPU
   quard_star_cpu_create(machine);
    // 创建中断器
   quard_star_interrupt_controller_create(machine);
   quard_star_flash_create(machine);
   // 创建主存
   quard_star_memory_create(machine);

    // 创建串口
   quard_star_serial_create(machine);
}

static void quard_star_machine_instance_init(Object *obj)
{

}

/* 创建machine */
static void quard_star_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V Quard Star board";
    mc->init = quard_star_machine_init;
    mc->max_cpus = QUARD_STAR_CPUS_MAX;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->pci_allow_0_address = true;
    mc->possible_cpu_arch_ids = riscv_numa_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = riscv_numa_cpu_index_to_props;
    mc->get_default_cpu_node_id = riscv_numa_get_default_cpu_node_id;
    mc->numa_mem_supported = true;
}
/* 注册 quard-star */
static const TypeInfo quard_star_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("quard-star"),
    .parent     = TYPE_MACHINE,
    .class_init = quard_star_machine_class_init,
    .instance_init = quard_star_machine_instance_init,
    .instance_size = sizeof(QuardStarState),
    .interfaces = (InterfaceInfo[]) {
         { TYPE_HOTPLUG_HANDLER },
         { }
    },
};

static void quard_star_machine_init_register_types(void)
{
    type_register_static(&quard_star_machine_typeinfo);
}
type_init(quard_star_machine_init_register_types)
