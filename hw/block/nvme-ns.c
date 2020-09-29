/*
 * QEMU NVM Express Virtual Namespace
 *
 * Copyright (c) 2019 CNEX Labs
 * Copyright (c) 2020 Samsung Electronics
 *
 * Authors:
 *  Klaus Jensen      <k.jensen@samsung.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "hw/block/block.h"
#include "hw/pci/pci.h"
#include "sysemu/sysemu.h"
#include "sysemu/block-backend.h"
#include "qapi/error.h"

#include "hw/qdev-properties.h"
#include "hw/qdev-core.h"

#include "trace.h"

#include "nvme.h"
#include "nvme-ns.h"

const char *nvme_zs_str(NvmeZone *zone)
{
    return nvme_zs_to_str(nvme_zs(zone));
}

const char *nvme_zs_to_str(NvmeZoneState zs)
{
    switch (zs) {
    case NVME_ZS_ZSE:  return "ZSE";
    case NVME_ZS_ZSIO: return "ZSIO";
    case NVME_ZS_ZSEO: return "ZSEO";
    case NVME_ZS_ZSC:  return "ZSC";
    case NVME_ZS_ZSRO: return "ZSRO";
    case NVME_ZS_ZSF:  return "ZSF";
    case NVME_ZS_ZSO:  return "ZSO";
    }

    return NULL;
}

static int nvme_blk_truncate(BlockBackend *blk, size_t len, Error **errp)
{
    int ret;
    uint64_t perm, shared_perm;

    blk_get_perm(blk, &perm, &shared_perm);

    ret = blk_set_perm(blk, perm | BLK_PERM_RESIZE, shared_perm, errp);
    if (ret < 0) {
        return ret;
    }

    ret = blk_truncate(blk, len, false, PREALLOC_MODE_OFF, 0, errp);
    if (ret < 0) {
        return ret;
    }

    ret = blk_set_perm(blk, perm, shared_perm, errp);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

static void nvme_ns_zns_init_zones(NvmeNamespace *ns)
{
    NvmeZone *zone;
    NvmeZoneDescriptor *zd;
    uint64_t zslba, zsze = nvme_ns_zsze(ns);

    for (int i = 0; i < ns->zns.num_zones; i++) {
        zslba = i * zsze;

        zone = &ns->zns.zones[i];
        zone->zd = &ns->zns.zd[i];
        if (ns->params.zns.zdes) {
            zone->zde = &ns->zns.zde[i];
        }
        zone->wp_staging = zslba;

        zd = zone->zd;
        zd->zt = NVME_ZT_SEQ;
        zd->zcap = cpu_to_le64(ns->params.zns.zcap);
        zd->wp = zd->zslba = cpu_to_le64(zslba);

        nvme_zs_set(zone, NVME_ZS_ZSE);
    }
}

static void nvme_ns_init_zoned(NvmeNamespace *ns)
{
    NvmeIdNsNvm *id_ns = nvme_ns_id_nvm(ns);
    NvmeIdNsZns *id_ns_zns = nvme_ns_id_zoned(ns);

    for (int i = 0; i <= id_ns->nlbaf; i++) {
        id_ns_zns->lbafe[i].zsze = ns->params.zns.zsze ?
            cpu_to_le64(ns->params.zns.zsze) :
            cpu_to_le64(pow2ceil(ns->params.zns.zcap));
        id_ns_zns->lbafe[i].zdes = ns->params.zns.zdes;
    }

    ns->zns.num_zones = nvme_ns_nlbas(ns) / nvme_ns_zsze(ns);
    ns->zns.zones = g_malloc0_n(ns->zns.num_zones, sizeof(NvmeZone));
    ns->zns.zd = g_malloc0_n(ns->zns.num_zones, sizeof(NvmeZoneDescriptor));
    if (ns->params.zns.zdes) {
        ns->zns.zde = g_malloc0_n(ns->zns.num_zones, nvme_ns_zdes_bytes(ns));
    }

    id_ns_zns->mar = cpu_to_le32(ns->params.zns.mar);
    id_ns_zns->mor = cpu_to_le32(ns->params.zns.mor);
}

static void nvme_ns_init(NvmeNamespace *ns)
{
    NvmeIdNsNvm *id_ns;

    ns->id_ns[NVME_IOCS_NVM] = g_new0(NvmeIdNsNvm, 1);
    id_ns = nvme_ns_id_nvm(ns);

    ns->iocs = ns->params.iocs;

    if (blk_get_flags(ns->blkconf.blk) & BDRV_O_UNMAP) {
        id_ns->dlfeat = 0x9;
    }

    id_ns->lbaf[0].ds = ns->params.lbads;

    id_ns->nsze = cpu_to_le64(nvme_ns_nlbas(ns));

    if (nvme_ns_zoned(ns)) {
        ns->id_ns[NVME_IOCS_ZONED] = g_new0(NvmeIdNsZns, 1);
        nvme_ns_init_zoned(ns);
    }

    /* no thin provisioning */
    id_ns->ncap = id_ns->nsze;
    id_ns->nuse = id_ns->ncap;
}

static int nvme_ns_pstate_init(NvmeNamespace *ns, Error **errp)
{
    BlockBackend *blk = ns->pstate.blk;
    NvmePstateHeader header;
    uint64_t nlbas = nvme_ns_nlbas(ns);
    size_t bitmap_len, pstate_len, zd_len = 0, zde_len = 0;
    int ret;

    ret = nvme_blk_truncate(blk, sizeof(NvmePstateHeader), errp);
    if (ret < 0) {
        return ret;
    }

    header = (NvmePstateHeader) {
        .magic   = cpu_to_le32(NVME_PSTATE_MAGIC),
        .version = cpu_to_le32(NVME_PSTATE_V1),
        .blk_len = cpu_to_le64(ns->size),
        .lbads   = ns->params.lbads,
        .iocs    = ns->params.iocs,
    };

    if (nvme_ns_zoned(ns)) {
        /* zns specific; offset 0xc00 */
        header.zns.zcap = cpu_to_le64(ns->params.zns.zcap);
        header.zns.zsze = ns->params.zns.zsze ?
            cpu_to_le64(ns->params.zns.zsze) :
            cpu_to_le64(pow2ceil(ns->params.zns.zcap));
        header.zns.zdes = ns->params.zns.zdes;
    }

    ret = blk_pwrite(blk, 0, &header, sizeof(header), 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "could not write pstate header");
        return ret;
    }

    bitmap_len = DIV_ROUND_UP(nlbas, sizeof(unsigned long));
    if (nvme_ns_zoned(ns)) {
        zd_len = ns->zns.num_zones * sizeof(NvmeZoneDescriptor);
        zde_len = nvme_ns_zoned(ns) ?
            ns->zns.num_zones * nvme_ns_zdes_bytes(ns) : 0;
    }
    pstate_len = ROUND_UP(sizeof(NvmePstateHeader) + bitmap_len + zd_len +
                          zde_len, BDRV_SECTOR_SIZE);

    ret = nvme_blk_truncate(blk, pstate_len, errp);
    if (ret < 0) {
        return ret;
    }

    ns->pstate.utilization.map = bitmap_new(nlbas);

    if (zd_len) {
        ns->pstate.zns.offset = ns->pstate.utilization.offset + bitmap_len,

        nvme_ns_zns_init_zones(ns);

        ret = blk_pwrite(blk, ns->pstate.zns.offset, ns->zns.zd, zd_len, 0);
        if (ret < 0) {
            error_setg_errno(errp, -ret,
                             "could not write zone descriptors to pstate");
            return ret;
        }
    }

    return 0;
}

void nvme_ns_zns_init_zone_state(NvmeNamespace *ns)
{
    ns->zns.resources.active = ns->params.zns.mar != 0xffffffff ?
        ns->params.zns.mar + 1 : ns->zns.num_zones;
    ns->zns.resources.open = ns->params.zns.mor != 0xffffffff ?
        ns->params.zns.mor + 1 : ns->zns.num_zones;

    for (int i = 0; i < ns->zns.num_zones; i++) {
        NvmeZone *zone = &ns->zns.zones[i];
        zone->zd = &ns->zns.zd[i];
        zone->zde = &ns->zns.zde[i];

        zone->wp_staging = nvme_wp(zone);

        switch (nvme_zs(zone)) {
        case NVME_ZS_ZSE:
        case NVME_ZS_ZSF:
        case NVME_ZS_ZSRO:
        case NVME_ZS_ZSO:
            continue;

        case NVME_ZS_ZSC:
            if (nvme_wp(zone) == nvme_zslba(zone) &&
                !(zone->zd->za & NVME_ZA_ZDEV)) {
                nvme_zs_set(zone, NVME_ZS_ZSE);
                continue;
            }

            if (ns->zns.resources.active) {
                ns->zns.resources.active--;
                continue;
            }

            /* fallthrough */

        case NVME_ZS_ZSIO:
        case NVME_ZS_ZSEO:
            zone->zd->wp = zone->zd->zslba;
            nvme_zs_set(zone, NVME_ZS_ZSF);
        }
    }
}

static int nvme_ns_pstate_load(NvmeNamespace *ns, size_t len, Error **errp)
{
    BlockBackend *blk = ns->pstate.blk;
    NvmePstateHeader header;
    uint64_t nlbas = nvme_ns_nlbas(ns);
    size_t bitmap_len, pstate_len, zd_len = 0, zde_len = 0;
    unsigned long *map;
    int ret;

    ret = blk_pread(blk, 0, &header, sizeof(header));
    if (ret < 0) {
        error_setg_errno(errp, -ret, "could not read pstate header");
        return ret;
    }

    if (le32_to_cpu(header.magic) != NVME_PSTATE_MAGIC) {
        error_setg(errp, "invalid pstate header");
        return -1;
    } else if (le32_to_cpu(header.version) > NVME_PSTATE_V1) {
        error_setg(errp, "unsupported pstate version");
        return -1;
    }

    if (le64_to_cpu(header.blk_len) != ns->size) {
        error_setg(errp, "invalid drive size");
        return -1;
    }

    if (header.lbads != ns->params.lbads) {
        error_setg(errp, "lbads parameter inconsistent with pstate "
                   "(pstate %u; parameter %u)",
                   header.lbads, ns->params.lbads);
        return -1;
    }

    if (header.iocs != ns->params.iocs) {
        error_setg(errp, "iocs parameter inconsistent with pstate "
                   "(pstate %u; parameter %u)",
                   header.iocs, ns->params.iocs);
        return -1;
    }

    if (header.zns.zcap != ns->params.zns.zcap) {
        error_setg(errp, "zns.zcap parameter inconsistent with pstate "
                   "(pstate %"PRIu64"; parameter %"PRIu64")",
                   header.zns.zcap, ns->params.zns.zcap);
        return -1;
    }

    if (ns->params.zns.zsze && header.zns.zsze != ns->params.zns.zsze) {
        error_setg(errp, "zns.zsze parameter inconsistent with pstate "
                   "(pstate %"PRIu64"; parameter %"PRIu64")",
                   header.zns.zsze, ns->params.zns.zsze);
        return -1;
    }

    if (header.zns.zdes != ns->params.zns.zdes) {
        error_setg(errp, "zns.zdes parameter inconsistent with pstate "
                   "(pstate %u; parameter %u)",
                   header.zns.zdes, ns->params.zns.zdes);
        return -1;
    }

    bitmap_len = DIV_ROUND_UP(nlbas, sizeof(unsigned long));
    if (nvme_ns_zoned(ns)) {
        zd_len = ns->zns.num_zones * sizeof(NvmeZoneDescriptor);
        zde_len = nvme_ns_zoned(ns) ?
            ns->zns.num_zones * nvme_ns_zdes_bytes(ns) : 0;
    }
    pstate_len = ROUND_UP(sizeof(NvmePstateHeader) + bitmap_len + zd_len +
                          zde_len, BDRV_SECTOR_SIZE);

    if (len != pstate_len) {
        error_setg(errp, "pstate size mismatch "
                   "(expected %zd bytes; was %zu bytes)",
                   pstate_len, len);
        return -1;
    }

    map = bitmap_new(nlbas);
    ret = blk_pread(blk, ns->pstate.utilization.offset, map, bitmap_len);
    if (ret < 0) {
        error_setg_errno(errp, -ret,
                         "could not read pstate allocation bitmap");
        g_free(map);
        return ret;
    }

#ifdef HOST_WORDS_BIGENDIAN
    ns->pstate.utilization.map = bitmap_new(nlbas);
    bitmap_from_le(ns->pstate.utilization.map, map, nlbas);
    g_free(map);
#else
    ns->pstate.utilization.map = map;
#endif

    if (zd_len) {
        ns->pstate.zns.offset = ns->pstate.utilization.offset + bitmap_len,

        ret = blk_pread(blk, ns->pstate.zns.offset, ns->zns.zd, zd_len);
        if (ret < 0) {
            error_setg_errno(errp, -ret,
                             "could not read zone descriptors from pstate");
            return ret;
        }

        if (zde_len) {
            ret = blk_pread(blk, ns->pstate.zns.offset + zd_len, ns->zns.zde,
                            zde_len);
            if (ret < 0) {
                error_setg_errno(errp, -ret, "could not read zone descriptor "
                                 "extensions from pstate");
                return ret;
            }
        }

        nvme_ns_zns_init_zone_state(ns);

        ret = blk_pwrite(blk, ns->pstate.zns.offset, ns->zns.zd, zd_len, 0);
        if (ret < 0) {
            error_setg_errno(errp, -ret,
                             "could not write zone descriptors to pstate");
            return ret;
        }
    }

    return 0;

}

static int nvme_ns_setup_blk_pstate(NvmeNamespace *ns, Error **errp)
{
    BlockBackend *blk = ns->pstate.blk;
    uint64_t perm, shared_perm;
    ssize_t len;
    int ret;

    perm = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE;
    shared_perm = BLK_PERM_ALL;

    ret = blk_set_perm(blk, perm, shared_perm, errp);
    if (ret) {
        return ret;
    }

    ns->pstate.utilization.offset = sizeof(NvmePstateHeader);

    len = blk_getlength(blk);
    if (len < 0) {
        error_setg_errno(errp, -len, "could not determine pstate size");
        return len;
    }

    if (!len) {
        return nvme_ns_pstate_init(ns, errp);
    }

    return nvme_ns_pstate_load(ns, len, errp);
}

static int nvme_ns_init_blk(NvmeCtrl *n, NvmeNamespace *ns, Error **errp)
{
    if (!blkconf_blocksizes(&ns->blkconf, errp)) {
        return -1;
    }

    if (!blkconf_apply_backend_options(&ns->blkconf,
                                       blk_is_read_only(ns->blkconf.blk),
                                       false, errp)) {
        return -1;
    }

    ns->size = blk_getlength(ns->blkconf.blk);
    if (ns->size < 0) {
        error_setg_errno(errp, -ns->size, "could not get blockdev size");
        return -1;
    }

    if (blk_enable_write_cache(ns->blkconf.blk)) {
        n->features.vwc = 0x1;
    }

    return 0;
}

static int nvme_ns_check_constraints(NvmeNamespace *ns, Error **errp)
{
    if (!ns->blkconf.blk) {
        error_setg(errp, "block backend not configured");
        return -1;
    }

    if (ns->params.lbads < 9 || ns->params.lbads > 12) {
        error_setg(errp, "unsupported lbads (supported: 9-12)");
        return -1;
    }

    switch (ns->params.iocs) {
    case NVME_IOCS_NVM:
        break;

    case NVME_IOCS_ZONED:
        if (!ns->params.zns.zcap) {
            error_setg(errp, "zns.zcap must be specified");
            return -1;
        }

        if (ns->params.zns.zsze && ns->params.zns.zsze < ns->params.zns.zcap) {
            error_setg(errp, "zns.zsze cannot be less than zns.zcap");
            return -1;
        }

        if (ns->params.zns.mor > ns->params.zns.mar) {
            error_setg(errp, "maximum open resources (zns.mor) must be less "
                       "than or equal to maximum active resources (zns.mar)");
            return -1;
        }

        break;

    default:
        error_setg(errp, "unsupported iocs");
        return -1;
    }

    return 0;
}

int nvme_ns_setup(NvmeCtrl *n, NvmeNamespace *ns, Error **errp)
{
    if (nvme_ns_check_constraints(ns, errp)) {
        return -1;
    }

    if (nvme_ns_init_blk(n, ns, errp)) {
        return -1;
    }

    nvme_ns_init(ns);

    if (ns->pstate.blk) {
        if (nvme_ns_setup_blk_pstate(ns, errp)) {
            return -1;
        }

        /*
         * With a pstate file in place we can enable the Deallocated or
         * Unwritten Logical Block Error feature.
         */
        NvmeIdNsNvm *id_ns = nvme_ns_id_nvm(ns);
        id_ns->nsfeat |= 0x4;
    } else if (nvme_ns_zoned(ns)) {
        nvme_ns_zns_init_zones(ns);
    }

    if (nvme_register_namespace(n, ns, errp)) {
        return -1;
    }

    return 0;
}

void nvme_ns_drain(NvmeNamespace *ns)
{
    blk_drain(ns->blkconf.blk);

    if (ns->pstate.blk) {
        blk_drain(ns->pstate.blk);
    }
}

void nvme_ns_flush(NvmeNamespace *ns)
{
    blk_flush(ns->blkconf.blk);

    if (ns->pstate.blk) {
        blk_flush(ns->pstate.blk);
    }
}

static void nvme_ns_realize(DeviceState *dev, Error **errp)
{
    NvmeNamespace *ns = NVME_NS(dev);
    BusState *s = qdev_get_parent_bus(dev);
    NvmeCtrl *n = NVME(s->parent);
    Error *local_err = NULL;

    if (nvme_ns_setup(n, ns, &local_err)) {
        error_propagate_prepend(errp, local_err,
                                "could not setup namespace: ");
        return;
    }
}

static Property nvme_ns_props[] = {
    DEFINE_BLOCK_PROPERTIES(NvmeNamespace, blkconf),
    DEFINE_PROP_UINT32("nsid", NvmeNamespace, params.nsid, 0),
    DEFINE_PROP_UINT8("lbads", NvmeNamespace, params.lbads, BDRV_SECTOR_BITS),
    DEFINE_PROP_DRIVE("pstate", NvmeNamespace, pstate.blk),
    DEFINE_PROP_UINT8("iocs", NvmeNamespace, params.iocs, NVME_IOCS_NVM),
    DEFINE_PROP_UINT64("zns.zcap", NvmeNamespace, params.zns.zcap, 0),
    DEFINE_PROP_UINT64("zns.zsze", NvmeNamespace, params.zns.zsze, 0),
    DEFINE_PROP_UINT8("zns.zdes", NvmeNamespace, params.zns.zdes, 0),
    DEFINE_PROP_UINT32("zns.mar", NvmeNamespace, params.zns.mar, 0xffffffff),
    DEFINE_PROP_UINT32("zns.mor", NvmeNamespace, params.zns.mor, 0xffffffff),
    DEFINE_PROP_END_OF_LIST(),
};

static void nvme_ns_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    dc->bus_type = TYPE_NVME_BUS;
    dc->realize = nvme_ns_realize;
    device_class_set_props(dc, nvme_ns_props);
    dc->desc = "Virtual NVMe namespace";
}

static void nvme_ns_instance_init(Object *obj)
{
    NvmeNamespace *ns = NVME_NS(obj);
    char *bootindex = g_strdup_printf("/namespace@%d,0", ns->params.nsid);

    device_add_bootindex_property(obj, &ns->bootindex, "bootindex",
                                  bootindex, DEVICE(obj));

    g_free(bootindex);
}

static const TypeInfo nvme_ns_info = {
    .name = TYPE_NVME_NS,
    .parent = TYPE_DEVICE,
    .class_init = nvme_ns_class_init,
    .instance_size = sizeof(NvmeNamespace),
    .instance_init = nvme_ns_instance_init,
};

static void nvme_ns_register_types(void)
{
    type_register_static(&nvme_ns_info);
}

type_init(nvme_ns_register_types)
