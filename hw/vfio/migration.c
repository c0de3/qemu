/*
 * Migration support for VFIO devices
 *
 * Copyright NVIDIA, Inc. 2019
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <linux/vfio.h>

#include "hw/vfio/vfio-common.h"
#include "cpu.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "migration/register.h"
#include "migration/blocker.h"
#include "migration/misc.h"
#include "qapi/error.h"
#include "exec/ramlist.h"
#include "exec/ram_addr.h"
#include "pci.h"
#include "trace.h"

/*
 * Flags used as delimiter:
 * 0xffffffff => MSB 32-bit all 1s
 * 0xef10     => emulated (virtual) function IO
 * 0x0000     => 16-bits reserved for flags
 */
#define VFIO_MIG_FLAG_END_OF_STATE      (0xffffffffef100001ULL)
#define VFIO_MIG_FLAG_DEV_CONFIG_STATE  (0xffffffffef100002ULL)
#define VFIO_MIG_FLAG_DEV_SETUP_STATE   (0xffffffffef100003ULL)
#define VFIO_MIG_FLAG_DEV_DATA_STATE    (0xffffffffef100004ULL)

static void vfio_migration_region_exit(VFIODevice *vbasedev)
{
    VFIOMigration *migration = vbasedev->migration;

    if (!migration) {
        return;
    }

    if (migration->region.buffer.size) {
        vfio_region_exit(&migration->region.buffer);
        vfio_region_finalize(&migration->region.buffer);
    }
}

static int vfio_migration_region_init(VFIODevice *vbasedev)
{
    VFIOMigration *migration = vbasedev->migration;
    Object *obj = NULL;
    int ret = -EINVAL;

    if (!migration) {
        return ret;
    }

    /* Migration support added for PCI device only */
    if (vbasedev->type == VFIO_DEVICE_TYPE_PCI) {
        obj = vfio_pci_get_object(vbasedev);
    }

    if (!obj) {
        return ret;
    }

    ret = vfio_region_setup(obj, vbasedev, &migration->region.buffer,
                            migration->region.index, "migration");
    if (ret) {
        error_report("Failed to setup VFIO migration region %d: %s",
                      migration->region.index, strerror(-ret));
        goto err;
    }

    if (!migration->region.buffer.size) {
        ret = -EINVAL;
        error_report("Invalid region size of VFIO migration region %d: %s",
                     migration->region.index, strerror(-ret));
        goto err;
    }

    return 0;

err:
    vfio_migration_region_exit(vbasedev);
    return ret;
}

static int vfio_migration_set_state(VFIODevice *vbasedev, uint32_t state)
{
    VFIOMigration *migration = vbasedev->migration;
    VFIORegion *region = &migration->region.buffer;
    int ret = 0;

    ret = pwrite(vbasedev->fd, &state, sizeof(state),
                 region->fd_offset + offsetof(struct vfio_device_migration_info,
                                              device_state));
    if (ret < 0) {
        error_report("Failed to set migration state %d %s",
                     ret, strerror(errno));
        return ret;
    }

    vbasedev->device_state = state;
    trace_vfio_migration_set_state(vbasedev->name, state);
    return 0;
}

static int vfio_save_buffer(QEMUFile *f, VFIODevice *vbasedev)
{
    VFIOMigration *migration = vbasedev->migration;
    VFIORegion *region = &migration->region.buffer;
    uint64_t data_offset = 0, data_size = 0;
    int ret;

    ret = pread(vbasedev->fd, &data_offset, sizeof(data_offset),
                region->fd_offset + offsetof(struct vfio_device_migration_info,
                                             data_offset));
    if (ret != sizeof(data_offset)) {
        error_report("Failed to get migration buffer data offset %d",
                     ret);
        return -EINVAL;
    }

    ret = pread(vbasedev->fd, &data_size, sizeof(data_size),
                region->fd_offset + offsetof(struct vfio_device_migration_info,
                                             data_size));
    if (ret != sizeof(data_size)) {
        error_report("Failed to get migration buffer data size %d",
                     ret);
        return -EINVAL;
    }

    if (data_size > 0) {
        void *buf = NULL;
        bool buffer_mmaped = false;

        if (region->mmaps) {
            int i;

            for (i = 0; i < region->nr_mmaps; i++) {
                if ((data_offset >= region->mmaps[i].offset) &&
                    (data_offset < region->mmaps[i].offset +
                                   region->mmaps[i].size)) {
                    buf = region->mmaps[i].mmap + (data_offset -
                                                   region->mmaps[i].offset);
                    buffer_mmaped = true;
                    break;
                }
            }
        }

        if (!buffer_mmaped) {
            buf = g_malloc0(data_size);
            ret = pread(vbasedev->fd, buf, data_size,
                        region->fd_offset + data_offset);
            if (ret != data_size) {
                error_report("Failed to get migration data %d", ret);
                g_free(buf);
                return -EINVAL;
            }
        }

        qemu_put_be64(f, data_size);
        qemu_put_buffer(f, buf, data_size);

        if (!buffer_mmaped) {
            g_free(buf);
        }
        migration->pending_bytes -= data_size;
    } else {
        qemu_put_be64(f, data_size);
    }

    trace_vfio_save_buffer(vbasedev->name, data_offset, data_size,
                           migration->pending_bytes);
    ret = qemu_file_get_error(f);

    return data_size;
}

static int vfio_update_pending(VFIODevice *vbasedev)
{
    VFIOMigration *migration = vbasedev->migration;
    VFIORegion *region = &migration->region.buffer;
    uint64_t pending_bytes = 0;
    int ret;

    ret = pread(vbasedev->fd, &pending_bytes, sizeof(pending_bytes),
                region->fd_offset + offsetof(struct vfio_device_migration_info,
                                             pending_bytes));
    if ((ret < 0) || (ret != sizeof(pending_bytes))) {
        error_report("Failed to get pending bytes %d", ret);
        migration->pending_bytes = 0;
        return (ret < 0) ? ret : -EINVAL;
    }

    migration->pending_bytes = pending_bytes;
    trace_vfio_update_pending(vbasedev->name, pending_bytes);
    return 0;
}

static int vfio_save_device_config_state(QEMUFile *f, void *opaque)
{
    VFIODevice *vbasedev = opaque;

    qemu_put_be64(f, VFIO_MIG_FLAG_DEV_CONFIG_STATE);

    if (vbasedev->type == VFIO_DEVICE_TYPE_PCI) {
        vfio_pci_save_config(vbasedev, f);
    }
    qemu_put_be64(f, VFIO_MIG_FLAG_END_OF_STATE);

    trace_vfio_save_device_config_state(vbasedev->name);

    return qemu_file_get_error(f);
}

static int vfio_load_device_config_state(QEMUFile *f, void *opaque)
{
    VFIODevice *vbasedev = opaque;

    if (vbasedev->type == VFIO_DEVICE_TYPE_PCI) {
        vfio_pci_load_config(vbasedev, f);
    }

    if (qemu_get_be64(f) != VFIO_MIG_FLAG_END_OF_STATE) {
        error_report("Wrong end of block while loading device config space");
        return -EINVAL;
    }

    trace_vfio_load_device_config_state(vbasedev->name);
    return qemu_file_get_error(f);
}

void vfio_get_dirty_page_list(VFIODevice *vbasedev,
                              uint64_t start_pfn,
                              uint64_t pfn_count,
                              uint64_t page_size)
{
    VFIOMigration *migration = vbasedev->migration;
    VFIORegion *region = &migration->region.buffer;
    uint64_t count = 0;
    int64_t copied_pfns = 0;
    int ret;

    qemu_mutex_lock(&migration->lock);
    ret = pwrite(vbasedev->fd, &start_pfn, sizeof(start_pfn),
                 region->fd_offset + offsetof(struct vfio_device_migration_info,
                                              start_pfn));
    if (ret < 0) {
        error_report("Failed to set dirty pages start address %d %s",
                ret, strerror(errno));
        goto dpl_unlock;
    }

    ret = pwrite(vbasedev->fd, &page_size, sizeof(page_size),
                 region->fd_offset + offsetof(struct vfio_device_migration_info,
                                              page_size));
    if (ret < 0) {
        error_report("Failed to set dirty page size %d %s",
                ret, strerror(errno));
        goto dpl_unlock;
    }

    ret = pwrite(vbasedev->fd, &pfn_count, sizeof(pfn_count),
                 region->fd_offset + offsetof(struct vfio_device_migration_info,
                                              total_pfns));
    if (ret < 0) {
        error_report("Failed to set dirty page total pfns %d %s",
                ret, strerror(errno));
        goto dpl_unlock;
    }

    do {
        uint64_t bitmap_size, data_offset = 0;
        void *buf = NULL;
        bool buffer_mmaped = false;

        /* Read copied dirty pfns */
        ret = pread(vbasedev->fd, &copied_pfns, sizeof(copied_pfns),
                region->fd_offset + offsetof(struct vfio_device_migration_info,
                                             copied_pfns));
        if (ret < 0) {
            error_report("Failed to get dirty pages bitmap count %d %s",
                    ret, strerror(errno));
            goto dpl_unlock;
        }

        if (copied_pfns == 0) {
            /*
             * copied_pfns could be 0 if driver doesn't have any page to
             * report dirty in given range
             */
            break;
        } else if (copied_pfns == -1) {
            /* Mark all pages dirty for this range */
            cpu_physical_memory_set_dirty_range(start_pfn * page_size,
                                                pfn_count * page_size,
                                                DIRTY_MEMORY_MIGRATION);
            break;
        }

        bitmap_size = (BITS_TO_LONGS(copied_pfns) + 1) * sizeof(unsigned long);

        ret = pread(vbasedev->fd, &data_offset, sizeof(data_offset),
                region->fd_offset + offsetof(struct vfio_device_migration_info,
                                             data_offset));
        if (ret != sizeof(data_offset)) {
            error_report("Failed to get migration buffer data offset %d",
                         ret);
            goto dpl_unlock;
        }

        if (region->mmaps) {
            int i;
            for (i = 0; i < region->nr_mmaps; i++) {
                if ((region->mmaps[i].offset >= data_offset) &&
                    (data_offset < region->mmaps[i].offset +
                                   region->mmaps[i].size)) {
                    buf = region->mmaps[i].mmap + (data_offset -
                                                   region->mmaps[i].offset);
                    buffer_mmaped = true;
                    break;
                }
            }
        }

        if (!buffer_mmaped) {
            buf = g_malloc0(bitmap_size);

            ret = pread(vbasedev->fd, buf, bitmap_size,
                        region->fd_offset + data_offset);
            if (ret != bitmap_size) {
                error_report("Failed to get dirty pages bitmap %d", ret);
                g_free(buf);
                goto dpl_unlock;
            }
        }

        cpu_physical_memory_set_dirty_lebitmap((unsigned long *)buf,
                                               (start_pfn + count) * page_size,
                                                copied_pfns);
        count +=  copied_pfns;

        if (!buffer_mmaped) {
            g_free(buf);
        }
    } while (count < pfn_count);

    trace_vfio_get_dirty_page_list(vbasedev->name, start_pfn, pfn_count,
                                   page_size);

dpl_unlock:
    qemu_mutex_unlock(&migration->lock);
}

/* ---------------------------------------------------------------------- */

static int vfio_save_setup(QEMUFile *f, void *opaque)
{
    VFIODevice *vbasedev = opaque;
    VFIOMigration *migration = vbasedev->migration;
    int ret;

    qemu_put_be64(f, VFIO_MIG_FLAG_DEV_SETUP_STATE);

    if (migration->region.buffer.mmaps) {
        qemu_mutex_lock_iothread();
        ret = vfio_region_mmap(&migration->region.buffer);
        qemu_mutex_unlock_iothread();
        if (ret) {
            error_report("Failed to mmap VFIO migration region %d: %s",
                         migration->region.index, strerror(-ret));
            return ret;
        }
    }

    if (vbasedev->vm_running) {
        ret = vfio_migration_set_state(vbasedev,
                         VFIO_DEVICE_STATE_RUNNING | VFIO_DEVICE_STATE_SAVING);
        if (ret) {
            error_report("Failed to set state RUNNING and SAVING");
            return ret;
        }
    } else {
        ret = vfio_migration_set_state(vbasedev, VFIO_DEVICE_STATE_SAVING);
        if (ret) {
            error_report("Failed to set state STOP and SAVING");
            return ret;
        }
    }

    qemu_put_be64(f, VFIO_MIG_FLAG_END_OF_STATE);

    ret = qemu_file_get_error(f);
    if (ret) {
        return ret;
    }

    trace_vfio_save_setup(vbasedev->name);
    return 0;
}

static void vfio_save_cleanup(void *opaque)
{
    VFIODevice *vbasedev = opaque;
    VFIOMigration *migration = vbasedev->migration;

    if (migration->region.buffer.mmaps) {
        vfio_region_unmap(&migration->region.buffer);
    }
    trace_vfio_cleanup(vbasedev->name);
}

static void vfio_save_pending(QEMUFile *f, void *opaque,
                              uint64_t threshold_size,
                              uint64_t *res_precopy_only,
                              uint64_t *res_compatible,
                              uint64_t *res_postcopy_only)
{
    VFIODevice *vbasedev = opaque;
    VFIOMigration *migration = vbasedev->migration;
    int ret;

    ret = vfio_update_pending(vbasedev);
    if (ret) {
        return;
    }

    if (vbasedev->device_state & VFIO_DEVICE_STATE_RUNNING) {
        *res_precopy_only += migration->pending_bytes;
    } else {
        *res_postcopy_only += migration->pending_bytes;
    }
    *res_compatible += 0;
    trace_vfio_save_pending(vbasedev->name);
}

static int vfio_save_iterate(QEMUFile *f, void *opaque)
{
    VFIODevice *vbasedev = opaque;
    VFIOMigration *migration = vbasedev->migration;
    int ret;

    qemu_put_be64(f, VFIO_MIG_FLAG_DEV_DATA_STATE);

    qemu_mutex_lock(&migration->lock);
    ret = vfio_save_buffer(f, vbasedev);
    qemu_mutex_unlock(&migration->lock);

    if (ret < 0) {
        error_report("vfio_save_buffer failed %s",
                     strerror(errno));
        return ret;
    }

    qemu_put_be64(f, VFIO_MIG_FLAG_END_OF_STATE);

    ret = qemu_file_get_error(f);
    if (ret) {
        return ret;
    }

    trace_vfio_save_iterate(vbasedev->name);
    return ret;
}

static int vfio_save_complete_precopy(QEMUFile *f, void *opaque)
{
    VFIODevice *vbasedev = opaque;
    VFIOMigration *migration = vbasedev->migration;
    int ret;

    ret = vfio_migration_set_state(vbasedev, VFIO_DEVICE_STATE_SAVING);
    if (ret) {
        error_report("Failed to set state STOP and SAVING");
        return ret;
    }

    ret = vfio_save_device_config_state(f, opaque);
    if (ret) {
        return ret;
    }

    ret = vfio_update_pending(vbasedev);
    if (ret) {
        return ret;
    }

    while (migration->pending_bytes > 0) {
        qemu_put_be64(f, VFIO_MIG_FLAG_DEV_DATA_STATE);
        ret = vfio_save_buffer(f, vbasedev);
        if (ret < 0) {
            error_report("Failed to save buffer");
            return ret;
        } else if (ret == 0) {
            break;
        }

        ret = vfio_update_pending(vbasedev);
        if (ret) {
            return ret;
        }
    }

    qemu_put_be64(f, VFIO_MIG_FLAG_END_OF_STATE);

    ret = qemu_file_get_error(f);
    if (ret) {
        return ret;
    }

    ret = vfio_migration_set_state(vbasedev, VFIO_DEVICE_STATE_STOPPED);
    if (ret) {
        error_report("Failed to set state STOPPED");
        return ret;
    }

    trace_vfio_save_complete_precopy(vbasedev->name);
    return ret;
}

static int vfio_load_setup(QEMUFile *f, void *opaque)
{
    VFIODevice *vbasedev = opaque;
    VFIOMigration *migration = vbasedev->migration;
    int ret = 0;

    if (migration->region.buffer.mmaps) {
        ret = vfio_region_mmap(&migration->region.buffer);
        if (ret) {
            error_report("Failed to mmap VFIO migration region %d: %s",
                         migration->region.index, strerror(-ret));
            return ret;
        }
    }

    ret = vfio_migration_set_state(vbasedev, VFIO_DEVICE_STATE_RESUMING);
    if (ret) {
        error_report("Failed to set state RESUMING");
    }
    return ret;
}

static int vfio_load_cleanup(void *opaque)
{
    vfio_save_cleanup(opaque);
    return 0;
}

static int vfio_load_state(QEMUFile *f, void *opaque, int version_id)
{
    VFIODevice *vbasedev = opaque;
    VFIOMigration *migration = vbasedev->migration;
    int ret = 0;
    uint64_t data, data_size;

    data = qemu_get_be64(f);
    while (data != VFIO_MIG_FLAG_END_OF_STATE) {

        trace_vfio_load_state(vbasedev->name, data);

        switch (data) {
        case VFIO_MIG_FLAG_DEV_CONFIG_STATE:
        {
            ret = vfio_load_device_config_state(f, opaque);
            if (ret) {
                return ret;
            }
            break;
        }
        case VFIO_MIG_FLAG_DEV_SETUP_STATE:
        {
            data = qemu_get_be64(f);
            if (data == VFIO_MIG_FLAG_END_OF_STATE) {
                return ret;
            } else {
                error_report("SETUP STATE: EOS not found 0x%lx", data);
                return -EINVAL;
            }
            break;
        }
        case VFIO_MIG_FLAG_DEV_DATA_STATE:
        {
            VFIORegion *region = &migration->region.buffer;
            void *buf = NULL;
            bool buffer_mmaped = false;
            uint64_t data_offset = 0;

            data_size = qemu_get_be64(f);
            if (data_size == 0) {
                break;
            }

            ret = pread(vbasedev->fd, &data_offset, sizeof(data_offset),
                        region->fd_offset +
                        offsetof(struct vfio_device_migration_info,
                        data_offset));
            if (ret != sizeof(data_offset)) {
                error_report("Failed to get migration buffer data offset %d",
                              ret);
                return -EINVAL;
            }

            if (region->mmaps) {
                int i;

                for (i = 0; i < region->nr_mmaps; i++) {
                    if (region->mmaps[i].mmap &&
                        (data_offset >= region->mmaps[i].offset) &&
                        (data_offset < region->mmaps[i].offset +
                                       region->mmaps[i].size)) {
                        buf = region->mmaps[i].mmap + (data_offset -
                                                      region->mmaps[i].offset);
                        buffer_mmaped = true;
                        break;
                    }
                }
            }

            if (!buffer_mmaped) {
                buf = g_malloc0(data_size);
            }

            qemu_get_buffer(f, buf, data_size);

            ret = pwrite(vbasedev->fd, &data_size, sizeof(data_size),
                         region->fd_offset +
                       offsetof(struct vfio_device_migration_info, data_size));
            if (ret != sizeof(data_size)) {
                error_report("Failed to set migration buffer data size %d",
                             ret);
                return -EINVAL;
            }

            if (!buffer_mmaped) {
                ret = pwrite(vbasedev->fd, buf, data_size,
                             region->fd_offset + data_offset);
                g_free(buf);

                if (ret != data_size) {
                    error_report("Failed to set migration buffer %d", ret);
                    return -EINVAL;
                }
            }
            trace_vfio_load_state_device_data(vbasedev->name, data_offset,
                                              data_size);
            break;
        }
        }

        ret = qemu_file_get_error(f);
        if (ret) {
            return ret;
        }
        data = qemu_get_be64(f);
    }

    return ret;
}

static SaveVMHandlers savevm_vfio_handlers = {
    .save_setup = vfio_save_setup,
    .save_cleanup = vfio_save_cleanup,
    .save_live_pending = vfio_save_pending,
    .save_live_iterate = vfio_save_iterate,
    .save_live_complete_precopy = vfio_save_complete_precopy,
    .load_setup = vfio_load_setup,
    .load_cleanup = vfio_load_cleanup,
    .load_state = vfio_load_state,
};

/* ---------------------------------------------------------------------- */

static void vfio_vmstate_change(void *opaque, int running, RunState state)
{
    VFIODevice *vbasedev = opaque;

    if ((vbasedev->vm_running != running) && running) {
        int ret;

        ret = vfio_migration_set_state(vbasedev, VFIO_DEVICE_STATE_RUNNING);
        if (ret) {
            error_report("Failed to set state RUNNING");
        }
    }

    vbasedev->vm_running = running;
    trace_vfio_vmstate_change(vbasedev->name, running);
}

static void vfio_migration_state_notifier(Notifier *notifier, void *data)
{
    MigrationState *s = data;
    VFIODevice *vbasedev = container_of(notifier, VFIODevice, migration_state);
    int ret;

    trace_vfio_migration_state_notifier(vbasedev->name, s->state);

    switch (s->state) {
    case MIGRATION_STATUS_ACTIVE:
        if (vbasedev->device_state & VFIO_DEVICE_STATE_RUNNING) {
            if (vbasedev->vm_running) {
                ret = vfio_migration_set_state(vbasedev,
                          VFIO_DEVICE_STATE_RUNNING | VFIO_DEVICE_STATE_SAVING);
                if (ret) {
                    error_report("Failed to set state RUNNING and SAVING");
                }
            } else {
                ret = vfio_migration_set_state(vbasedev,
                                               VFIO_DEVICE_STATE_SAVING);
                if (ret) {
                    error_report("Failed to set state STOP and SAVING");
                }
            }
        } else {
            ret = vfio_migration_set_state(vbasedev,
                                           VFIO_DEVICE_STATE_RESUMING);
            if (ret) {
                error_report("Failed to set state RESUMING");
            }
        }
        return;

    case MIGRATION_STATUS_CANCELLING:
    case MIGRATION_STATUS_CANCELLED:
    case MIGRATION_STATUS_FAILED:
        ret = vfio_migration_set_state(vbasedev, VFIO_DEVICE_STATE_RUNNING);
        if (ret) {
            error_report("Failed to set state RUNNING");
        }
        return;
    }
}

static int vfio_migration_init(VFIODevice *vbasedev,
                               struct vfio_region_info *info)
{
    int ret;

    vbasedev->migration = g_new0(VFIOMigration, 1);
    vbasedev->migration->region.index = info->index;

    ret = vfio_migration_region_init(vbasedev);
    if (ret) {
        error_report("Failed to initialise migration region");
        return ret;
    }

    qemu_mutex_init(&vbasedev->migration->lock);
    register_savevm_live(NULL, "vfio", -1, 1, &savevm_vfio_handlers, vbasedev);
    vbasedev->vm_state = qemu_add_vm_change_state_handler(vfio_vmstate_change,
                                                          vbasedev);

    vbasedev->migration_state.notify = vfio_migration_state_notifier;
    add_migration_state_change_notifier(&vbasedev->migration_state);

    return 0;
}

/* ---------------------------------------------------------------------- */

int vfio_migration_probe(VFIODevice *vbasedev, Error **errp)
{
    struct vfio_region_info *info;
    int ret;

    ret = vfio_get_dev_region_info(vbasedev, VFIO_REGION_TYPE_MIGRATION,
                                   VFIO_REGION_SUBTYPE_MIGRATION, &info);
    if (ret) {
        Error *local_err = NULL;

        error_setg(&vbasedev->migration_blocker,
                   "VFIO device doesn't support migration");
        ret = migrate_add_blocker(vbasedev->migration_blocker, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            error_free(vbasedev->migration_blocker);
            return ret;
        }
    } else {
        trace_vfio_migration_probe(vbasedev->name, info->index);
        return vfio_migration_init(vbasedev, info);
    }

    return 0;
}

void vfio_migration_finalize(VFIODevice *vbasedev)
{
    if (!vbasedev->migration) {
        return;
    }

    if (vbasedev->migration_state.notify) {
        remove_migration_state_change_notifier(&vbasedev->migration_state);
    }

    if (vbasedev->vm_state) {
        qemu_del_vm_change_state_handler(vbasedev->vm_state);
    }

    if (vbasedev->migration_blocker) {
        migrate_del_blocker(vbasedev->migration_blocker);
        error_free(vbasedev->migration_blocker);
    }

    qemu_mutex_destroy(&vbasedev->migration->lock);
    vfio_migration_region_exit(vbasedev);
    g_free(vbasedev->migration);
}
