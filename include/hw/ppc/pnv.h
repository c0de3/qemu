/*
 * QEMU PowerPC PowerNV various definitions
 *
 * Copyright (c) 2014-2016 BenH, IBM Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PPC_PNV_H
#define PPC_PNV_H

#include "hw/boards.h"
#include "hw/sysbus.h"
#include "hw/ipmi/ipmi.h"
#include "hw/ppc/pnv_lpc.h"
#include "hw/ppc/pnv_pnor.h"
#include "hw/ppc/pnv_psi.h"
#include "hw/ppc/pnv_occ.h"
#include "hw/ppc/pnv_homer.h"
#include "hw/ppc/pnv_xive.h"
#include "hw/ppc/pnv_core.h"
#include "hw/pci-host/pnv_phb3.h"
#include "hw/pci-host/pnv_phb4.h"

#define TYPE_PNV_CHIP "pnv-chip"
#define PNV_CHIP(obj) OBJECT_CHECK(PnvChip, (obj), TYPE_PNV_CHIP)
#define PNV_CHIP_CLASS(klass) \
     OBJECT_CLASS_CHECK(PnvChipClass, (klass), TYPE_PNV_CHIP)
#define PNV_CHIP_GET_CLASS(obj) \
     OBJECT_GET_CLASS(PnvChipClass, (obj), TYPE_PNV_CHIP)

typedef struct PnvChip {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    uint32_t     chip_id;
    uint64_t     ram_start;
    uint64_t     ram_size;

    uint32_t     nr_cores;
    uint32_t     nr_threads;
    uint64_t     cores_mask;
    PnvCore      **cores;

    uint32_t     num_phbs;

    MemoryRegion xscom_mmio;
    MemoryRegion xscom;
    AddressSpace xscom_as;

    gchar        *dt_isa_nodename;
} PnvChip;

#define TYPE_PNV8_CHIP "pnv8-chip"
#define PNV8_CHIP(obj) OBJECT_CHECK(Pnv8Chip, (obj), TYPE_PNV8_CHIP)

typedef struct Pnv8Chip {
    /*< private >*/
    PnvChip      parent_obj;

    /*< public >*/
    MemoryRegion icp_mmio;

    PnvLpcController lpc;
    Pnv8Psi      psi;
    PnvOCC       occ;
    PnvHomer     homer;

#define PNV8_CHIP_PHB3_MAX 4
    PnvPHB3      phbs[PNV8_CHIP_PHB3_MAX];

    XICSFabric    *xics;
} Pnv8Chip;

#define TYPE_PNV9_CHIP "pnv9-chip"
#define PNV9_CHIP(obj) OBJECT_CHECK(Pnv9Chip, (obj), TYPE_PNV9_CHIP)

typedef struct Pnv9Chip {
    /*< private >*/
    PnvChip      parent_obj;

    /*< public >*/
    PnvXive      xive;
    Pnv9Psi      psi;
    PnvLpcController lpc;
    PnvOCC       occ;
    PnvHomer     homer;

    uint32_t     nr_quads;
    PnvQuad      *quads;

#define PNV9_CHIP_MAX_PEC 3
    PnvPhb4PecState pecs[PNV9_CHIP_MAX_PEC];
} Pnv9Chip;

/*
 * A SMT8 fused core is a pair of SMT4 cores.
 */
#define PNV9_PIR2FUSEDCORE(pir) (((pir) >> 3) & 0xf)
#define PNV9_PIR2CHIP(pir)      (((pir) >> 8) & 0x7f)

#define TYPE_PNV10_CHIP "pnv10-chip"
#define PNV10_CHIP(obj) OBJECT_CHECK(Pnv10Chip, (obj), TYPE_PNV10_CHIP)

typedef struct Pnv10Chip {
    /*< private >*/
    PnvChip      parent_obj;

    /*< public >*/
    PnvXive2     xive;
    Pnv9Psi      psi;
    PnvLpcController lpc;
    PnvOCC       occ;

    uint32_t     nr_quads;
    PnvQuad      *quads;

} Pnv10Chip;

#define PNV10_PIR2FUSEDCORE(pir) (((pir) >> 3) & 0xf)
#define PNV10_PIR2CHIP(pir)      (((pir) >> 8) & 0x7f)

typedef struct PnvChipClass {
    /*< private >*/
    SysBusDeviceClass parent_class;

    /*< public >*/
    uint64_t     chip_cfam_id;
    uint64_t     cores_mask;
    uint32_t     num_phbs;

    DeviceRealize parent_realize;

    uint32_t (*core_pir)(PnvChip *chip, uint32_t core_id);
    void (*intc_create)(PnvChip *chip, PowerPCCPU *cpu, Error **errp);
    void (*intc_reset)(PnvChip *chip, PowerPCCPU *cpu);
    void (*intc_destroy)(PnvChip *chip, PowerPCCPU *cpu);
    void (*intc_print_info)(PnvChip *chip, PowerPCCPU *cpu, Monitor *mon);
    ISABus *(*isa_create)(PnvChip *chip, Error **errp);
    void (*dt_populate)(PnvChip *chip, void *fdt);
    void (*pic_print_info)(PnvChip *chip, Monitor *mon);
    uint64_t (*xscom_core_base)(PnvChip *chip, uint32_t core_id);
    uint32_t (*xscom_pcba)(PnvChip *chip, uint64_t addr);
} PnvChipClass;

#define PNV_CHIP_TYPE_SUFFIX "-" TYPE_PNV_CHIP
#define PNV_CHIP_TYPE_NAME(cpu_model) cpu_model PNV_CHIP_TYPE_SUFFIX

#define TYPE_PNV_CHIP_POWER8E PNV_CHIP_TYPE_NAME("power8e_v2.1")
#define PNV_CHIP_POWER8E(obj) \
    OBJECT_CHECK(PnvChip, (obj), TYPE_PNV_CHIP_POWER8E)

#define TYPE_PNV_CHIP_POWER8 PNV_CHIP_TYPE_NAME("power8_v2.0")
#define PNV_CHIP_POWER8(obj) \
    OBJECT_CHECK(PnvChip, (obj), TYPE_PNV_CHIP_POWER8)

#define TYPE_PNV_CHIP_POWER8NVL PNV_CHIP_TYPE_NAME("power8nvl_v1.0")
#define PNV_CHIP_POWER8NVL(obj) \
    OBJECT_CHECK(PnvChip, (obj), TYPE_PNV_CHIP_POWER8NVL)

#define TYPE_PNV_CHIP_POWER9 PNV_CHIP_TYPE_NAME("power9_v2.0")
#define PNV_CHIP_POWER9(obj) \
    OBJECT_CHECK(PnvChip, (obj), TYPE_PNV_CHIP_POWER9)

#define TYPE_PNV_CHIP_POWER10 PNV_CHIP_TYPE_NAME("power10_v1.0")
#define PNV_CHIP_POWER10(obj) \
    OBJECT_CHECK(PnvChip, (obj), TYPE_PNV_CHIP_POWER10)

/*
 * This generates a HW chip id depending on an index, as found on a
 * two socket system with dual chip modules :
 *
 *    0x0, 0x1, 0x10, 0x11
 *
 * 4 chips should be the maximum
 *
 * TODO: use a machine property to define the chip ids
 */
#define PNV_CHIP_HWID(i) ((((i) & 0x3e) << 3) | ((i) & 0x1))

/*
 * Converts back a HW chip id to an index. This is useful to calculate
 * the MMIO addresses of some controllers which depend on the chip id.
 */
#define PNV_CHIP_INDEX(chip)                                    \
    (((chip)->chip_id >> 2) * 2 + ((chip)->chip_id & 0x3))

PowerPCCPU *pnv_chip_find_cpu(PnvChip *chip, uint32_t pir);

#define TYPE_PNV_MACHINE       MACHINE_TYPE_NAME("powernv")
#define PNV_MACHINE(obj) \
    OBJECT_CHECK(PnvMachineState, (obj), TYPE_PNV_MACHINE)
#define PNV_MACHINE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(PnvMachineClass, obj, TYPE_PNV_MACHINE)
#define PNV_MACHINE_CLASS(klass) \
    OBJECT_CLASS_CHECK(PnvMachineClass, klass, TYPE_PNV_MACHINE)

typedef struct PnvMachineState PnvMachineState;

typedef struct PnvMachineClass {
    /*< private >*/
    MachineClass parent_class;

    /*< public >*/
    const char *compat;
    int compat_size;

    void (*dt_power_mgt)(PnvMachineState *pnv, void *fdt);
} PnvMachineClass;

struct PnvMachineState {
    /*< private >*/
    MachineState parent_obj;

    uint32_t     initrd_base;
    long         initrd_size;

    uint32_t     num_chips;
    PnvChip      **chips;

    ISABus       *isa_bus;
    uint32_t     cpld_irqstate;

    IPMIBmc      *bmc;
    Notifier     powerdown_notifier;

    PnvPnor      *pnor;

    hwaddr       fw_load_addr;
};

#define PNV_FDT_ADDR          0x01000000
#define PNV_TIMEBASE_FREQ     512000000ULL

/*
 * BMC helpers
 */
void pnv_dt_bmc_sensors(IPMIBmc *bmc, void *fdt);
void pnv_bmc_powerdown(IPMIBmc *bmc);
IPMIBmc *pnv_bmc_create(PnvPnor *pnor);
IPMIBmc *pnv_bmc_find(Error **errp);
void pnv_bmc_set_pnor(IPMIBmc *bmc, PnvPnor *pnor);

/*
 * POWER8 MMIO base addresses
 */
#define PNV_XSCOM_SIZE        0x800000000ull
#define PNV_XSCOM_BASE(chip)                                            \
    (0x0003fc0000000000ull + ((uint64_t)(chip)->chip_id) * PNV_XSCOM_SIZE)

#define PNV_OCC_COMMON_AREA_SIZE    0x0000000000800000ull
#define PNV_OCC_COMMON_AREA_BASE    0x7fff800000ull
#define PNV_OCC_SENSOR_BASE(chip)   (PNV_OCC_COMMON_AREA_BASE + \
    PNV_OCC_SENSOR_DATA_BLOCK_BASE(PNV_CHIP_INDEX(chip)))

#define PNV_HOMER_SIZE              0x0000000000400000ull
#define PNV_HOMER_BASE(chip)                                            \
    (0x7ffd800000ull + ((uint64_t)PNV_CHIP_INDEX(chip)) * PNV_HOMER_SIZE)


/*
 * XSCOM 0x20109CA defines the ICP BAR:
 *
 * 0:29   : bits 14 to 43 of address to define 1 MB region.
 * 30     : 1 to enable ICP to receive loads/stores against its BAR region
 * 31:63  : Constant 0
 *
 * Usually defined as :
 *
 *      0xffffe00200000000 -> 0x0003ffff80000000
 *      0xffffe00600000000 -> 0x0003ffff80100000
 *      0xffffe02200000000 -> 0x0003ffff80800000
 *      0xffffe02600000000 -> 0x0003ffff80900000
 */
#define PNV_ICP_SIZE         0x0000000000100000ull
#define PNV_ICP_BASE(chip)                                              \
    (0x0003ffff80000000ull + (uint64_t) PNV_CHIP_INDEX(chip) * PNV_ICP_SIZE)


#define PNV_PSIHB_SIZE       0x0000000000100000ull
#define PNV_PSIHB_BASE(chip) \
    (0x0003fffe80000000ull + (uint64_t)PNV_CHIP_INDEX(chip) * PNV_PSIHB_SIZE)

#define PNV_PSIHB_FSP_SIZE   0x0000000100000000ull
#define PNV_PSIHB_FSP_BASE(chip) \
    (0x0003ffe000000000ull + (uint64_t)PNV_CHIP_INDEX(chip) * \
     PNV_PSIHB_FSP_SIZE)

/*
 * POWER9 MMIO base addresses
 */
#define PNV9_CHIP_BASE(chip, base)   \
    ((base) + ((uint64_t) (chip)->chip_id << 42))

#define PNV9_XIVE_VC_SIZE            0x0000008000000000ull
#define PNV9_XIVE_VC_BASE(chip)      PNV9_CHIP_BASE(chip, 0x0006010000000000ull)

#define PNV9_XIVE_PC_SIZE            0x0000001000000000ull
#define PNV9_XIVE_PC_BASE(chip)      PNV9_CHIP_BASE(chip, 0x0006018000000000ull)

#define PNV9_LPCM_SIZE               0x0000000100000000ull
#define PNV9_LPCM_BASE(chip)         PNV9_CHIP_BASE(chip, 0x0006030000000000ull)

#define PNV9_PSIHB_SIZE              0x0000000000100000ull
#define PNV9_PSIHB_BASE(chip)        PNV9_CHIP_BASE(chip, 0x0006030203000000ull)

#define PNV9_XIVE_IC_SIZE            0x0000000000080000ull
#define PNV9_XIVE_IC_BASE(chip)      PNV9_CHIP_BASE(chip, 0x0006030203100000ull)

#define PNV9_XIVE_TM_SIZE            0x0000000000040000ull
#define PNV9_XIVE_TM_BASE(chip)      PNV9_CHIP_BASE(chip, 0x0006030203180000ull)

#define PNV9_PSIHB_ESB_SIZE          0x0000000000010000ull
#define PNV9_PSIHB_ESB_BASE(chip)    PNV9_CHIP_BASE(chip, 0x00060302031c0000ull)

#define PNV9_XSCOM_SIZE              0x0000000400000000ull
#define PNV9_XSCOM_BASE(chip)        PNV9_CHIP_BASE(chip, 0x00603fc00000000ull)

#define PNV9_OCC_COMMON_AREA_SIZE    0x0000000000800000ull
#define PNV9_OCC_COMMON_AREA_BASE    0x203fff800000ull
#define PNV9_OCC_SENSOR_BASE(chip)   (PNV9_OCC_COMMON_AREA_BASE +       \
    PNV_OCC_SENSOR_DATA_BLOCK_BASE(PNV_CHIP_INDEX(chip)))

#define PNV9_HOMER_SIZE              0x0000000000400000ull
#define PNV9_HOMER_BASE(chip)                                           \
    (0x203ffd800000ull + ((uint64_t)PNV_CHIP_INDEX(chip)) * PNV9_HOMER_SIZE)

/*
 * POWER10 MMIO base addresses - 16TB stride per chip
 */
#define PNV10_CHIP_BASE(chip, base)   \
    ((base) + ((uint64_t) (chip)->chip_id << 44))

#define PNV10_XSCOM_SIZE             0x0000000400000000ull
#define PNV10_XSCOM_BASE(chip)       PNV10_CHIP_BASE(chip, 0x00603fc00000000ull)

#define PNV10_LPCM_SIZE             0x0000000100000000ull
#define PNV10_LPCM_BASE(chip)       PNV10_CHIP_BASE(chip, 0x0006030000000000ull)

#define PNV10_XIVE2_IC_SIZE         0x0000000002000000ull
#define PNV10_XIVE2_IC_BASE(chip)   PNV10_CHIP_BASE(chip, 0x0006030200000000ull)

#define PNV10_PSIHB_ESB_SIZE        0x0000000000100000ull
#define PNV10_PSIHB_ESB_BASE(chip)  PNV10_CHIP_BASE(chip, 0x0006030202000000ull)

#define PNV10_PSIHB_SIZE            0x0000000000100000ull
#define PNV10_PSIHB_BASE(chip)      PNV10_CHIP_BASE(chip, 0x0006030203000000ull)

#define PNV10_XIVE2_TM_SIZE         0x0000000000040000ull
#define PNV10_XIVE2_TM_BASE(chip)   PNV10_CHIP_BASE(chip, 0x0006030203180000ull)

#define PNV10_XIVE2_NVC_SIZE        0x0000000008000000ull
#define PNV10_XIVE2_NVC_BASE(chip)  PNV10_CHIP_BASE(chip, 0x0006030208000000ull)

#define PNV10_XIVE2_NVPG_SIZE       0x0000010000000000ull
#define PNV10_XIVE2_NVPG_BASE(chip) PNV10_CHIP_BASE(chip, 0x0006040000000000ull)

#define PNV10_XIVE2_ESB_SIZE        0x0000010000000000ull
#define PNV10_XIVE2_ESB_BASE(chip)  PNV10_CHIP_BASE(chip, 0x0006050000000000ull)

#define PNV10_XIVE2_END_SIZE        0x0000020000000000ull
#define PNV10_XIVE2_END_BASE(chip)  PNV10_CHIP_BASE(chip, 0x0006060000000000ull)

#endif /* PPC_PNV_H */
