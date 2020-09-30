# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.

"""
QAPI Generator

This is the main entry point for generating C code from the QAPI schema.
"""

import argparse
import re
import sys

from qapi.commands import gen_commands
from qapi.error import QAPIError
from qapi.events import gen_events
from qapi.introspect import gen_introspect
from qapi.schema import QAPISchema
from qapi.types import gen_types
from qapi.visit import gen_visit


DEFAULT_OUTPUT_DIR = ''
DEFAULT_PREFIX = ''


def generate(schema_file: str,
             output_dir: str,
             prefix: str,
             unmask: bool = False,
             builtins: bool = False) -> None:
    """
    generate uses a given schema to produce C code in the target directory.

    :param schema_file: The primary QAPI schema file.
    :param output_dir: The output directory to store generated code.
    :param prefix: Optional C-code prefix for symbol names.
    :param unmask: Expose non-ABI names through introspection?
    :param builtins: Generate code for built-in types?

    :raise QAPIError: On failures.
    """
    match = re.match(r'([A-Za-z_.-][A-Za-z0-9_.-]*)?', prefix)
    if match.end() != len(prefix):
        msg = "funny character '{:s}' in prefix '{:s}'".format(
            prefix[match.end()], prefix)
        raise QAPIError('', None, msg)

    schema = QAPISchema(schema_file)
    gen_types(schema, output_dir, prefix, builtins)
    gen_visit(schema, output_dir, prefix, builtins)
    gen_commands(schema, output_dir, prefix)
    gen_events(schema, output_dir, prefix)
    gen_introspect(schema, output_dir, prefix, unmask)


def main() -> int:
    """
    gapi-gen shell script entrypoint.
    Expects arguments via sys.argv, see --help for details.

    :return: int, 0 on success, 1 on failure.
    """
    parser = argparse.ArgumentParser(
        description='Generate code from a QAPI schema')
    parser.add_argument('-b', '--builtins', action='store_true',
                        help="generate code for built-in types")
    parser.add_argument('-o', '--output-dir', action='store',
                        default=DEFAULT_OUTPUT_DIR,
                        help="write output to directory OUTPUT_DIR")
    parser.add_argument('-p', '--prefix', action='store',
                        default=DEFAULT_PREFIX,
                        help="prefix for symbols")
    parser.add_argument('-u', '--unmask-non-abi-names', action='store_true',
                        dest='unmask',
                        help="expose non-ABI names in introspection")
    parser.add_argument('schema', action='store')
    args = parser.parse_args()

    try:
        generate(args.schema,
                 output_dir=args.output_dir,
                 prefix=args.prefix,
                 unmask=args.unmask,
                 builtins=args.builtins)
    except QAPIError as err:
        print(f"{sys.argv[0]}: {str(err)}", file=sys.stderr)
        return 1
    return 0
