/*
 *  CFI parallel flash with Intel command set emulation
 *
 *  Copyright (c) 2006 Thorsten Zitterell
 *  Copyright (c) 2005 Jocelyn Mayer
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

/*
 * For now, this code can emulate flashes of 1, 2 or 4 bytes width.
 * Supported commands/modes are:
 * - flash read
 * - flash write
 * - flash ID read
 * - sector erase
 * - CFI queries
 *
 * It does not support timings
 * It does not support flash interleaving
 * It does not implement software data protection as found in many real chips
 * It does not implement erase suspend/resume commands
 * It does not implement multiple sectors erase
 *
 * It does not implement much more ...
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/block/flash.h"
#include "sysemu/block-backend.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/bitops.h"
#include "exec/address-spaces.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "sysemu/sysemu.h"

#define PFLASH_BUG(fmt, ...) \
do { \
    fprintf(stderr, "PFLASH: Possible BUG - " fmt, ## __VA_ARGS__); \
    exit(1); \
} while(0)

/* #define PFLASH_DEBUG */
#ifdef PFLASH_DEBUG
#define DPRINTF(fmt, ...)                                   \
do {                                                        \
    fprintf(stderr, "PFLASH: " fmt , ## __VA_ARGS__);       \
} while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define CFI_PFLASH01(obj) OBJECT_CHECK(pflash_t, (obj), TYPE_CFI_PFLASH01)

#define PFLASH_BE          0
#define PFLASH_SECURE      1

struct pflash_t {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    BlockBackend *blk;
    uint32_t nb_blocs;
    uint64_t sector_len;
    uint8_t bank_width;
    uint8_t device_width; /* If 0, device width not specified. */
    uint8_t max_device_width;  /* max device width in bytes */
    uint32_t features;
    uint8_t wcycle; /* if 0, the flash is read normally */
    int ro;
    uint8_t cmd;
    uint8_t status;
    uint16_t ident0;
    uint16_t ident1;
    uint16_t ident2;
    uint16_t ident3;
    uint8_t cfi_len;
    uint8_t cfi_table[0x52];
    uint64_t counter;
    unsigned int writeblock_size;
    QEMUTimer *timer;
    MemoryRegion mem;
    char *name;
    void *storage;
    VMChangeStateEntry *vmstate;
    bool old_multiple_chip_handling;
};

static int pflash_post_load(void *opaque, int version_id);

static const VMStateDescription vmstate_pflash = {
    .name = "pflash_cfi01",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pflash_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(wcycle, pflash_t),
        VMSTATE_UINT8(cmd, pflash_t),
        VMSTATE_UINT8(status, pflash_t),
        VMSTATE_UINT64(counter, pflash_t),
        VMSTATE_END_OF_LIST()
    }
};

static void pflash_timer (void *opaque)
{
    pflash_t *pfl = opaque;

    DPRINTF("%s: command %02x done\n", __func__, pfl->cmd);
    /* Reset flash */
    pfl->status ^= 0x80;
    memory_region_rom_device_set_romd(&pfl->mem, true);
    pfl->wcycle = 0;
    pfl->cmd = 0;
}

/* Perform a CFI query based on the bank width of the flash.
 * If this code is called we know we have a device_width set for
 * this flash.
 */
static uint32_t pflash_cfi_query(pflash_t *pfl, hwaddr offset)
{
    int i;
    uint32_t resp = 0;
    hwaddr boff;

    /* Adjust incoming offset to match expected device-width
     * addressing. CFI query addresses are always specified in terms of
     * the maximum supported width of the device.  This means that x8
     * devices and x8/x16 devices in x8 mode behave differently.  For
     * devices that are not used at their max width, we will be
     * provided with addresses that use higher address bits than
     * expected (based on the max width), so we will shift them lower
     * so that they will match the addresses used when
     * device_width==max_device_width.
     */
    boff = offset >> (ctz32(pfl->bank_width) +
                      ctz32(pfl->max_device_width) - ctz32(pfl->device_width));

    if (boff > pfl->cfi_len) {
        return 0;
    }
    /* Now we will construct the CFI response generated by a single
     * device, then replicate that for all devices that make up the
     * bus.  For wide parts used in x8 mode, CFI query responses
     * are different than native byte-wide parts.
     */
    resp = pfl->cfi_table[boff];
    if (pfl->device_width != pfl->max_device_width) {
        /* The only case currently supported is x8 mode for a
         * wider part.
         */
        if (pfl->device_width != 1 || pfl->bank_width > 4) {
            DPRINTF("%s: Unsupported device configuration: "
                    "device_width=%d, max_device_width=%d\n",
                    __func__, pfl->device_width,
                    pfl->max_device_width);
            return 0;
        }
        /* CFI query data is repeated, rather than zero padded for
         * wide devices used in x8 mode.
         */
        for (i = 1; i < pfl->max_device_width; i++) {
            resp = deposit32(resp, 8 * i, 8, pfl->cfi_table[boff]);
        }
    }
    /* Replicate responses for each device in bank. */
    if (pfl->device_width < pfl->bank_width) {
        for (i = pfl->device_width;
             i < pfl->bank_width; i += pfl->device_width) {
            resp = deposit32(resp, 8 * i, 8 * pfl->device_width, resp);
        }
    }

    return resp;
}



/* Perform a device id query based on the bank width of the flash. */
static uint32_t pflash_devid_query(pflash_t *pfl, hwaddr offset)
{
    int i;
    uint32_t resp;
    hwaddr boff;

    /* Adjust incoming offset to match expected device-width
     * addressing. Device ID read addresses are always specified in
     * terms of the maximum supported width of the device.  This means
     * that x8 devices and x8/x16 devices in x8 mode behave
     * differently. For devices that are not used at their max width,
     * we will be provided with addresses that use higher address bits
     * than expected (based on the max width), so we will shift them
     * lower so that they will match the addresses used when
     * device_width==max_device_width.
     */
    boff = offset >> (ctz32(pfl->bank_width) +
                      ctz32(pfl->max_device_width) - ctz32(pfl->device_width));

    /* Mask off upper bits which may be used in to query block
     * or sector lock status at other addresses.
     * Offsets 2/3 are block lock status, is not emulated.
     */
    switch (boff & 0xFF) {
    case 0:
        resp = pfl->ident0;
        DPRINTF("%s: Manufacturer Code %04x\n", __func__, resp);
        break;
    case 1:
        resp = pfl->ident1;
        DPRINTF("%s: Device ID Code %04x\n", __func__, resp);
        break;
    default:
        DPRINTF("%s: Read Device Information offset=%x\n", __func__,
                (unsigned)offset);
        return 0;
        break;
    }
    /* Replicate responses for each device in bank. */
    if (pfl->device_width < pfl->bank_width) {
        for (i = pfl->device_width;
              i < pfl->bank_width; i += pfl->device_width) {
            resp = deposit32(resp, 8 * i, 8 * pfl->device_width, resp);
        }
    }

    return resp;
}

static uint32_t pflash_data_read(pflash_t *pfl, hwaddr offset,
                                 int width, int be)
{
    uint8_t *p;
    uint32_t ret;

    p = pfl->storage;
    switch (width) {
    case 1:
        ret = p[offset];
        DPRINTF("%s: data offset " TARGET_FMT_plx " %02x\n",
                __func__, offset, ret);
        break;
    case 2:
        if (be) {
            ret = p[offset] << 8;
            ret |= p[offset + 1];
        } else {
            ret = p[offset];
            ret |= p[offset + 1] << 8;
        }
        DPRINTF("%s: data offset " TARGET_FMT_plx " %04x\n",
                __func__, offset, ret);
        break;
    case 4:
        if (be) {
            ret = p[offset] << 24;
            ret |= p[offset + 1] << 16;
            ret |= p[offset + 2] << 8;
            ret |= p[offset + 3];
        } else {
            ret = p[offset];
            ret |= p[offset + 1] << 8;
            ret |= p[offset + 2] << 16;
            ret |= p[offset + 3] << 24;
        }
        DPRINTF("%s: data offset " TARGET_FMT_plx " %08x\n",
                __func__, offset, ret);
        break;
    default:
        DPRINTF("BUG in %s\n", __func__);
        abort();
    }
    return ret;
}

static uint32_t pflash_read (pflash_t *pfl, hwaddr offset,
                             int width, int be)
{
    hwaddr boff;
    uint32_t ret;

    ret = -1;

#if 0
    DPRINTF("%s: reading offset " TARGET_FMT_plx " under cmd %02x width %d\n",
            __func__, offset, pfl->cmd, width);
#endif
    switch (pfl->cmd) {
    default:
        /* This should never happen : reset state & treat it as a read */
        DPRINTF("%s: unknown command state: %x\n", __func__, pfl->cmd);
        pfl->wcycle = 0;
        pfl->cmd = 0;
        /* fall through to read code */
    case 0x00:
        /* Flash area read */
        ret = pflash_data_read(pfl, offset, width, be);
        break;
    case 0x10: /* Single byte program */
    case 0x20: /* Block erase */
    case 0x28: /* Block erase */
    case 0x40: /* single byte program */
    case 0x50: /* Clear status register */
    case 0x60: /* Block /un)lock */
    case 0x70: /* Status Register */
    case 0xe8: /* Write block */
        /* Status register read.  Return status from each device in
         * bank.
         */
        ret = pfl->status;
        if (pfl->device_width && width > pfl->device_width) {
            int shift = pfl->device_width * 8;
            while (shift + pfl->device_width * 8 <= width * 8) {
                ret |= pfl->status << shift;
                shift += pfl->device_width * 8;
            }
        } else if (!pfl->device_width && width > 2) {
            /* Handle 32 bit flash cases where device width is not
             * set. (Existing behavior before device width added.)
             */
            ret |= pfl->status << 16;
        }
        DPRINTF("%s: status %x\n", __func__, ret);
        break;
    case 0x90:
        if (!pfl->device_width) {
            /* Preserve old behavior if device width not specified */
            boff = offset & 0xFF;
            if (pfl->bank_width == 2) {
                boff = boff >> 1;
            } else if (pfl->bank_width == 4) {
                boff = boff >> 2;
            }

            switch (boff) {
            case 0:
                ret = pfl->ident0 << 8 | pfl->ident1;
                DPRINTF("%s: Manufacturer Code %04x\n", __func__, ret);
                break;
            case 1:
                ret = pfl->ident2 << 8 | pfl->ident3;
                DPRINTF("%s: Device ID Code %04x\n", __func__, ret);
                break;
            default:
                DPRINTF("%s: Read Device Information boff=%x\n", __func__,
                        (unsigned)boff);
                ret = 0;
                break;
            }
        } else {
            /* If we have a read larger than the bank_width, combine multiple
             * manufacturer/device ID queries into a single response.
             */
            int i;
            for (i = 0; i < width; i += pfl->bank_width) {
                ret = deposit32(ret, i * 8, pfl->bank_width * 8,
                                pflash_devid_query(pfl,
                                                 offset + i * pfl->bank_width));
            }
        }
        break;
    case 0x98: /* Query mode */
        if (!pfl->device_width) {
            /* Preserve old behavior if device width not specified */
            boff = offset & 0xFF;
            if (pfl->bank_width == 2) {
                boff = boff >> 1;
            } else if (pfl->bank_width == 4) {
                boff = boff >> 2;
            }

            if (boff > pfl->cfi_len) {
                ret = 0;
            } else {
                ret = pfl->cfi_table[boff];
            }
        } else {
            /* If we have a read larger than the bank_width, combine multiple
             * CFI queries into a single response.
             */
            int i;
            for (i = 0; i < width; i += pfl->bank_width) {
                ret = deposit32(ret, i * 8, pfl->bank_width * 8,
                                pflash_cfi_query(pfl,
                                                 offset + i * pfl->bank_width));
            }
        }

        break;
    }
    return ret;
}

/* update flash content on disk */
static void pflash_update(pflash_t *pfl, int offset,
                          int size)
{
    int offset_end;
    if (pfl->blk) {
        offset_end = offset + size;
        /* widen to sector boundaries */
        offset = QEMU_ALIGN_DOWN(offset, BDRV_SECTOR_SIZE);
        offset_end = QEMU_ALIGN_UP(offset_end, BDRV_SECTOR_SIZE);
        blk_pwrite(pfl->blk, offset, pfl->storage + offset,
                   offset_end - offset, 0);
    }
}

static inline void pflash_data_write(pflash_t *pfl, hwaddr offset,
                                     uint32_t value, int width, int be)
{
    uint8_t *p = pfl->storage;

    DPRINTF("%s: block write offset " TARGET_FMT_plx
            " value %x counter %016" PRIx64 "\n",
            __func__, offset, value, pfl->counter);
    switch (width) {
    case 1:
        p[offset] = value;
        break;
    case 2:
        if (be) {
            p[offset] = value >> 8;
            p[offset + 1] = value;
        } else {
            p[offset] = value;
            p[offset + 1] = value >> 8;
        }
        break;
    case 4:
        if (be) {
            p[offset] = value >> 24;
            p[offset + 1] = value >> 16;
            p[offset + 2] = value >> 8;
            p[offset + 3] = value;
        } else {
            p[offset] = value;
            p[offset + 1] = value >> 8;
            p[offset + 2] = value >> 16;
            p[offset + 3] = value >> 24;
        }
        break;
    }

}

static void pflash_write(pflash_t *pfl, hwaddr offset,
                         uint32_t value, int width, int be)
{
    uint8_t *p;
    uint8_t cmd;

    cmd = value;

    DPRINTF("%s: writing offset " TARGET_FMT_plx " value %08x width %d wcycle 0x%x\n",
            __func__, offset, value, width, pfl->wcycle);

    if (!pfl->wcycle) {
        /* Set the device in I/O access mode */
        memory_region_rom_device_set_romd(&pfl->mem, false);
    }

    switch (pfl->wcycle) {
    case 0:
        /* read mode */
        switch (cmd) {
        case 0x00: /* ??? */
            goto reset_flash;
        case 0x10: /* Single Byte Program */
        case 0x40: /* Single Byte Program */
            DPRINTF("%s: Single Byte Program\n", __func__);
            break;
        case 0x20: /* Block erase */
            p = pfl->storage;
            offset &= ~(pfl->sector_len - 1);

            DPRINTF("%s: block erase at " TARGET_FMT_plx " bytes %x\n",
                    __func__, offset, (unsigned)pfl->sector_len);

            if (!pfl->ro) {
                memset(p + offset, 0xff, pfl->sector_len);
                pflash_update(pfl, offset, pfl->sector_len);
            } else {
                pfl->status |= 0x20; /* Block erase error */
            }
            pfl->status |= 0x80; /* Ready! */
            break;
        case 0x50: /* Clear status bits */
            DPRINTF("%s: Clear status bits\n", __func__);
            pfl->status = 0x0;
            goto reset_flash;
        case 0x60: /* Block (un)lock */
            DPRINTF("%s: Block unlock\n", __func__);
            break;
        case 0x70: /* Status Register */
            DPRINTF("%s: Read status register\n", __func__);
            pfl->cmd = cmd;
            return;
        case 0x90: /* Read Device ID */
            DPRINTF("%s: Read Device information\n", __func__);
            pfl->cmd = cmd;
            return;
        case 0x98: /* CFI query */
            DPRINTF("%s: CFI query\n", __func__);
            break;
        case 0xe8: /* Write to buffer */
            DPRINTF("%s: Write to buffer\n", __func__);
            pfl->status |= 0x80; /* Ready! */
            break;
        case 0xf0: /* Probe for AMD flash */
            DPRINTF("%s: Probe for AMD flash\n", __func__);
            goto reset_flash;
        case 0xff: /* Read array mode */
            DPRINTF("%s: Read array mode\n", __func__);
            goto reset_flash;
        default:
            goto error_flash;
        }
        pfl->wcycle++;
        pfl->cmd = cmd;
        break;
    case 1:
        switch (pfl->cmd) {
        case 0x10: /* Single Byte Program */
        case 0x40: /* Single Byte Program */
            DPRINTF("%s: Single Byte Program\n", __func__);
            if (!pfl->ro) {
                pflash_data_write(pfl, offset, value, width, be);
                pflash_update(pfl, offset, width);
            } else {
                pfl->status |= 0x10; /* Programming error */
            }
            pfl->status |= 0x80; /* Ready! */
            pfl->wcycle = 0;
        break;
        case 0x20: /* Block erase */
        case 0x28:
            if (cmd == 0xd0) { /* confirm */
                pfl->wcycle = 0;
                pfl->status |= 0x80;
            } else if (cmd == 0xff) { /* read array mode */
                goto reset_flash;
            } else
                goto error_flash;

            break;
        case 0xe8:
            /* Mask writeblock size based on device width, or bank width if
             * device width not specified.
             */
            if (pfl->device_width) {
                value = extract32(value, 0, pfl->device_width * 8);
            } else {
                value = extract32(value, 0, pfl->bank_width * 8);
            }
            DPRINTF("%s: block write of %x bytes\n", __func__, value);
            pfl->counter = value;
            pfl->wcycle++;
            break;
        case 0x60:
            if (cmd == 0xd0) {
                pfl->wcycle = 0;
                pfl->status |= 0x80;
            } else if (cmd == 0x01) {
                pfl->wcycle = 0;
                pfl->status |= 0x80;
            } else if (cmd == 0xff) {
                goto reset_flash;
            } else {
                DPRINTF("%s: Unknown (un)locking command\n", __func__);
                goto reset_flash;
            }
            break;
        case 0x98:
            if (cmd == 0xff) {
                goto reset_flash;
            } else {
                DPRINTF("%s: leaving query mode\n", __func__);
            }
            break;
        default:
            goto error_flash;
        }
        break;
    case 2:
        switch (pfl->cmd) {
        case 0xe8: /* Block write */
            if (!pfl->ro) {
                pflash_data_write(pfl, offset, value, width, be);
            } else {
                pfl->status |= 0x10; /* Programming error */
            }

            pfl->status |= 0x80;

            if (!pfl->counter) {
                hwaddr mask = pfl->writeblock_size - 1;
                mask = ~mask;

                DPRINTF("%s: block write finished\n", __func__);
                pfl->wcycle++;
                if (!pfl->ro) {
                    /* Flush the entire write buffer onto backing storage.  */
                    pflash_update(pfl, offset & mask, pfl->writeblock_size);
                } else {
                    pfl->status |= 0x10; /* Programming error */
                }
            }

            pfl->counter--;
            break;
        default:
            goto error_flash;
        }
        break;
    case 3: /* Confirm mode */
        switch (pfl->cmd) {
        case 0xe8: /* Block write */
            if (cmd == 0xd0) {
                pfl->wcycle = 0;
                pfl->status |= 0x80;
            } else {
                DPRINTF("%s: unknown command for \"write block\"\n", __func__);
                PFLASH_BUG("Write block confirm");
                goto reset_flash;
            }
            break;
        default:
            goto error_flash;
        }
        break;
    default:
        /* Should never happen */
        DPRINTF("%s: invalid write state\n",  __func__);
        goto reset_flash;
    }
    return;

 error_flash:
    qemu_log_mask(LOG_UNIMP, "%s: Unimplemented flash cmd sequence "
                  "(offset " TARGET_FMT_plx ", wcycle 0x%x cmd 0x%x value 0x%x)"
                  "\n", __func__, offset, pfl->wcycle, pfl->cmd, value);

 reset_flash:
    memory_region_rom_device_set_romd(&pfl->mem, true);

    pfl->wcycle = 0;
    pfl->cmd = 0;
}


static MemTxResult pflash_mem_read_with_attrs(void *opaque, hwaddr addr, uint64_t *value,
                                              unsigned len, MemTxAttrs attrs)
{
    pflash_t *pfl = opaque;
    bool be = !!(pfl->features & (1 << PFLASH_BE));

    if ((pfl->features & (1 << PFLASH_SECURE)) && !attrs.secure) {
        *value = pflash_data_read(opaque, addr, len, be);
    } else {
        *value = pflash_read(opaque, addr, len, be);
    }
    return MEMTX_OK;
}

static MemTxResult pflash_mem_write_with_attrs(void *opaque, hwaddr addr, uint64_t value,
                                               unsigned len, MemTxAttrs attrs)
{
    pflash_t *pfl = opaque;
    bool be = !!(pfl->features & (1 << PFLASH_BE));

    if ((pfl->features & (1 << PFLASH_SECURE)) && !attrs.secure) {
        return MEMTX_ERROR;
    } else {
        pflash_write(opaque, addr, value, len, be);
        return MEMTX_OK;
    }
}

static const MemoryRegionOps pflash_cfi01_ops = {
    .read_with_attrs = pflash_mem_read_with_attrs,
    .write_with_attrs = pflash_mem_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void pflash_cfi01_realize(DeviceState *dev, Error **errp)
{
    pflash_t *pfl = CFI_PFLASH01(dev);
    uint64_t total_len;
    int ret;
    uint64_t blocks_per_device, sector_len_per_device, device_len;
    int num_devices;
    Error *local_err = NULL;

    if (pfl->sector_len == 0) {
        error_setg(errp, "attribute \"sector-length\" not specified or zero.");
        return;
    }
    if (pfl->nb_blocs == 0) {
        error_setg(errp, "attribute \"num-blocks\" not specified or zero.");
        return;
    }
    if (pfl->name == NULL) {
        error_setg(errp, "attribute \"name\" not specified.");
        return;
    }

    total_len = pfl->sector_len * pfl->nb_blocs;

    /* These are only used to expose the parameters of each device
     * in the cfi_table[].
     */
    num_devices = pfl->device_width ? (pfl->bank_width / pfl->device_width) : 1;
    if (pfl->old_multiple_chip_handling) {
        blocks_per_device = pfl->nb_blocs / num_devices;
        sector_len_per_device = pfl->sector_len;
    } else {
        blocks_per_device = pfl->nb_blocs;
        sector_len_per_device = pfl->sector_len / num_devices;
    }
    device_len = sector_len_per_device * blocks_per_device;

    /* XXX: to be fixed */
#if 0
    if (total_len != (8 * 1024 * 1024) && total_len != (16 * 1024 * 1024) &&
        total_len != (32 * 1024 * 1024) && total_len != (64 * 1024 * 1024))
        return NULL;
#endif

    memory_region_init_rom_device(
        &pfl->mem, OBJECT(dev),
        &pflash_cfi01_ops,
        pfl,
        pfl->name, total_len, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    vmstate_register_ram(&pfl->mem, DEVICE(pfl));
    pfl->storage = memory_region_get_ram_ptr(&pfl->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &pfl->mem);

    if (pfl->blk) {
        uint64_t perm;
        pfl->ro = blk_is_read_only(pfl->blk);
        perm = BLK_PERM_CONSISTENT_READ | (pfl->ro ? 0 : BLK_PERM_WRITE);
        ret = blk_set_perm(pfl->blk, perm, BLK_PERM_ALL, errp);
        if (ret < 0) {
            return;
        }
    } else {
        pfl->ro = 0;
    }

    if (pfl->blk) {
        /* read the initial flash content */
        ret = blk_pread(pfl->blk, 0, pfl->storage, total_len);

        if (ret < 0) {
            vmstate_unregister_ram(&pfl->mem, DEVICE(pfl));
            error_setg(errp, "failed to read the initial flash content");
            return;
        }
    }

    /* Default to devices being used at their maximum device width. This was
     * assumed before the device_width support was added.
     */
    if (!pfl->max_device_width) {
        pfl->max_device_width = pfl->device_width;
    }

    pfl->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pflash_timer, pfl);
    pfl->wcycle = 0;
    pfl->cmd = 0;
    pfl->status = 0;
    /* Hardcoded CFI table */
    pfl->cfi_len = 0x52;
    /* Standard "QRY" string */
    pfl->cfi_table[0x10] = 'Q';
    pfl->cfi_table[0x11] = 'R';
    pfl->cfi_table[0x12] = 'Y';
    /* Command set (Intel) */
    pfl->cfi_table[0x13] = 0x01;
    pfl->cfi_table[0x14] = 0x00;
    /* Primary extended table address (none) */
    pfl->cfi_table[0x15] = 0x31;
    pfl->cfi_table[0x16] = 0x00;
    /* Alternate command set (none) */
    pfl->cfi_table[0x17] = 0x00;
    pfl->cfi_table[0x18] = 0x00;
    /* Alternate extended table (none) */
    pfl->cfi_table[0x19] = 0x00;
    pfl->cfi_table[0x1A] = 0x00;
    /* Vcc min */
    pfl->cfi_table[0x1B] = 0x45;
    /* Vcc max */
    pfl->cfi_table[0x1C] = 0x55;
    /* Vpp min (no Vpp pin) */
    pfl->cfi_table[0x1D] = 0x00;
    /* Vpp max (no Vpp pin) */
    pfl->cfi_table[0x1E] = 0x00;
    /* Reserved */
    pfl->cfi_table[0x1F] = 0x07;
    /* Timeout for min size buffer write */
    pfl->cfi_table[0x20] = 0x07;
    /* Typical timeout for block erase */
    pfl->cfi_table[0x21] = 0x0a;
    /* Typical timeout for full chip erase (4096 ms) */
    pfl->cfi_table[0x22] = 0x00;
    /* Reserved */
    pfl->cfi_table[0x23] = 0x04;
    /* Max timeout for buffer write */
    pfl->cfi_table[0x24] = 0x04;
    /* Max timeout for block erase */
    pfl->cfi_table[0x25] = 0x04;
    /* Max timeout for chip erase */
    pfl->cfi_table[0x26] = 0x00;
    /* Device size */
    pfl->cfi_table[0x27] = ctz32(device_len); /* + 1; */
    /* Flash device interface (8 & 16 bits) */
    pfl->cfi_table[0x28] = 0x02;
    pfl->cfi_table[0x29] = 0x00;
    /* Max number of bytes in multi-bytes write */
    if (pfl->bank_width == 1) {
        pfl->cfi_table[0x2A] = 0x08;
    } else {
        pfl->cfi_table[0x2A] = 0x0B;
    }
    pfl->writeblock_size = 1 << pfl->cfi_table[0x2A];
    if (!pfl->old_multiple_chip_handling && num_devices > 1) {
        pfl->writeblock_size *= num_devices;
    }

    pfl->cfi_table[0x2B] = 0x00;
    /* Number of erase block regions (uniform) */
    pfl->cfi_table[0x2C] = 0x01;
    /* Erase block region 1 */
    pfl->cfi_table[0x2D] = blocks_per_device - 1;
    pfl->cfi_table[0x2E] = (blocks_per_device - 1) >> 8;
    pfl->cfi_table[0x2F] = sector_len_per_device >> 8;
    pfl->cfi_table[0x30] = sector_len_per_device >> 16;

    /* Extended */
    pfl->cfi_table[0x31] = 'P';
    pfl->cfi_table[0x32] = 'R';
    pfl->cfi_table[0x33] = 'I';

    pfl->cfi_table[0x34] = '1';
    pfl->cfi_table[0x35] = '0';

    pfl->cfi_table[0x36] = 0x00;
    pfl->cfi_table[0x37] = 0x00;
    pfl->cfi_table[0x38] = 0x00;
    pfl->cfi_table[0x39] = 0x00;

    pfl->cfi_table[0x3a] = 0x00;

    pfl->cfi_table[0x3b] = 0x00;
    pfl->cfi_table[0x3c] = 0x00;

    pfl->cfi_table[0x3f] = 0x01; /* Number of protection fields */
}

static Property pflash_cfi01_properties[] = {
    DEFINE_PROP_DRIVE("drive", struct pflash_t, blk),
    /* num-blocks is the number of blocks actually visible to the guest,
     * ie the total size of the device divided by the sector length.
     * If we're emulating flash devices wired in parallel the actual
     * number of blocks per indvidual device will differ.
     */
    DEFINE_PROP_UINT32("num-blocks", struct pflash_t, nb_blocs, 0),
    DEFINE_PROP_UINT64("sector-length", struct pflash_t, sector_len, 0),
    /* width here is the overall width of this QEMU device in bytes.
     * The QEMU device may be emulating a number of flash devices
     * wired up in parallel; the width of each individual flash
     * device should be specified via device-width. If the individual
     * devices have a maximum width which is greater than the width
     * they are being used for, this maximum width should be set via
     * max-device-width (which otherwise defaults to device-width).
     * So for instance a 32-bit wide QEMU flash device made from four
     * 16-bit flash devices used in 8-bit wide mode would be configured
     * with width = 4, device-width = 1, max-device-width = 2.
     *
     * If device-width is not specified we default to backwards
     * compatible behaviour which is a bad emulation of two
     * 16 bit devices making up a 32 bit wide QEMU device. This
     * is deprecated for new uses of this device.
     */
    DEFINE_PROP_UINT8("width", struct pflash_t, bank_width, 0),
    DEFINE_PROP_UINT8("device-width", struct pflash_t, device_width, 0),
    DEFINE_PROP_UINT8("max-device-width", struct pflash_t, max_device_width, 0),
    DEFINE_PROP_BIT("big-endian", struct pflash_t, features, PFLASH_BE, 0),
    DEFINE_PROP_BIT("secure", struct pflash_t, features, PFLASH_SECURE, 0),
    DEFINE_PROP_UINT16("id0", struct pflash_t, ident0, 0),
    DEFINE_PROP_UINT16("id1", struct pflash_t, ident1, 0),
    DEFINE_PROP_UINT16("id2", struct pflash_t, ident2, 0),
    DEFINE_PROP_UINT16("id3", struct pflash_t, ident3, 0),
    DEFINE_PROP_STRING("name", struct pflash_t, name),
    DEFINE_PROP_BOOL("old-multiple-chip-handling", struct pflash_t,
                     old_multiple_chip_handling, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void pflash_cfi01_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pflash_cfi01_realize;
    dc->props = pflash_cfi01_properties;
    dc->vmsd = &vmstate_pflash;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    /*
     * FIXME: Set only because we are not sure yet if this device
     * will be outside the q35 sysbus whitelist.
     */
    dc->user_creatable = true;
}


static const TypeInfo pflash_cfi01_info = {
    .name           = TYPE_CFI_PFLASH01,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(struct pflash_t),
    .class_init     = pflash_cfi01_class_init,
};

static void pflash_cfi01_register_types(void)
{
    type_register_static(&pflash_cfi01_info);
}

type_init(pflash_cfi01_register_types)

pflash_t *pflash_cfi01_register(hwaddr base,
                                DeviceState *qdev, const char *name,
                                hwaddr size,
                                BlockBackend *blk,
                                uint32_t sector_len, int nb_blocs,
                                int bank_width, uint16_t id0, uint16_t id1,
                                uint16_t id2, uint16_t id3, int be)
{
    DeviceState *dev = qdev_create(NULL, TYPE_CFI_PFLASH01);

    if (blk) {
        qdev_prop_set_drive(dev, "drive", blk, &error_abort);
    }
    qdev_prop_set_uint32(dev, "num-blocks", nb_blocs);
    qdev_prop_set_uint64(dev, "sector-length", sector_len);
    qdev_prop_set_uint8(dev, "width", bank_width);
    qdev_prop_set_bit(dev, "big-endian", !!be);
    qdev_prop_set_uint16(dev, "id0", id0);
    qdev_prop_set_uint16(dev, "id1", id1);
    qdev_prop_set_uint16(dev, "id2", id2);
    qdev_prop_set_uint16(dev, "id3", id3);
    qdev_prop_set_string(dev, "name", name);
    qdev_init_nofail(dev);

    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return CFI_PFLASH01(dev);
}

MemoryRegion *pflash_cfi01_get_memory(pflash_t *fl)
{
    return &fl->mem;
}

static void postload_update_cb(void *opaque, int running, RunState state)
{
    pflash_t *pfl = opaque;

    /* This is called after bdrv_invalidate_cache_all.  */
    qemu_del_vm_change_state_handler(pfl->vmstate);
    pfl->vmstate = NULL;

    DPRINTF("%s: updating bdrv for %s\n", __func__, pfl->name);
    pflash_update(pfl, 0, pfl->sector_len * pfl->nb_blocs);
}

static int pflash_post_load(void *opaque, int version_id)
{
    pflash_t *pfl = opaque;

    if (!pfl->ro) {
        pfl->vmstate = qemu_add_vm_change_state_handler(postload_update_cb,
                                                        pfl);
    }
    return 0;
}
