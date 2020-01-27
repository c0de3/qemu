/*
 * Blockdev HMP commands
 *
 * Copyright (c) 2004 Fabrice Bellard
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * or (at your option) any later version.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/boards.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "qapi/qapi-commands-block.h"
#include "qapi/qmp/qdict.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "qemu/config-file.h"
#include "qemu/option.h"
#include "sysemu/sysemu.h"
#include "monitor/monitor.h"
#include "block/block_int.h"
#include "block/block-hmp-commands.h"
#include "monitor/hmp.h"

void hmp_drive_add(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;
    DriveInfo *dinfo;
    QemuOpts *opts;
    MachineClass *mc;
    const char *optstr = qdict_get_str(qdict, "opts");
    bool node = qdict_get_try_bool(qdict, "node", false);

    if (node) {
        hmp_drive_add_node(mon, optstr);
        return;
    }

    opts = drive_def(optstr);
    if (!opts)
        return;

    mc = MACHINE_GET_CLASS(current_machine);
    dinfo = drive_new(opts, mc->block_default_type, &err);
    if (err) {
        error_report_err(err);
        qemu_opts_del(opts);
        goto err;
    }

    if (!dinfo) {
        return;
    }

    switch (dinfo->type) {
    case IF_NONE:
        monitor_printf(mon, "OK\n");
        break;
    default:
        monitor_printf(mon, "Can't hot-add drive to type %d\n", dinfo->type);
        goto err;
    }
    return;

err:
    if (dinfo) {
        BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
        monitor_remove_blk(blk);
        blk_unref(blk);
    }
}

void hmp_drive_del(Monitor *mon, const QDict *qdict)
{
    const char *id = qdict_get_str(qdict, "id");
    BlockBackend *blk;
    BlockDriverState *bs;
    AioContext *aio_context;
    Error *local_err = NULL;

    bs = bdrv_find_node(id);
    if (bs) {
        qmp_blockdev_del(id, &local_err);
        if (local_err) {
            error_report_err(local_err);
        }
        return;
    }

    blk = blk_by_name(id);
    if (!blk) {
        error_report("Device '%s' not found", id);
        return;
    }

    if (!blk_legacy_dinfo(blk)) {
        error_report("Deleting device added with blockdev-add"
                     " is not supported");
        return;
    }

    aio_context = blk_get_aio_context(blk);
    aio_context_acquire(aio_context);

    bs = blk_bs(blk);
    if (bs) {
        if (bdrv_op_is_blocked(bs, BLOCK_OP_TYPE_DRIVE_DEL, &local_err)) {
            error_report_err(local_err);
            aio_context_release(aio_context);
            return;
        }

        blk_remove_bs(blk);
    }

    /* Make the BlockBackend and the attached BlockDriverState anonymous */
    monitor_remove_blk(blk);

    /* If this BlockBackend has a device attached to it, its refcount will be
     * decremented when the device is removed; otherwise we have to do so here.
     */
    if (blk_get_attached_dev(blk)) {
        /* Further I/O must not pause the guest */
        blk_set_on_error(blk, BLOCKDEV_ON_ERROR_REPORT,
                         BLOCKDEV_ON_ERROR_REPORT);
    } else {
        blk_unref(blk);
    }

    aio_context_release(aio_context);
}

void hmp_commit(Monitor *mon, const QDict *qdict)
{
    const char *device = qdict_get_str(qdict, "device");
    BlockBackend *blk;
    int ret;

    if (!strcmp(device, "all")) {
        ret = blk_commit_all();
    } else {
        BlockDriverState *bs;
        AioContext *aio_context;

        blk = blk_by_name(device);
        if (!blk) {
            error_report("Device '%s' not found", device);
            return;
        }
        if (!blk_is_available(blk)) {
            error_report("Device '%s' has no medium", device);
            return;
        }

        bs = blk_bs(blk);
        aio_context = bdrv_get_aio_context(bs);
        aio_context_acquire(aio_context);

        ret = bdrv_commit(bs);

        aio_context_release(aio_context);
    }
    if (ret < 0) {
        error_report("'commit' error for '%s': %s", device, strerror(-ret));
    }
}

void hmp_drive_mirror(Monitor *mon, const QDict *qdict)
{
    const char *filename = qdict_get_str(qdict, "target");
    const char *format = qdict_get_try_str(qdict, "format");
    bool reuse = qdict_get_try_bool(qdict, "reuse", false);
    bool full = qdict_get_try_bool(qdict, "full", false);
    Error *err = NULL;
    DriveMirror mirror = {
        .device = (char *)qdict_get_str(qdict, "device"),
        .target = (char *)filename,
        .has_format = !!format,
        .format = (char *)format,
        .sync = full ? MIRROR_SYNC_MODE_FULL : MIRROR_SYNC_MODE_TOP,
        .has_mode = true,
        .mode = reuse ? NEW_IMAGE_MODE_EXISTING : NEW_IMAGE_MODE_ABSOLUTE_PATHS,
        .unmap = true,
    };

    if (!filename) {
        error_setg(&err, QERR_MISSING_PARAMETER, "target");
        hmp_handle_error(mon, err);
        return;
    }
    qmp_drive_mirror(&mirror, &err);
    hmp_handle_error(mon, err);
}

void hmp_drive_backup(Monitor *mon, const QDict *qdict)
{
    const char *device = qdict_get_str(qdict, "device");
    const char *filename = qdict_get_str(qdict, "target");
    const char *format = qdict_get_try_str(qdict, "format");
    bool reuse = qdict_get_try_bool(qdict, "reuse", false);
    bool full = qdict_get_try_bool(qdict, "full", false);
    bool compress = qdict_get_try_bool(qdict, "compress", false);
    Error *err = NULL;
    DriveBackup backup = {
        .device = (char *)device,
        .target = (char *)filename,
        .has_format = !!format,
        .format = (char *)format,
        .sync = full ? MIRROR_SYNC_MODE_FULL : MIRROR_SYNC_MODE_TOP,
        .has_mode = true,
        .mode = reuse ? NEW_IMAGE_MODE_EXISTING : NEW_IMAGE_MODE_ABSOLUTE_PATHS,
        .has_compress = !!compress,
        .compress = compress,
    };

    if (!filename) {
        error_setg(&err, QERR_MISSING_PARAMETER, "target");
        hmp_handle_error(mon, err);
        return;
    }

    qmp_drive_backup(&backup, &err);
    hmp_handle_error(mon, err);
}
