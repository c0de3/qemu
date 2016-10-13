#ifndef QEMU_HW_XEN_FRONTEND_H
#define QEMU_HW_XEN_FRONTEND_H

char *xenstore_read_fe_str(struct XenDevice *xendev, const char *node);
int xenstore_read_fe_int(struct XenDevice *xendev, const char *node, int *ival);
int xenstore_read_fe_uint64(struct XenDevice *xendev, const char *node,
                                                        uint64_t *uval);

void xen_be_frontend_changed(struct XenDevice *xendev, const char *node);

#endif /* QEMU_HW_XEN_FRONTEND_H */
