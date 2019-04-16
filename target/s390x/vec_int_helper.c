/*
 * QEMU TCG support -- s390x vector integer instruction support
 *
 * Copyright (C) 2019 Red Hat Inc
 *
 * Authors:
 *   David Hildenbrand <david@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "cpu.h"
#include "vec.h"
#include "exec/helper-proto.h"

static bool s390_vec_is_zero(const S390Vector *v)
{
    return !v->doubleword[0] && !v->doubleword[1];
}

static void s390_vec_xor(S390Vector *res, const S390Vector *a,
                         const S390Vector *b)
{
    res->doubleword[0] = a->doubleword[0] ^ b->doubleword[0];
    res->doubleword[1] = a->doubleword[1] ^ b->doubleword[1];
}

static void s390_vec_shl(S390Vector *d, const S390Vector *a, uint64_t count)
{
    uint64_t tmp;

    g_assert(count < 128);
    if (count == 0) {
        d->doubleword[0] = a->doubleword[0];
        d->doubleword[1] = a->doubleword[1];
    } else if (count == 64) {
        d->doubleword[0] = a->doubleword[1];
        d->doubleword[1] = 0;
    } else if (count < 64) {
        tmp = extract64(a->doubleword[1], 64 - count, count);
        d->doubleword[1] = a->doubleword[1] << count;
        d->doubleword[0] = (a->doubleword[0] << count) | tmp;
    } else {
        d->doubleword[0] = a->doubleword[1] << (count - 64);
        d->doubleword[1] = 0;
    }
}

static void s390_vec_shr(S390Vector *d, const S390Vector *a, uint64_t count)
{
    uint64_t tmp;

    g_assert(count < 128);
    if (count == 0) {
        d->doubleword[0] = a->doubleword[0];
        d->doubleword[1] = a->doubleword[1];
    } else if (count == 64) {
        d->doubleword[1] = a->doubleword[0];
        d->doubleword[0] = 0;
    } else if (count < 64) {
        tmp = a->doubleword[1] >> count;
        d->doubleword[1] = deposit64(tmp, 64 - count, count, a->doubleword[0]);
        d->doubleword[0] = a->doubleword[0] >> count;
    } else {
        d->doubleword[1] = a->doubleword[0] >> (count - 64);
        d->doubleword[0] = 0;
    }
}
#define DEF_VAVG(BITS)                                                         \
void HELPER(gvec_vavg##BITS)(void *v1, const void *v2, const void *v3,         \
                             uint32_t desc)                                    \
{                                                                              \
    int i;                                                                     \
                                                                               \
    for (i = 0; i < (128 / BITS); i++) {                                       \
        const int32_t a = (int##BITS##_t)s390_vec_read_element##BITS(v2, i);   \
        const int32_t b = (int##BITS##_t)s390_vec_read_element##BITS(v3, i);   \
                                                                               \
        s390_vec_write_element##BITS(v1, i, (a + b + 1) >> 1);                 \
    }                                                                          \
}
DEF_VAVG(8)
DEF_VAVG(16)

#define DEF_VAVGL(BITS)                                                        \
void HELPER(gvec_vavgl##BITS)(void *v1, const void *v2, const void *v3,        \
                              uint32_t desc)                                   \
{                                                                              \
    int i;                                                                     \
                                                                               \
    for (i = 0; i < (128 / BITS); i++) {                                       \
        const uint##BITS##_t a = s390_vec_read_element##BITS(v2, i);           \
        const uint##BITS##_t b = s390_vec_read_element##BITS(v3, i);           \
                                                                               \
        s390_vec_write_element##BITS(v1, i, (a + b + 1) >> 1);                 \
    }                                                                          \
}
DEF_VAVGL(8)
DEF_VAVGL(16)

#define DEF_VCLZ(BITS)                                                         \
void HELPER(gvec_vclz##BITS)(void *v1, const void *v2, uint32_t desc)          \
{                                                                              \
    int i;                                                                     \
                                                                               \
    for (i = 0; i < (128 / BITS); i++) {                                       \
        const uint##BITS##_t a = s390_vec_read_element##BITS(v2, i);           \
                                                                               \
        s390_vec_write_element##BITS(v1, i, clz32(a) - 32 + BITS);             \
    }                                                                          \
}
DEF_VCLZ(8)
DEF_VCLZ(16)

#define DEF_VCTZ(BITS)                                                         \
void HELPER(gvec_vctz##BITS)(void *v1, const void *v2, uint32_t desc)          \
{                                                                              \
    int i;                                                                     \
                                                                               \
    for (i = 0; i < (128 / BITS); i++) {                                       \
        const uint##BITS##_t a = s390_vec_read_element##BITS(v2, i);           \
                                                                               \
        s390_vec_write_element##BITS(v1, i, a ? ctz32(a) : BITS);              \
    }                                                                          \
}
DEF_VCTZ(8)
DEF_VCTZ(16)

/* like binary multiplication, but XOR instead of addition */
#define DEF_GALOIS_MULTIPLY(BITS, TBITS)                                       \
static uint##TBITS##_t galois_multiply##BITS(uint##TBITS##_t a,                \
                                             uint##TBITS##_t b)                \
{                                                                              \
    uint##TBITS##_t res = 0;                                                   \
                                                                               \
    while (b) {                                                                \
        if (b & 0x1) {                                                         \
            res = res ^ a;                                                     \
        }                                                                      \
        a = a << 1;                                                            \
        b = b >> 1;                                                            \
    }                                                                          \
    return res;                                                                \
}
DEF_GALOIS_MULTIPLY(8, 16)
DEF_GALOIS_MULTIPLY(16, 32)
DEF_GALOIS_MULTIPLY(32, 64)

static S390Vector galois_multiply64(uint64_t a, uint64_t b)
{
    S390Vector res = {};
    S390Vector va = {
        .doubleword[1] = a,
    };
    S390Vector vb = {
        .doubleword[1] = b,
    };

    while (!s390_vec_is_zero(&vb)) {
        if (vb.doubleword[1] & 0x1) {
            s390_vec_xor(&res, &res, &va);
        }
        s390_vec_shl(&va, &va, 1);
        s390_vec_shr(&vb, &vb, 1);
    }
    return res;
}

#define DEF_VGFM(BITS, TBITS)                                                  \
void HELPER(gvec_vgfm##BITS)(void *v1, const void *v2, const void *v3,         \
                             uint32_t desc)                                    \
{                                                                              \
    int i;                                                                     \
                                                                               \
    for (i = 0; i < (128 / TBITS); i++) {                                      \
        uint##BITS##_t a = s390_vec_read_element##BITS(v2, i * 2);             \
        uint##BITS##_t b = s390_vec_read_element##BITS(v3, i * 2);             \
        uint##TBITS##_t d = galois_multiply##BITS(a, b);                       \
                                                                               \
        a = s390_vec_read_element##BITS(v2, i * 2 + 1);                        \
        b = s390_vec_read_element##BITS(v3, i * 2 + 1);                        \
        d = d ^ galois_multiply32(a, b);                                       \
        s390_vec_write_element##TBITS(v1, i, d);                               \
    }                                                                          \
}
DEF_VGFM(8, 16)
DEF_VGFM(16, 32)
DEF_VGFM(32, 64)

void HELPER(gvec_vgfm64)(void *v1, const void *v2, const void *v3,
                         uint32_t desc)
{
    S390Vector tmp1, tmp2;
    uint64_t a, b;

    a = s390_vec_read_element64(v2, 0);
    b = s390_vec_read_element64(v3, 0);
    tmp1 = galois_multiply64(a, b);
    a = s390_vec_read_element64(v2, 1);
    b = s390_vec_read_element64(v3, 1);
    tmp2 = galois_multiply64(a, b);
    s390_vec_xor(v1, &tmp1, &tmp2);
}

#define DEF_VGFMA(BITS, TBITS)                                                 \
void HELPER(gvec_vgfma##BITS)(void *v1, const void *v2, const void *v3,        \
                              const void *v4, uint32_t desc)                   \
{                                                                              \
    int i;                                                                     \
                                                                               \
    for (i = 0; i < (128 / TBITS); i++) {                                      \
        uint##BITS##_t a = s390_vec_read_element##BITS(v2, i * 2);             \
        uint##BITS##_t b = s390_vec_read_element##BITS(v3, i * 2);             \
        uint##TBITS##_t d = galois_multiply##BITS(a, b);                       \
                                                                               \
        a = s390_vec_read_element##BITS(v2, i * 2 + 1);                        \
        b = s390_vec_read_element##BITS(v3, i * 2 + 1);                        \
        d = d ^ galois_multiply32(a, b);                                       \
        d = d ^ s390_vec_read_element##TBITS(v4, i);                           \
        s390_vec_write_element##TBITS(v1, i, d);                               \
    }                                                                          \
}
DEF_VGFMA(8, 16)
DEF_VGFMA(16, 32)
DEF_VGFMA(32, 64)

void HELPER(gvec_vgfma64)(void *v1, const void *v2, const void *v3,
                          const void *v4, uint32_t desc)
{
    S390Vector tmp1, tmp2;
    uint64_t a, b;

    a = s390_vec_read_element64(v2, 0);
    b = s390_vec_read_element64(v3, 0);
    tmp1 = galois_multiply64(a, b);
    a = s390_vec_read_element64(v2, 1);
    b = s390_vec_read_element64(v3, 1);
    tmp2 = galois_multiply64(a, b);
    s390_vec_xor(&tmp1, &tmp1, &tmp2);
    s390_vec_xor(v1, &tmp1, v4);
}

#define DEF_VLP(BITS)                                                          \
void HELPER(gvec_vlp##BITS)(void *v1, const void *v2, uint32_t desc)           \
{                                                                              \
    int i;                                                                     \
                                                                               \
    for (i = 0; i < (128 / BITS); i++) {                                       \
        const int##BITS##_t a = s390_vec_read_element##BITS(v2, i);            \
                                                                               \
        s390_vec_write_element##BITS(v1, i, a < 0 ? -a : a);                   \
    }                                                                          \
}
DEF_VLP(8)
DEF_VLP(16)
