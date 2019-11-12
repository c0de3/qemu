#!/usr/bin/env python
#
# Test virtio-scsi and virtio-blk queue settings for all machine types
#
# Copyright (c) 2019 Virtuozzo International GmbH
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

import sys
import os
import re

sys.path.append(os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
from qemu.machine import QEMUMachine
from avocado_qemu import Test

#list of machine types and virtqueue properties to test
VIRTIO_SCSI_PROPS = {'seg_max_adjust': 'seg_max_adjust'}
VIRTIO_BLK_PROPS = {'seg_max_adjust': 'seg-max-adjust'}

DEV_TYPES = {'virtio-scsi-pci': VIRTIO_SCSI_PROPS,
             'virtio-blk-pci': VIRTIO_BLK_PROPS}

VM_DEV_PARAMS = {'virtio-scsi-pci': ['-device', 'virtio-scsi-pci,id=scsi0'],
                 'virtio-blk-pci': ['-device',
                                    'virtio-blk-pci,id=scsi0,drive=drive0',
                                    '-drive',
                                    'driver=null-co,id=drive0,if=none']}


class VirtioMaxSegSettingsCheck(Test):
    @staticmethod
    def make_pattern(props):
        pattern_items = ['{0} = \w+'.format(prop) for prop in props]
        return '|'.join(pattern_items)

    def query_virtqueue(self, vm, dev_type_name):
        query_ok = False
        error = None
        props = None

        output = vm.command('human-monitor-command',
                            command_line = 'info qtree')
        props_list = DEV_TYPES[dev_type_name].values();
        pattern = self.make_pattern(props_list)
        res = re.findall(pattern, output)

        if len(res) != len(props_list):
            props_list = set(props_list)
            res = set(res)
            not_found = props_list.difference(res)
            not_found = ', '.join(not_found)
            error = '({0}): The following properties not found: {1}'\
                     .format(dev_type_name, not_found)
        else:
            query_ok = True
            props = dict()
            for prop in res:
                p = prop.split(' = ')
                props[p[0]] = p[1]
        return query_ok, props, error

    def check_mt(self, mt, dev_type_name):
        with QEMUMachine(self.qemu_bin) as vm:
            vm.set_machine(mt["name"])
            for s in VM_DEV_PARAMS[dev_type_name]:
                vm.add_args(s)
            vm.launch()
            query_ok, props, error = self.query_virtqueue(vm, dev_type_name)

        if not query_ok:
            self.fail('machine type {0}: {1}'.format(mt['name'], error))

        for prop_name, prop_val in props.items():
            expected_val = mt[prop_name]
            self.assertEqual(expected_val, prop_val)

    @staticmethod
    def seg_max_adjust_enabled(mt):
        # machine types > 4.1 should have seg_max_adjust = true
        # others seg_max_adjust = false
        mt = mt.split("-")

        # machine types with one line name and name like pc-x.x
        if len(mt) <= 2:
            return False

        # machine types like pc-<chip_name>-x.x[.x]
        ver = mt[2]
        ver = ver.split(".");

        # all versions greater than 4.1 goes with seg_max_adjust enabled
        major = int(ver[0])
        minor = int(ver[1])

        if major > 4 or (major == 4 and minor > 1):
            return True
        return False

    def test_machine_types(self):
        # collect all machine types except 'none', 'isapc', 'microvm'
        with QEMUMachine(self.qemu_bin) as vm:
            vm.launch()
            machines = [m['name'] for m in vm.command('query-machines')]
            vm.shutdown()
        machines.remove('none')
        machines.remove('isapc')
        machines.remove('microvm')

        for dev_type in DEV_TYPES:
            # create the list of machine types and their parameters.
            mtypes = list()
            for m in machines:
                if self.seg_max_adjust_enabled(m):
                    enabled = 'true'
                else:
                    enabled = 'false'
                mtypes.append({'name': m,
                               DEV_TYPES[dev_type]['seg_max_adjust']: enabled})

            # test each machine type for a device type
            for mt in mtypes:
                self.check_mt(mt, dev_type)
