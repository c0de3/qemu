/*
 *  Dynamic device configuration and creation -- buses.
 *
 *  Copyright (c) 2009 CodeSourcery
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "qemu/ctype.h"
#include "qemu/module.h"
#include "qapi/error.h"

void qbus_set_hotplug_handler(BusState *bus, Object *handler, Error **errp)
{
    object_property_set_link(OBJECT(bus), OBJECT(handler),
                             QDEV_HOTPLUG_HANDLER_PROPERTY, errp);
}

void qbus_set_bus_hotplug_handler(BusState *bus, Error **errp)
{
    qbus_set_hotplug_handler(bus, OBJECT(bus), errp);
}

int qbus_walk_children(BusState *bus,
                       qdev_walkerfn *pre_devfn, qbus_walkerfn *pre_busfn,
                       qdev_walkerfn *post_devfn, qbus_walkerfn *post_busfn,
                       void *opaque)
{
    BusChild *kid;
    int err;

    if (pre_busfn) {
        err = pre_busfn(bus, opaque);
        if (err) {
            return err;
        }
    }

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        err = qdev_walk_children(kid->child,
                                 pre_devfn, pre_busfn,
                                 post_devfn, post_busfn, opaque);
        if (err < 0) {
            return err;
        }
    }

    if (post_busfn) {
        err = post_busfn(bus, opaque);
        if (err) {
            return err;
        }
    }

    return 0;
}

bool bus_is_in_reset(BusState *bus)
{
    return resettable_is_in_reset(OBJECT(bus));
}

static ResettableState *bus_get_reset_state(Object *obj)
{
    BusState *bus = BUS(obj);
    return &bus->reset;
}

static void bus_reset_child_foreach(Object *obj, ResettableChildCallback cb,
                                    void *opaque, ResetType type)
{
    BusState *bus = BUS(obj);
    BusChild *kid;

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        cb(OBJECT(kid->child), opaque, type);
    }
}

static void qbus_realize(BusState *bus, DeviceState *parent, const char *name)
{
    const char *typename = object_get_typename(OBJECT(bus));
    BusClass *bc;
    int i, bus_id;

    bus->parent = parent;

    if (name) {
        bus->name = g_strdup(name);
    } else if (bus->parent && bus->parent->id) {
        /* parent device has id -> use it plus parent-bus-id for bus name */
        bus_id = bus->parent->num_child_bus;
        bus->name = g_strdup_printf("%s.%d", bus->parent->id, bus_id);
    } else {
        /* no id -> use lowercase bus type plus global bus-id for bus name */
        bc = BUS_GET_CLASS(bus);
        bus_id = bc->automatic_ids++;
        bus->name = g_strdup_printf("%s.%d", typename, bus_id);
        for (i = 0; bus->name[i]; i++) {
            bus->name[i] = qemu_tolower(bus->name[i]);
        }
    }

    if (bus->parent) {
        QLIST_INSERT_HEAD(&bus->parent->child_bus, bus, sibling);
        bus->parent->num_child_bus++;
        object_property_add_child(OBJECT(bus->parent), bus->name, OBJECT(bus), NULL);
        object_unref(OBJECT(bus));
    } else {
        /* The only bus without a parent is the main system bus */
        assert(bus == sysbus_get_default());
    }
}

static void bus_unparent(Object *obj)
{
    BusState *bus = BUS(obj);
    BusChild *kid;

    /* Only the main system bus has no parent, and that bus is never freed */
    assert(bus->parent);

    while ((kid = QTAILQ_FIRST(&bus->children)) != NULL) {
        DeviceState *dev = kid->child;
        object_unparent(OBJECT(dev));
    }
    QLIST_REMOVE(bus, sibling);
    bus->parent->num_child_bus--;
    bus->parent = NULL;
}

void qbus_create_inplace(void *bus, size_t size, const char *typename,
                         DeviceState *parent, const char *name)
{
    object_initialize(bus, size, typename);
    qbus_realize(bus, parent, name);
}

BusState *qbus_create(const char *typename, DeviceState *parent, const char *name)
{
    BusState *bus;

    bus = BUS(object_new(typename));
    qbus_realize(bus, parent, name);

    return bus;
}

static bool bus_get_realized(Object *obj, Error **errp)
{
    BusState *bus = BUS(obj);

    return bus->realized;
}

static void bus_set_realized(Object *obj, bool value, Error **errp)
{
    BusState *bus = BUS(obj);
    BusClass *bc = BUS_GET_CLASS(bus);
    BusChild *kid;
    Error *local_err = NULL;

    if (value && !bus->realized) {
        if (bc->realize) {
            bc->realize(bus, &local_err);
        }

        /* TODO: recursive realization */
    } else if (!value && bus->realized) {
        QTAILQ_FOREACH(kid, &bus->children, sibling) {
            DeviceState *dev = kid->child;
            object_property_set_bool(OBJECT(dev), false, "realized",
                                     &local_err);
            if (local_err != NULL) {
                break;
            }
        }
        if (bc->unrealize && local_err == NULL) {
            bc->unrealize(bus, &local_err);
        }
    }

    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    bus->realized = value;
}

static void qbus_initfn(Object *obj)
{
    BusState *bus = BUS(obj);

    QTAILQ_INIT(&bus->children);
    object_property_add_link(obj, QDEV_HOTPLUG_HANDLER_PROPERTY,
                             TYPE_HOTPLUG_HANDLER,
                             (Object **)&bus->hotplug_handler,
                             object_property_allow_set_link,
                             0,
                             NULL);
    object_property_add_bool(obj, "realized",
                             bus_get_realized, bus_set_realized, NULL);
}

static char *default_bus_get_fw_dev_path(DeviceState *dev)
{
    return g_strdup(object_get_typename(OBJECT(dev)));
}

/**
 * bus_phases_reset:
 * Transition reset method for buses to allow moving
 * smoothly from legacy reset method to multi-phases
 */
static void bus_phases_reset(BusState *bus)
{
    ResettableClass *rc = RESETTABLE_GET_CLASS(bus);

    if (rc->phases.enter) {
        rc->phases.enter(OBJECT(bus), RESET_TYPE_COLD);
    }
    if (rc->phases.hold) {
        rc->phases.hold(OBJECT(bus));
    }
    if (rc->phases.exit) {
        rc->phases.exit(OBJECT(bus));
    }
}

static void bus_transitional_reset(Object *obj)
{
    BusClass *bc = BUS_GET_CLASS(obj);

    /*
     * This will call either @bus_phases_reset (for multi-phases transitioned
     * buses) or a bus's specific method for not-yet transitioned buses.
     * In both case, it does not reset children.
     */
    if (bc->reset) {
        bc->reset(BUS(obj));
    }
}

/**
 * bus_get_transitional_reset:
 * check if the bus's class is ready for multi-phase
 */
static ResettableTrFunction bus_get_transitional_reset(Object *obj)
{
    BusClass *dc = BUS_GET_CLASS(obj);
    if (dc->reset != bus_phases_reset) {
        /*
         * dc->reset has been overridden by a subclass,
         * the bus is not ready for multi phase yet.
         */
        return bus_transitional_reset;
    }
    return NULL;
}

static void bus_class_init(ObjectClass *class, void *data)
{
    BusClass *bc = BUS_CLASS(class);
    ResettableClass *rc = RESETTABLE_CLASS(class);

    class->unparent = bus_unparent;
    bc->get_fw_dev_path = default_bus_get_fw_dev_path;

    rc->get_state = bus_get_reset_state;
    rc->child_foreach = bus_reset_child_foreach;

    /*
     * @bus_phases_reset is put as the default reset method below, allowing
     * to do the multi-phase transition from base classes to leaf classes. It
     * allows a legacy-reset Bus class to extend a multi-phases-reset
     * Bus class for the following reason:
     * + If a base class B has been moved to multi-phase, then it does not
     *   override this default reset method and may have defined phase methods.
     * + A daughter class C (extending class B) which uses
     *   bus_class_set_parent_reset() (or similar means) to override the
     *   reset method will still work as expected. @bus_phases_reset function
     *   will be registered as the parent reset method and effectively call
     *   parent reset phases.
     */
    bc->reset = bus_phases_reset;
    rc->get_transitional_function = bus_get_transitional_reset;
}

static void qbus_finalize(Object *obj)
{
    BusState *bus = BUS(obj);

    g_free(bus->name);
}

static const TypeInfo bus_info = {
    .name = TYPE_BUS,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(BusState),
    .abstract = true,
    .class_size = sizeof(BusClass),
    .instance_init = qbus_initfn,
    .instance_finalize = qbus_finalize,
    .class_init = bus_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_RESETTABLE_INTERFACE },
        { }
    },
};

static void bus_register_types(void)
{
    type_register_static(&bus_info);
}

type_init(bus_register_types)
