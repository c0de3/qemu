#ifndef AARCH64_TARGET_SIGNAL_H
#define AARCH64_TARGET_SIGNAL_H

#include "cpu.h"

/* this struct defines a stack used during syscall handling */

typedef struct target_sigaltstack {
    abi_ulong ss_sp;
    abi_int ss_flags;
    abi_ulong ss_size;
} target_stack_t;


/*
 * sigaltstack controls
 */
#define TARGET_SS_ONSTACK 1
#define TARGET_SS_DISABLE 2

#define TARGET_MINSIGSTKSZ 2048
#define TARGET_SIGSTKSZ 8192

static inline abi_ulong get_sp_from_cpustate(CPUARMState *state)
{
   return state->xregs[31];
}

void setup_frame(int sig, struct target_sigaction *ka,
                 target_sigset_t *set, CPUARMState *env);
void setup_rt_frame(int sig, struct target_sigaction *ka,
                    target_siginfo_t *info, target_sigset_t *set,
                    CPUARMState *env);
#endif /* AARCH64_TARGET_SIGNAL_H */
