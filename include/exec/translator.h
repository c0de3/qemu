/*
 * Generic intermediate code generation.
 *
 * Copyright (C) 2016-2017 Lluís Vilanova <vilanova@ac.upc.edu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef EXEC__TRANSLATOR_H
#define EXEC__TRANSLATOR_H

/*
 * Include this header from a target-specific file, and add a
 *
 *     DisasContextBase base;
 *
 * member in your target-specific DisasContext.
 */


#include "exec/exec-all.h"
#include "tcg/tcg.h"


/**
 * DisasJumpType:
 * @DISAS_NEXT: Next instruction in program order.
 * @DISAS_TOO_MANY: Too many instructions translated.
 * @DISAS_NORETURN: Following code is dead.
 * @DISAS_TARGET_*: Start of target-specific conditions.
 *
 * What instruction to disassemble next.
 */
typedef enum DisasJumpType {
    DISAS_NEXT,
    DISAS_TOO_MANY,
    DISAS_NORETURN,
    DISAS_TARGET_0,
    DISAS_TARGET_1,
    DISAS_TARGET_2,
    DISAS_TARGET_3,
    DISAS_TARGET_4,
    DISAS_TARGET_5,
    DISAS_TARGET_6,
    DISAS_TARGET_7,
    DISAS_TARGET_8,
    DISAS_TARGET_9,
    DISAS_TARGET_10,
    DISAS_TARGET_11,
} DisasJumpType;

/**
 * DisasContextBase:
 * @tb: Translation block for this disassembly.
 * @pc_first: Address of first guest instruction in this TB.
 * @pc_next: Address of next guest instruction in this TB (current during
 *           disassembly).
 * @is_jmp: What instruction to disassemble next.
 * @num_insns: Number of translated instructions (including current).
 * @singlestep_enabled: "Hardware" single stepping enabled.
 *
 * Architecture-agnostic disassembly context.
 */
typedef struct DisasContextBase {
    TranslationBlock *tb;
    target_ulong pc_first;
    target_ulong pc_next;
    DisasJumpType is_jmp;
    unsigned int num_insns;
    bool singlestep_enabled;
} DisasContextBase;

/**
 * TranslatorOps:
 * @init_disas_context: Initialize a DisasContext struct (DisasContextBase has
 *                      already been initialized).
 * @tb_start: Start translating a new TB. Can override the maximum number of
 *            instructions to translate, as calculated by the generic code in
 *            translator_loop().
 * @insn_start: Start translating a new instruction.
 * @breakpoint_check: Check if a breakpoint did hit, in which case no more
 *                    breakpoints are checked. When called, the breakpoint has
 *                    already been checked to match the PC, but targets can
 *                    decide the breakpoint missed the address (e.g., due to
 *                    conditions encoded in their flags).
 * @translate_insn: Disassemble one instruction and return the PC for the next
 *                  one. Can set db->is_jmp to DISAS_TARGET or above to stop
 *                  translation.
 * @tb_stop: Stop translating a TB.
 * @disas_log: Print instruction disassembly to log.
 *
 * Target-specific operations for the generic translator loop.
 *
 * The following hooks can set DisasContextBase::is_jmp to stop the translation
 * loop:
 *
 * - insn_start(), translate_insn()
 *   -> is_jmp != DISAS_NEXT
 *
 * - insn_start(), breakpoint_check(), translate_insn()
 *   -> is_jmp == DISAS_NORETURN
 */
typedef struct TranslatorOps {
    void (*init_disas_context)(DisasContextBase *db, CPUState *cpu);
    void (*tb_start)(DisasContextBase *db, CPUState *cpu, int *max_insns);
    void (*insn_start)(DisasContextBase *db, CPUState *cpu);
    bool (*breakpoint_check)(DisasContextBase *db, CPUState *cpu,
                             const CPUBreakpoint *bp);
    target_ulong (*translate_insn)(DisasContextBase *db, CPUState *cpu);
    void (*tb_stop)(DisasContextBase *db, CPUState *cpu);
    void (*disas_log)(const DisasContextBase *db, CPUState *cpu);
} TranslatorOps;

/**
 * translator_loop:
 * @ops: Target-specific operations.
 * @db: Disassembly context.
 * @cpu: Target vCPU.
 * @tb: Translation block.
 *
 * Generic translator loop.
 *
 * Translation will stop in the following cases (in order):
 * - When set by #TranslatorOps::insn_start.
 * - When set by #TranslatorOps::translate_insn.
 * - When the TCG operation buffer is full.
 * - When single-stepping is enabled (system-wide or on the current vCPU).
 * - When too many instructions have been translated.
 */
void translator_loop(const TranslatorOps *ops, DisasContextBase *db,
                     CPUState *cpu, TranslationBlock *tb);

#endif  /* EXEC__TRANSLATOR_H */
