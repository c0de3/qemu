/*
 * PEF (Protected Execution Framework) for POWER support
 *
 * Copyright David Gibson, Redhat Inc. 2020
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"

#define TYPE_PEF_GUEST "pef-guest"
#define PEF_GUEST(obj)                                  \
    OBJECT_CHECK(PefGuestState, (obj), TYPE_SEV_GUEST)

typedef struct PefGuestState PefGuestState;

/**
 * PefGuestState:
 *
 * The PefGuestState object is used for creating and managing a PEF
 * guest.
 *
 * # $QEMU \
 *         -object pef-guest,id=pef0 \
 *         -machine ...,guest-memory-protection=pef0
 */
struct PefGuestState {
    Object parent_obj;
};

static Error *pef_mig_blocker;

static int pef_kvm_init(GuestMemoryProtection *gmpo, Error **errp)
{
    PefGuestState *pef = PEF_GUEST(gmpo);

    if (!kvm_check_extension(kvm_state, KVM_CAP_PPC_SECURE_GUEST)) {
        error_setg(errp,
                   "KVM implementation does not support Secure VMs (is an ultravisor running?)");
        return -1;
    } else {
        int ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_PPC_SECURE_GUEST, 0, 1);

        if (ret < 0) {
            error_setg(errp,
                       "Error enabling PEF with KVM");
            return -1;
        }
    }

    return 0;
}

static void pef_guest_class_init(ObjectClass *oc, void *data)
{
    GuestMemoryProtectionClass *gmpc = GUEST_MEMORY_PROTECTION_CLASS(oc);

    gmpc->kvm_init = pef_kvm_init;
}

static const TypeInfo pef_guest_info = {
    .parent = TYPE_OBJECT,
    .name = TYPE_PEF_GUEST,
    .instance_size = sizeof(PefGuestState),
    .class_init = pef_guest_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_GUEST_MEMORY_PROTECTION },
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void
pef_register_types(void)
{
    type_register_static(&pef_guest_info);
}

type_init(pef_register_types);
