/*
 * Virtio vhost-user GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2018
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *     Marc-André Lureau <marcandre.lureau@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef VUGPU_VIRGL_H_
#define VUGPU_VIRGL_H_

#include "vugpu.h"

int vg_virgl_init(VuGpu *g);
void vg_virgl_process_cmd(VuGpu *vg, struct virtio_gpu_ctrl_command *cmd);
void vg_virgl_update_cursor_data(VuGpu *g, uint32_t resource_id,
                                 gpointer data);

#endif
