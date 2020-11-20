/*
 * vhost software live migration ring
 *
 * SPDX-FileCopyrightText: Red Hat, Inc. 2020
 * SPDX-FileContributor: Author: Eugenio Pérez <eperezma@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VHOST_SW_LM_RING_H
#define VHOST_SW_LM_RING_H

#include "qemu/osdep.h"

#include "hw/virtio/virtio.h"
#include "hw/virtio/vhost.h"

typedef struct VhostShadowVirtqueue VhostShadowVirtqueue;

bool vhost_vring_kick(VhostShadowVirtqueue *vq);

VhostShadowVirtqueue *vhost_sw_lm_shadow_vq(struct vhost_dev *dev, int idx);

void vhost_sw_lm_shadow_vq_free(VhostShadowVirtqueue *vq);

#endif
