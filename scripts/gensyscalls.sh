#!/bin/sh

linux="$1"
output="$2"

TMP=$(mktemp -d)

if [ "$linux" = "" ] ; then
    echo "Needs path to linux source tree" 1>&2
    exit 1
fi

if [ "$output" = "" ] ; then
    output="$PWD"
fi

upper()
{
    echo "$1" | tr "[:lower:]" "[:upper:]" | tr "[:punct:]" "_"
}

qemu_arch()
{
    case "$1" in
    arm64)
        echo "aarch64"
        ;;
    *)
        upper "$1"
        ;;
    esac
}

read_includes()
{
    arch=$1
    bits=$2

     cpp -P -nostdinc -fdirectives-only \
        -D_UAPI_ASM_$(upper ${arch})_BITSPERLONG_H \
        -D__BITS_PER_LONG=${bits} \
        -I${linux}/arch/${arch}/include/uapi/ \
        -I${linux}/include/uapi \
        -I${TMP} \
        "${linux}/arch/${arch}/include/uapi/asm/unistd.h"
}

filter_defines()
{
    grep -e "#define __NR_" -e "#define __NR3264"
}

rename_defines()
{
    sed "s/ __NR_/ TARGET_NR_/g;s/(__NR_/(TARGET_NR_/g"
}

evaluate_values()
{
    sed "s/#define TARGET_NR_/QEMU TARGET_NR_/" | \
    cpp -P -nostdinc | \
    sed "s/^QEMU /#define /"
}

generate_syscall_nr()
{
    arch=$1
    bits=$2
    file="$3"
    guard="$(upper LINUX_USER_$(qemu_arch $arch)_$(basename "$file"))"

    (echo "/*"
    echo " * This file contains the system call numbers."
    echo " */"
    echo "#ifndef ${guard}"
    echo "#define ${guard}"
    echo
    read_includes $arch $bits | filter_defines | rename_defines | \
                                evaluate_values | sort -n -k 3
    echo
    echo "#endif /* ${guard} */"
    echo) > "$file"
}

mkdir "$TMP/asm"
> "$TMP/asm/bitsperlong.h"

generate_syscall_nr arm64 64 "$output/linux-user/aarch64/syscall_nr.h"
generate_syscall_nr nios2 32 "$output/linux-user/nios2/syscall_nr.h"
generate_syscall_nr openrisc 32 "$output/linux-user/openrisc/syscall_nr.h"

generate_syscall_nr riscv 32 "$output/linux-user/riscv/syscall32_nr.h"
generate_syscall_nr riscv 64 "$output/linux-user/riscv/syscall64_nr.h"
rm -fr "$TMP"
