#ifndef HW_RISCV_QUARD_STAR__H
#define HW_RISCV_QUARD_STAR__H

#include "hw/riscv/riscv_hart.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#include "hw/block/flash.h"

#define QUARD_STAR_CPUS_MAX 8
#define QUARD_STAR_SOCKETS_MAX 8

#define TYPE_RISCV_QUARD_STAR_MACHINE MACHINE_TYPE_NAME("quard-star")
typedef struct QuardStarState QuardStarState;
DECLARE_INSTANCE_CHECKER(QuardStarState, RISCV_VIRT_MACHINE,
                         TYPE_RISCV_QUARD_STAR_MACHINE)

struct QuardStarState {
    /*< private >*/
    MachineState parent;

    /*< public >*/
    RISCVHartArrayState soc[QUARD_STAR_SOCKETS_MAX];
    DeviceState *plic;
    PFlashCFI01 *flash;
};

enum {
    QUARD_STAR_MROM,
    QUARD_STAR_SRAM,
    QUARD_STAR_CLINT,
    QUARD_STAR_PLIC,
    QUARD_STAR_UART0,
    QUARD_STAR_UART1,
    QUARD_STAR_UART2,
    QUARD_STAR_DRAM,
    QUARD_STAR_FLASH,
};

enum {
    QUARD_STAR_UART0_IRQ = 10,  //定义了串口中断号为10
    QUARD_STAR_UART1_IRQ = 11,
    QUARD_STAR_UART2_IRQ = 12,
};

#define QUARD_STAR_PLIC_NUM_SOURCES    127
#define QUARD_STAR_PLIC_NUM_PRIORITIES 7
#define QUARD_STAR_PLIC_PRIORITY_BASE  0x04
#define QUARD_STAR_PLIC_PENDING_BASE   0x1000
#define QUARD_STAR_PLIC_ENABLE_BASE    0x2000
#define QUARD_STAR_PLIC_ENABLE_STRIDE  0x80
#define QUARD_STAR_PLIC_CONTEXT_BASE   0x200000
#define QUARD_STAR_PLIC_CONTEXT_STRIDE 0x1000

#endif