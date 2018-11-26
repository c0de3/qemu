/*
 * vhost-net support
 *
 * Copyright Red Hat, Inc. 2010
 *
 * Authors:
 *  Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "net/net.h"
#include "net/tap.h"
#include "net/vhost-user.h"

#include "hw/virtio/virtio-net.h"
#include "net/vhost_net.h"
#include "qemu/error-report.h"


uint64_t vhost_net_get_max_queues(VHostNetState *net)
{
    return 1;
}

struct vhost_net *vhost_net_init(VhostNetOptions *options)
{
    error_report("vhost-net support is not compiled in");
    return NULL;
}

int vhost_net_start(VirtIODevice *dev,
                    NetClientState *ncs,
                    int total_queues)
{
    return -ENOSYS;
}
void vhost_net_stop(VirtIODevice *dev,
                    NetClientState *ncs,
                    int total_queues)
{
}

void vhost_net_cleanup(struct vhost_net *net)
{
}

uint64_t vhost_net_get_features(struct vhost_net *net, uint64_t features)
{
    return features;
}

void vhost_net_ack_features(struct vhost_net *net, uint64_t features)
{
}

uint64_t vhost_net_get_acked_features(VHostNetState *net)
{
    return 0;
}

bool vhost_net_virtqueue_pending(VHostNetState *net, int idx)
{
    return false;
}

void vhost_net_virtqueue_mask(VHostNetState *net, VirtIODevice *dev,
                              int idx, bool mask)
{
}

int vhost_net_notify_migration_done(struct vhost_net *net, char* mac_addr)
{
    return -1;
}

VHostNetState *get_vhost_net(NetClientState *nc)
{
    return 0;
}

int vhost_set_vring_enable(NetClientState *nc, int enable)
{
    return 0;
}

int vhost_net_set_mtu(struct vhost_net *net, uint16_t mtu)
{
    return 0;
}
