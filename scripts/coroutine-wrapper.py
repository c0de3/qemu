#!/usr/bin/env python3
#
# Generate coroutine wrappers for block subsystem.
#
# Copyright (c) 2020 Virtuozzo International GmbH.
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

import re
from typing import List, Iterator

header = '''/*
 * File is generated by scripts/coroutine-wrapper.py
 */

#include "qemu/osdep.h"
#include "block/coroutines.h"
#include "block/block-gen.h"'''

template = """

/*
 * Wrappers for $name$
 */

typedef struct $struct_name$ {
    BdrvPollCo poll_state;
    $fields$
} $struct_name$;

static void coroutine_fn $name$_entry(void *opaque)
{
    $struct_name$ *s = opaque;

    $assign_ret$$name$($args_from_s$);

    s->poll_state.in_progress = false;

    bdrv_poll_co__on_exit();
}

$ret_type$ $wrapper_name$($args_def$)
{
    if (qemu_in_coroutine()) {
        $do_return$$name$($arg_names$);
    } else {
        $struct_name$ s = {
            .poll_state.bs = $bs$,
            .poll_state.in_progress = true,

            $initializers$
        };

        s.poll_state.co = qemu_coroutine_create($name$_entry, &s);

        $do_return$bdrv_poll_co(&s.poll_state);
    }
}"""

# We want to use python string.format() formatter, which uses curly brackets
# as separators. But it's not comfortable with C. So, we used dollars instead,
# and now is the time to escape curly brackets and convert dollars.
template = template.replace('{', '{{').replace('}', '}}')
template = re.sub(r'\$(\w+)\$', r'{\1}', template)


class ParamDecl:
    param_re = re.compile(r'(?P<decl>'
                          r'(?P<type>.*[ *])'
                          r'(?P<name>[a-z][a-z0-9_]*)'
                          r')')

    def __init__(self, param_decl: str) -> None:
        m = self.param_re.match(param_decl.strip())
        self.decl = m.group('decl')
        self.type = m.group('type')
        self.name = m.group('name')


class FuncDecl:
    def __init__(self, return_type: str, name: str, args: str) -> None:
        self.return_type = return_type.strip()
        self.name = name.strip()
        self.args: List[ParamDecl] = []
        self.args = [ParamDecl(arg) for arg in args.split(',')]

    def get_args_decl(self) -> str:
        return ', '.join(arg.decl for arg in self.args)

    def get_arg_names(self) -> str:
        return ', '.join(arg.name for arg in self.args)

    def gen_struct_fields(self) -> str:
        return '\n    '.join(f'{arg.decl};' for arg in self.args)

    def gen_struct_initializers(self, indent: int) -> str:
        sep = '\n' + ' ' * indent
        return sep.join(f'.{a.name} = {a.name},' for a in self.args)


# Match wrappers declaration, with generated_co_wrapper mark
func_decl_re = re.compile(r'^(?P<return_type>(int|void))'
                          r'\s*generated_co_wrapper\s*'
                          r'(?P<wrapper_name>[a-z][a-z0-9_]*)'
                          r'\((?P<args>[^)]*)\);$', re.MULTILINE)


def func_decl_iter(text: str) -> Iterator:
    for m in func_decl_re.finditer(text):
        yield FuncDecl(return_type=m.group('return_type'),
                       name=m.group('wrapper_name'),
                       args=m.group('args'))


def struct_name(func_name: str) -> str:
    """some_function_name -> SomeFunctionName"""
    words = func_name.split('_')
    words = [w[0].upper() + w[1:] for w in words]
    return ''.join(words)


def make_wrapper(func: FuncDecl) -> str:
    assert func.name.startswith('bdrv_')
    co_name = 'bdrv_co_' + func.name[5:]

    has_ret = func.return_type != 'void'

    params = {
        'name': co_name,
        'do_return': 'return ' if has_ret else '',
        'assign_ret': 's->poll_state.ret = ' if has_ret else '',
        'struct_name': struct_name(co_name),
        'wrapper_name': func.name,
        'ret_type': func.return_type,
        'args_def': func.get_args_decl(),
        'arg_names': func.get_arg_names(),
        'fields': func.gen_struct_fields(),
        'initializers': func.gen_struct_initializers(12),
        'args_from_s': ', '.join(f's->{a.name}' for a in func.args),
    }

    if func.args[0].type == 'BlockDriverState *':
        params['bs'] = 'bs'
    else:
        assert func.args[0].type == 'BdrvChild *'
        params['bs'] = 'child->bs'

    return template.format(**params)


if __name__ == '__main__':
    import sys

    print(header)
    for func in func_decl_iter(sys.stdin.read()):
        print(make_wrapper(func))
