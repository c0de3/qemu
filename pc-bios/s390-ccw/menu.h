/*
 * QEMU S390 Interactive Boot Menu
 *
 * Copyright 2017 IBM Corp.
 * Author: Collin L. Walling <walling@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#ifndef MENU_H
#define MENU_H

#include "libc.h"

#define BOOT_MENU_FLAG_BOOT_OPTS 0x80
#define BOOT_MENU_FLAG_ZIPL_OPTS 0x40

/* Offsets from zipl fields to zipl banner start */
#define ZIPL_TIMEOUT_OFFSET 138
#define ZIPL_FLAG_OFFSET    140

typedef struct ZiplParms {
    uint16_t flag;
    uint16_t timeout;
    uint64_t menu_start;
} ZiplParms;

int menu_get_zipl_boot_index(const void *stage2, ZiplParms zipl_parms);
int menu_get_enum_boot_index(int entries);
void menu_set_parms(uint8_t boot_menu_flags, uint16_t boot_menu_timeout);
bool menu_check_flags(uint8_t check_flags);

#endif /* MENU_H */
