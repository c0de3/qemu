/*
 * Device vmstate
 *
 * Copyright (c) 2019 GreenSocs
 *
 * Authors:
 *   Damien Hedde
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/qdev.h"
#include "migration/vmstate.h"

static bool device_vmstate_reset_needed(void *opaque)
{
    DeviceState *dev = (DeviceState *) opaque;
    return dev->resetting;
}

const struct VMStateDescription device_vmstate_reset = {
    .name = "device_reset",
    .version_id = 0,
    .minimum_version_id = 0,
    .needed = device_vmstate_reset_needed,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(resetting, DeviceState),
        VMSTATE_BOOL(cold_reset_input.state, DeviceState),
        VMSTATE_BOOL(warm_reset_input.state, DeviceState),
        VMSTATE_END_OF_LIST()
    }
};
