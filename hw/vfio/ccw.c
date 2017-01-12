/*
 * vfio based subchannel assignment support
 *
 * Copyright 2017 IBM Corp.
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 *            Xiao Feng Ren <renxiaof@linux.vnet.ibm.com>
 *            Pierre Morel <pmorel@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or(at
 * your option) any version. See the COPYING file in the top-level
 * directory.
 */

#include <linux/vfio.h>
#include <sys/ioctl.h>

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/vfio/vfio.h"
#include "hw/vfio/vfio-common.h"
#include "hw/s390x/s390-ccw.h"
#include "hw/s390x/ccw-device.h"

#define TYPE_VFIO_CCW "vfio-ccw"
typedef struct VFIOCCWDevice {
    S390CCWDevice cdev;
    VFIODevice vdev;
} VFIOCCWDevice;

static void vfio_ccw_compute_needs_reset(VFIODevice *vdev)
{
    vdev->needs_reset = false;
}

/*
 * We don't need vfio_hot_reset_multi and vfio_eoi operationis for
 * vfio_ccw device now.
 */
struct VFIODeviceOps vfio_ccw_ops = {
    .vfio_compute_needs_reset = vfio_ccw_compute_needs_reset,
};

static void vfio_ccw_reset(DeviceState *dev)
{
    CcwDevice *ccw_dev = DO_UPCAST(CcwDevice, parent_obj, dev);
    S390CCWDevice *cdev = DO_UPCAST(S390CCWDevice, parent_obj, ccw_dev);
    VFIOCCWDevice *vcdev = DO_UPCAST(VFIOCCWDevice, cdev, cdev);

    ioctl(vcdev->vdev.fd, VFIO_DEVICE_RESET);
}

static void vfio_put_device(VFIOCCWDevice *vcdev)
{
    g_free(vcdev->vdev.name);
    vfio_put_base_device(&vcdev->vdev);
}

static VFIOGroup *vfio_ccw_get_group(S390CCWDevice *cdev, char **path,
                                     Error **errp)
{
    struct stat st;
    int groupid;
    GError *gerror = NULL;

    /* Check that host subchannel exists. */
    path[0] = g_strdup_printf("/sys/bus/css/devices/%x.%x.%04x",
                              cdev->hostid.cssid,
                              cdev->hostid.ssid,
                              cdev->hostid.devid);
    if (stat(path[0], &st) < 0) {
        error_setg(errp, "vfio: no such host subchannel %s", path[0]);
        return NULL;
    }

    /* Check that mediated device exists. */
    path[1] = g_strdup_printf("%s/%s", path[0], cdev->mdevid);
    if (stat(path[0], &st) < 0) {
        error_setg(errp, "vfio: no such mediated device %s", path[1]);
        return NULL;
    }

    /* Get the iommu_group patch as the interim variable. */
    path[2] = g_strconcat(path[1], "/iommu_group", NULL);

    /* Get the link file path of the device iommu_group. */
    path[3] = g_file_read_link(path[2], &gerror);
    if (!path[3]) {
        error_setg(errp, "vfio: error no iommu_group for subchannel");
        return NULL;
    }

    /* Get the device groupid. */
    if (sscanf(basename(path[3]), "%d", &groupid) != 1) {
        error_setg(errp, "vfio: error reading %s:%m", path[3]);
        return NULL;
    }

    return vfio_get_group(groupid, &address_space_memory, errp);
}

static void vfio_ccw_put_group(VFIOGroup *group, char **path)
{
    g_free(path);
    vfio_put_group(group);
}

static void vfio_ccw_realize(DeviceState *dev, Error **errp)
{
    VFIODevice *vbasedev;
    VFIOGroup *group;
    CcwDevice *ccw_dev = DO_UPCAST(CcwDevice, parent_obj, dev);
    S390CCWDevice *cdev = DO_UPCAST(S390CCWDevice, parent_obj, ccw_dev);
    VFIOCCWDevice *vcdev = DO_UPCAST(VFIOCCWDevice, cdev, cdev);
    S390CCWDeviceClass *cdc = S390_CCW_DEVICE_GET_CLASS(cdev);
    char *path[4] = {NULL, NULL, NULL, NULL};

    /* Call the class init function for subchannel. */
    if (cdc->realize) {
        cdc->realize(cdev, errp);
        if (*errp) {
            return;
        }
    }

    group = vfio_ccw_get_group(cdev, path, errp);
    if (!group) {
        goto out_group_err;
    }

    vcdev->vdev.ops = &vfio_ccw_ops;
    vcdev->vdev.type = VFIO_DEVICE_TYPE_CCW;
    vcdev->vdev.name = g_strdup_printf("%x.%x.%04x", cdev->hostid.cssid,
                                       cdev->hostid.ssid, cdev->hostid.devid);
    QLIST_FOREACH(vbasedev, &group->device_list, next) {
        if (strcmp(vbasedev->name, vcdev->vdev.name) == 0) {
            error_setg(errp, "vfio: subchannel %s has already been attached",
                       basename(path[0]));
            goto out_device_err;
        }
    }

    if (vfio_get_device(group, cdev->mdevid, &vcdev->vdev, errp)) {
        goto out_device_err;
    }

    return;

out_device_err:
    vfio_ccw_put_group(group, path);
out_group_err:
    if (cdc->unrealize) {
        cdc->unrealize(cdev, errp);
    }
}

static void vfio_ccw_unrealize(DeviceState *dev, Error **errp)
{
    CcwDevice *ccw_dev = DO_UPCAST(CcwDevice, parent_obj, dev);
    S390CCWDevice *cdev = DO_UPCAST(S390CCWDevice, parent_obj, ccw_dev);
    VFIOCCWDevice *vcdev = DO_UPCAST(VFIOCCWDevice, cdev, cdev);
    S390CCWDeviceClass *cdc = S390_CCW_DEVICE_GET_CLASS(cdev);
    VFIOGroup *group = vcdev->vdev.group;

    if (cdc->unrealize) {
        cdc->unrealize(cdev, errp);
    }

    vfio_put_device(vcdev);
    vfio_put_group(group);
}

static Property vfio_ccw_properties[] = {
    DEFINE_PROP_CSS_DEV_ID("hostid", VFIOCCWDevice, cdev.hostid),
    DEFINE_PROP_CSS_DEV_ID("guestid", VFIOCCWDevice, cdev.parent_obj.bus_id),
    DEFINE_PROP_STRING("mdevid", VFIOCCWDevice, cdev.mdevid),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vfio_ccw_vmstate = {
    .name = TYPE_VFIO_CCW,
    .unmigratable = 1,
};

static void vfio_ccw_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->props = vfio_ccw_properties;
    dc->vmsd = &vfio_ccw_vmstate;
    dc->desc = "VFIO-based subchannel assignment";
    dc->realize = vfio_ccw_realize;
    dc->unrealize = vfio_ccw_unrealize;
    dc->reset = vfio_ccw_reset;
}

static const TypeInfo vfio_ccw_info = {
    .name = TYPE_VFIO_CCW,
    .parent = TYPE_S390_CCW,
    .instance_size = sizeof(VFIOCCWDevice),
    .class_init = vfio_ccw_class_init,
};

static void register_vfio_ccw_type(void)
{
    type_register_static(&vfio_ccw_info);
}

type_init(register_vfio_ccw_type)
