/*
 * QEMU seccomp mode 2 support with libseccomp
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Eduardo Otubo    <eotubo@br.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */
#include "qemu/osdep.h"
#include <seccomp.h>
#include "sysemu/seccomp.h"

/* For some architectures (notably ARM) cacheflush is not supported until
 * libseccomp 2.2.3, but configure enforces that we are using a more recent
 * version on those hosts, so it is OK for this check to be less strict.
 */
#if SCMP_VER_MAJOR >= 3
  #define HAVE_CACHEFLUSH
#elif SCMP_VER_MAJOR == 2 && SCMP_VER_MINOR >= 2
  #define HAVE_CACHEFLUSH
#endif

struct QemuSeccompSyscall {
    int32_t num;
    int type;
    uint8_t set;
};

static const struct QemuSeccompSyscall blacklist[] = {
    /* default set of syscalls to blacklist */
    { SCMP_SYS(reboot),                1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(swapon),                1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(swapoff),               1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(syslog),                1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(mount),                 1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(umount),                1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(kexec_load),            1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(afs_syscall),           1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(break),                 1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(ftime),                 1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(getpmsg),               1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(gtty),                  1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(lock),                  1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(mpx),                   1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(prof),                  1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(profil),                1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(putpmsg),               1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(security),              1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(stty),                  1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(tuxcall),               1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(ulimit),                1, QEMU_SECCOMP_SET_DEFAULT },
    { SCMP_SYS(vserver),               1, QEMU_SECCOMP_SET_DEFAULT },
    /* obsolete */
    { SCMP_SYS(setuid),                4, QEMU_SECCOMP_SET_PRIVILEGED },
    { SCMP_SYS(setgid),                4, QEMU_SECCOMP_SET_PRIVILEGED },
    { SCMP_SYS(setpgid),               4, QEMU_SECCOMP_SET_PRIVILEGED },
    { SCMP_SYS(setsid),                4, QEMU_SECCOMP_SET_PRIVILEGED },
    { SCMP_SYS(setreuid),              4, QEMU_SECCOMP_SET_PRIVILEGED },
    { SCMP_SYS(setregid),              4, QEMU_SECCOMP_SET_PRIVILEGED },
    { SCMP_SYS(setresuid),             4, QEMU_SECCOMP_SET_PRIVILEGED },
    { SCMP_SYS(setresgid),             4, QEMU_SECCOMP_SET_PRIVILEGED },
    { SCMP_SYS(setfsuid),              4, QEMU_SECCOMP_SET_PRIVILEGED },
    { SCMP_SYS(setfsgid),              4, QEMU_SECCOMP_SET_PRIVILEGED },
    /* spawn */
    { SCMP_SYS(fork),                  8, QEMU_SECCOMP_SET_SPAWN },
    { SCMP_SYS(vfork),                 8, QEMU_SECCOMP_SET_SPAWN },
    { SCMP_SYS(execve),                8, QEMU_SECCOMP_SET_SPAWN },
    /* resource control */
    { SCMP_SYS(getpriority),           16, QEMU_SECCOMP_SET_RESOURCECTL },
    { SCMP_SYS(setpriority),           16, QEMU_SECCOMP_SET_RESOURCECTL },
    { SCMP_SYS(sched_setparam),        16, QEMU_SECCOMP_SET_RESOURCECTL },
    { SCMP_SYS(sched_getparam),        16, QEMU_SECCOMP_SET_RESOURCECTL },
    { SCMP_SYS(sched_setscheduler),    16, QEMU_SECCOMP_SET_RESOURCECTL },
    { SCMP_SYS(sched_getscheduler),    16, QEMU_SECCOMP_SET_RESOURCECTL },
    { SCMP_SYS(sched_setaffinity),     16, QEMU_SECCOMP_SET_RESOURCECTL },
    { SCMP_SYS(sched_getaffinity),     16, QEMU_SECCOMP_SET_RESOURCECTL },
    { SCMP_SYS(sched_get_priority_max),16, QEMU_SECCOMP_SET_RESOURCECTL },
    { SCMP_SYS(sched_get_priority_min),16, QEMU_SECCOMP_SET_RESOURCECTL },
};


int seccomp_start(uint32_t seccomp_opts)
{
    int rc = 0;
    unsigned int i = 0;
    scmp_filter_ctx ctx;

    ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (ctx == NULL) {
        rc = -1;
        goto seccomp_return;
    }

    for (i = 0; i < ARRAY_SIZE(blacklist); i++) {
        switch (blacklist[i].set) {
        case QEMU_SECCOMP_SET_OBSOLETE:
            if (!(seccomp_opts & QEMU_SECCOMP_SET_OBSOLETE)) {
                goto add_syscall;
            } else {
                continue;
            }

            break;
        case QEMU_SECCOMP_SET_PRIVILEGED:
            if (seccomp_opts & QEMU_SECCOMP_SET_PRIVILEGED) {
                goto add_syscall;
            } else {
                continue;
            }

            break;
        case QEMU_SECCOMP_SET_SPAWN:
            if (seccomp_opts & QEMU_SECCOMP_SET_SPAWN) {
                goto add_syscall;
            } else {
                continue;
            }

            break;
        case QEMU_SECCOMP_SET_RESOURCECTL:
            if (seccomp_opts & QEMU_SECCOMP_SET_RESOURCECTL) {
                goto add_syscall;
            } else {
                continue;
            }

            break;
        default:
            goto add_syscall;
        }
add_syscall:
        rc = seccomp_rule_add(ctx, SCMP_ACT_KILL, blacklist[i].num, 0);
        if (rc < 0) {
            goto seccomp_return;
        }
    }

    rc = seccomp_load(ctx);

  seccomp_return:
    seccomp_release(ctx);
    return rc;
}
