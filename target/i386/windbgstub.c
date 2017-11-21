/*
 * windbgstub.c
 *
 * Copyright (c) 2010-2017 Institute for System Programming
 *                         of the Russian Academy of Sciences.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"

#ifndef TARGET_X86_64
#include "exec/windbgstub-utils.h"

bool windbg_on_load(void)
{
    return false;
}

#endif
