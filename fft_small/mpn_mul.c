/*
    Copyright (C) 2022 Daniel Schultz

    This file is part of FLINT.

    FLINT is free software: you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.  See <https://www.gnu.org/licenses/>.
*/

#include "fft_small.h"
#include "machine_vectors.h"
#include "profiler.h"
#include<stdint.h>
#include<string.h>

#define USE_ORG

/* cmp(a, b*2^e), a does not have to be normalized */
int flint_mpn_cmp_ui_2exp(const ulong* a, ulong an, ulong b, ulong e)
{
    ulong q = e/FLINT_BITS;
    ulong r = e%FLINT_BITS;
    ulong x, b0, b1;

    while (an > 0 && a[an-1] == 0)
        an--;

    if (an == 0)
        return b != 0;

    // b*2^e = (b*2^r       )*2^(64*q)
    //       = (b0 + b1*2^64)*2^(64*q)
    if (r == 0)
    {
        b0 = b;
        b1 = 0;
    }
    else
    {
        b0 = b << r;
        b1 = b >> (FLINT_BITS - r);
    }

    //      check words [q+2,infty)
    // then check words [q+1, 64*q+128) against b1
    // then check words [q, q+1) against b0
    // then check words [0, q)

    if (an > q + 2)
        return 1;

    x = (q+1 < an) ? a[q+1] : 0;
    if (x != b1)
        return x > b1 ? 1 : -1;

    x = (q < an) ? a[q] : 0;
    if (x != b0)
        return x > b0 ? 1 : -1;

    q = n_min(q, an);
    while (q > 0)
    {
        q--;
        if (a[q] != 0)
            return 1;
    }

    return 0;
}


void crt_data_init(crt_data_t C, ulong prime, ulong coeff_len, ulong nprimes)
{
    C->prime = prime;
    C->coeff_len = coeff_len;
    C->nprimes = nprimes;
    C->data = FLINT_ARRAY_ALLOC(nprimes*coeff_len + coeff_len + nprimes, ulong);
}

void crt_data_clear(crt_data_t C)
{
    flint_free(C->data);
}

/* return mpn of length C->coeff_len */
FLINT_FORCE_INLINE ulong* crt_data_co_prime(const crt_data_t C, ulong i)
{
    FLINT_ASSERT(i < C->nprimes);
    return C->data + i*C->coeff_len;
}

FLINT_FORCE_INLINE ulong* _crt_data_co_prime(const crt_data_t C, ulong i, ulong n)
{
    FLINT_ASSERT(i < C->nprimes);
    FLINT_ASSERT(n == C->coeff_len);
    return C->data + i*n;
}

/* return mpn of length C->coeff_len */
FLINT_FORCE_INLINE ulong* crt_data_prod_primes(const crt_data_t C)
{
    return C->data + C->nprimes*C->coeff_len;
}

/* the reduction of co_prime mod the i^th prime */
FLINT_FORCE_INLINE ulong* crt_data_co_prime_red(const crt_data_t C, ulong i)
{
    FLINT_ASSERT(i < C->nprimes);
    return C->data + C->nprimes*C->coeff_len + C->coeff_len + i;
}

/*
 need  ceil(64*bn/bits) <= prod_primes/2^(2*bits)
 i.e.  (64*bn+bits-1)/bits <= prod_primes/2^(2*bits)
       64*bn <= bits*prod_primes/2^(2*bits) - (bits-1)
*/
ulong crt_data_find_bound(const crt_data_t C, ulong bits)
{
    ulong bound = 0;
    ulong q = (2*bits)/FLINT_BITS;
    ulong r = (2*bits)%FLINT_BITS;
    ulong i;
    ulong* x;
    TMP_INIT;

    TMP_START;
    x = TMP_ARRAY_ALLOC(C->coeff_len+1, ulong);

    x[C->coeff_len] = mpn_mul_1(x, crt_data_prod_primes(C), C->coeff_len, bits);

    if (q < C->coeff_len+1)
    {
        if (r > 0)
            mpn_rshift(x+q, x+q, C->coeff_len+1-q, r);

        if (!mpn_sub_1(x+q, x+q, C->coeff_len+1-q, bits-1))
        {
            mpn_rshift(x+q, x+q, C->coeff_len+1-q, 6);
            bound = (x+q)[0];
            for (i = q + 1; i < C->coeff_len+1; i++)
                if (x[i] != 0)
                    bound = -UWORD(1);
        }
    }

    TMP_END;
    return bound;    
}



FLINT_FORCE_INLINE vec4ui vec4d_convert_limited_vec4ui(vec4d a) {
    __m256d t = _mm256_set1_pd(0x0010000000000000);
    return _mm256_castpd_si256(_mm256_xor_pd(_mm256_add_pd(a, t), t));
}






FLINT_FORCE_INLINE void vec4ui_store_unaligned(ulong* z, vec4ui a) {
    _mm256_storeu_si256((__m256i*) z, a);
}

void vec4_print(vec4d x) {
flint_printf("{%f, %f, %f, %f}", x[0], x[1], x[2], x[3]);
}

#define aindex(i) (((i) < an) ? a[i] : (uint32_t)(0))

#define CODE(ir) \
{ \
    ulong k = ((i+ir)*bits)/32; \
    ulong j = ((  ir)*bits)%32; \
\
    vec4d ak = vec4d_set_d((double)(a[k] >> j)); \
    for (ulong l = 0; l < nvs; l++) \
        X[l] = ak; \
    k++; \
    j = 32 - j; \
    while (j + 32 <= bits) \
    { \
        ak = vec4d_set_d((double)(a[k])); \
        for (ulong l = 0; l < nvs; l++) \
            X[l] = vec4d_add(X[l], vec4d_mulmod2(ak, two_pow[j*nvs+l], P[l], PINV[l])); \
        k++; \
        j += 32; \
    } \
\
    if ((bits-j) != 0) \
    { \
        ak = vec4d_set_d((double)(a[k] << (32-(bits-j)))); \
        for (ulong l = 0; l < nvs; l++) \
            X[l] = vec4d_add(X[l], vec4d_mulmod2(ak, two_pow[(bits-32)*nvs+l], P[l], PINV[l])); \
    } \
\
    for (ulong l = 0; l < nvs; l++) \
        X[l] = vec4d_reduce_to_pm1n(X[l], P[l], PINV[l]); \
\
    for (ulong l = 0; l < np; l++) \
        sd_fft_ctx_set_index(d + l*dstride, i+ir, X[l/VEC_SZ][l%VEC_SZ]); \
}


/* The the l^th fft ctx Rffts[l] is expected to have data at d + l*dstride */
#define DEFINE_IT(NP, BITS) \
static void CAT3(mpn_to_ffts, NP, BITS)( \
    sd_fft_ctx_struct* Rffts, double* d, ulong dstride, \
    const ulong* a_, ulong an_, ulong atrunc, \
    const vec4d* two_pow, \
    ulong start_easy, ulong stop_easy, \
    ulong start_hard, ulong stop_hard) \
{ \
    ulong np = NP; \
    ulong bits = BITS; \
    ulong nvs = n_cdiv(np, VEC_SZ); \
 \
    FLINT_ASSERT(bits >= FLINT_BITS); \
 \
    const uint32_t* a = (const uint32_t*)(a_); \
    ulong an = 2*an_; \
 \
    vec4d X[nvs]; \
    vec4d P[nvs]; \
    vec4d PINV[nvs]; \
 \
    for (ulong l = 0; l < nvs; l++) \
    { \
        P[l]    = vec4d_set_d4(Rffts[4*l+0].p, Rffts[4*l+1].p, Rffts[4*l+2].p, Rffts[4*l+3].p); \
        PINV[l] = vec4d_set_d4(Rffts[4*l+0].pinv, Rffts[4*l+1].pinv, Rffts[4*l+2].pinv, Rffts[4*l+3].pinv); \
    } \
 \
    if ((bits % 8) == 0) \
    { \
        FLINT_ASSERT(start_easy % 4 == 0); \
        FLINT_ASSERT(stop_easy % 4 == 0); \
        for (ulong i = start_easy ; i < stop_easy; i += 4) \
        { \
            CODE(0);CODE(1);CODE(2);CODE(3); \
        } \
    } \
    else if ((bits % 4) == 0) \
    { \
        FLINT_ASSERT(start_easy % 8 == 0); \
        FLINT_ASSERT(stop_easy % 8 == 0); \
        for (ulong i = start_easy ; i < stop_easy; i += 8) \
        { \
            CODE(0);CODE(1);CODE(2);CODE(3); \
            CODE(4);CODE(5);CODE(6);CODE(7); \
        } \
    } \
    else if ((bits % 2) == 0) \
    { \
        FLINT_ASSERT(start_easy % 16 == 0); \
        FLINT_ASSERT(stop_easy % 16 == 0); \
        for (ulong i = start_easy ; i < stop_easy; i += 16) \
        { \
            CODE(0);CODE(1);CODE(2);CODE(3); \
            CODE(4);CODE(5);CODE(6);CODE(7); \
            CODE(8);CODE(9);CODE(10);CODE(11); \
            CODE(12);CODE(13);CODE(14);CODE(15); \
        } \
    } \
    else \
    { \
        FLINT_ASSERT(0); \
    } \
 \
    for (ulong i = start_hard; i < stop_hard; i++) \
    { \
        ulong k = (i*bits)/32; \
        ulong j = (i*bits)%32; \
 \
        vec4d ak = vec4d_set_d((double)(aindex(k) >> j)); \
        for (ulong l = 0; l < nvs; l++) \
            X[l] = ak; \
        k++; \
        j = 32 - j; \
        while (j + 32 <= bits) \
        { \
            ak = vec4d_set_d((double)(aindex(k))); \
            for (ulong l = 0; l < nvs; l++) \
                X[l] = vec4d_add(X[l], vec4d_mulmod2(ak, two_pow[j*nvs+l], P[l], PINV[l])); \
            k++; \
            j += 32; \
        } \
 \
        if ((bits-j) != 0) \
        { \
            ak = vec4d_set_d((double)(aindex(k) << (32-(bits-j)))); \
            for (ulong l = 0; l < nvs; l++) \
                X[l] = vec4d_add(X[l], vec4d_mulmod2(ak, two_pow[(bits-32)*nvs+l], P[l], PINV[l])); \
        } \
 \
        for (ulong l = 0; l < nvs; l++) \
            X[l] = vec4d_reduce_to_pm1n(X[l], P[l], PINV[l]); \
 \
        for (ulong l = 0; l < np; l++) \
            sd_fft_ctx_set_index(d + l*dstride, i, X[l/VEC_SZ][l%VEC_SZ]); \
    } \
 \
    for (ulong l = 0; l < np; l++) \
        for (ulong i = stop_hard; i < atrunc; i++) \
            sd_fft_ctx_set_index(d + l*dstride, i, 0.0); \
}

#ifdef USE_ORG
DEFINE_IT(4, 64)
DEFINE_IT(3, 64)
DEFINE_IT(3, 66)
DEFINE_IT(3, 68)
DEFINE_IT(4, 78)
DEFINE_IT(4, 80)
DEFINE_IT(4, 82)
DEFINE_IT(4, 84)
DEFINE_IT(4, 86)
DEFINE_IT(4, 88)
DEFINE_IT(4, 90)
DEFINE_IT(4, 92)
DEFINE_IT(5,106)
DEFINE_IT(5,110)
DEFINE_IT(5,112)
DEFINE_IT(5,114)
DEFINE_IT(5,116)
DEFINE_IT(6,132)
DEFINE_IT(6,134)
DEFINE_IT(6,136)
DEFINE_IT(6,138)
DEFINE_IT(6,140)
DEFINE_IT(6,142)
DEFINE_IT(7,156)
DEFINE_IT(7,160)
DEFINE_IT(7,162)
DEFINE_IT(7,164)
DEFINE_IT(7,166)
DEFINE_IT(8,176)
DEFINE_IT(8,178)
DEFINE_IT(8,180)
DEFINE_IT(8,182)
DEFINE_IT(8,184)
DEFINE_IT(8,186)
DEFINE_IT(8,188)
#endif
#undef DEFINE_IT
#undef CODE
#undef aindex


FLINT_FORCE_INLINE ulong _addcarry_ulong(unsigned char cf, ulong x, ulong y, ulong* s)
{
    long long unsigned int _s;
    cf = _addcarry_u64(cf, (long long unsigned int)(x),
                           (long long unsigned int)(y),
                           &_s);
    *s = (ulong)(_s);
    return cf;
}

FLINT_FORCE_INLINE ulong _subborrow_ulong(unsigned char cf, ulong x, ulong y, ulong* s)
{
    long long unsigned int _s;
    cf = _subborrow_u64(cf, (long long unsigned int)(x),
                            (long long unsigned int)(y),
                           &_s);
    *s = (ulong)(_s);
    return cf;
}

/* seems all version of gcc generate worse code if the intrinsics are used */
#if 1
#define add_sssssaaaaaaaaaa(s4,s3,s2,s1,s0, a4,a3,a2,a1,a0, b4,b3,b2,b1,b0)  \
  __asm__ ("addq %14,%q4\n\tadcq %12,%q3\n\tadcq %10,%q2\n\tadcq %8,%q1\n\tadcq %6,%q0"    \
       : "=r" (s4), "=&r" (s3), "=&r" (s2), "=&r" (s1), "=&r" (s0)                    \
       : "0"  ((mp_limb_t)(a4)), "rme" ((mp_limb_t)(b4)),                 \
         "1"  ((mp_limb_t)(a3)), "rme" ((mp_limb_t)(b3)),                 \
         "2"  ((mp_limb_t)(a2)), "rme" ((mp_limb_t)(b2)),                 \
         "3"  ((mp_limb_t)(a1)), "rme" ((mp_limb_t)(b1)),                 \
         "4"  ((mp_limb_t)(a0)), "rme" ((mp_limb_t)(b0)))

#define add_ssssssaaaaaaaaaaaa(s5,s4,s3,s2,s1,s0, a5,a4,a3,a2,a1,a0, b5,b4,b3,b2,b1,b0)  \
  __asm__ ("addq %17,%q5\nadcq %15,%q4\n\tadcq %13,%q3\n\tadcq %11,%q2\n\tadcq %9,%q1\n\tadcq %7,%q0"    \
       : "=r" (s5), "=&r" (s4), "=&r" (s3), "=&r" (s2), "=&r" (s1), "=&r" (s0)                    \
       : "0"  ((mp_limb_t)(a5)), "rme" ((mp_limb_t)(b5)),                 \
         "1"  ((mp_limb_t)(a4)), "rme" ((mp_limb_t)(b4)),                 \
         "2"  ((mp_limb_t)(a3)), "rme" ((mp_limb_t)(b3)),                 \
         "3"  ((mp_limb_t)(a2)), "rme" ((mp_limb_t)(b2)),                 \
         "4"  ((mp_limb_t)(a1)), "rme" ((mp_limb_t)(b1)),                 \
         "5"  ((mp_limb_t)(a0)), "rme" ((mp_limb_t)(b0)))

#define add_sssssssaaaaaaaaaaaaaa(s6,s5,s4,s3,s2,s1,s0, a6,a5,a4,a3,a2,a1,a0, b6,b5,b4,b3,b2,b1,b0)  \
  __asm__ ("addq %20,%q6\nadcq %18,%q5\nadcq %16,%q4\n\tadcq %14,%q3\n\tadcq %12,%q2\n\tadcq %10,%q1\n\tadcq %8,%q0"    \
       : "=r" (s6), "=&r" (s5), "=&r" (s4), "=&r" (s3), "=&r" (s2), "=&r" (s1), "=&r" (s0)                    \
       : "0"  ((mp_limb_t)(a6)), "rme" ((mp_limb_t)(b6)),                 \
         "1"  ((mp_limb_t)(a5)), "rme" ((mp_limb_t)(b5)),                 \
         "2"  ((mp_limb_t)(a4)), "rme" ((mp_limb_t)(b4)),                 \
         "3"  ((mp_limb_t)(a3)), "rme" ((mp_limb_t)(b3)),                 \
         "4"  ((mp_limb_t)(a2)), "rme" ((mp_limb_t)(b2)),                 \
         "5"  ((mp_limb_t)(a1)), "rme" ((mp_limb_t)(b1)),                 \
         "6"  ((mp_limb_t)(a0)), "rme" ((mp_limb_t)(b0)))

#define add_ssssssssaaaaaaaaaaaaaaaa(s7,s6,s5,s4,s3,s2,s1,s0, a7,a6,a5,a4,a3,a2,a1,a0, b7,b6,b5,b4,b3,b2,b1,b0)  \
  __asm__ ("addq %23,%q7\nadcq %21,%q6\nadcq %19,%q5\n\tadcq %17,%q4\n\tadcq %15,%q3\n\tadcq %13,%q2\n\tadcq %11,%q1\n\tadcq %9,%q0"    \
       : "=r" (s7), "=&r" (s6), "=&r" (s5), "=&r" (s4), "=&r" (s3), "=&r" (s2), "=&r" (s1), "=&r" (s0)                    \
       : "0"  ((mp_limb_t)(a7)), "rme" ((mp_limb_t)(b7)),                 \
         "1"  ((mp_limb_t)(a6)), "rme" ((mp_limb_t)(b6)),                 \
         "2"  ((mp_limb_t)(a5)), "rme" ((mp_limb_t)(b5)),                 \
         "3"  ((mp_limb_t)(a4)), "rme" ((mp_limb_t)(b4)),                 \
         "4"  ((mp_limb_t)(a3)), "rme" ((mp_limb_t)(b3)),                 \
         "5"  ((mp_limb_t)(a2)), "rme" ((mp_limb_t)(b2)),                 \
         "6"  ((mp_limb_t)(a1)), "rme" ((mp_limb_t)(b1)),                 \
         "7"  ((mp_limb_t)(a0)), "rme" ((mp_limb_t)(b0)))


#define sub_ddddmmmmssss(s3, s2, s1, s0, a3, a2, a1, a0, b3, b2, b1, b0)  \
  __asm__ ("subq %11,%q3\n\tsbbq %9,%q2\n\tsbbq %7,%q1\n\tsbbq %5,%q0"    \
       : "=r" (s3), "=&r" (s2), "=&r" (s1), "=&r" (s0)                    \
       : "0"  ((mp_limb_t)(a3)), "rme" ((mp_limb_t)(b3)),                 \
         "1"  ((mp_limb_t)(a2)), "rme" ((mp_limb_t)(b2)),                 \
         "2"  ((mp_limb_t)(a1)), "rme" ((mp_limb_t)(b1)),                 \
         "3"  ((mp_limb_t)(a0)), "rme" ((mp_limb_t)(b0)))

#define sub_dddddmmmmmsssss(s4,s3,s2,s1,s0, a4,a3,a2,a1,a0, b4,b3,b2,b1,b0)  \
  __asm__ ("subq %14,%q4\n\tsbbq %12,%q3\n\tsbbq %10,%q2\n\tsbbq %8,%q1\n\tsbbq %6,%q0"    \
       : "=r" (s4), "=&r" (s3), "=&r" (s2), "=&r" (s1), "=&r" (s0)                    \
       : "0"  ((mp_limb_t)(a4)), "rme" ((mp_limb_t)(b4)),                 \
         "1"  ((mp_limb_t)(a3)), "rme" ((mp_limb_t)(b3)),                 \
         "2"  ((mp_limb_t)(a2)), "rme" ((mp_limb_t)(b2)),                 \
         "3"  ((mp_limb_t)(a1)), "rme" ((mp_limb_t)(b1)),                 \
         "4"  ((mp_limb_t)(a0)), "rme" ((mp_limb_t)(b0)))

#define sub_ddddddmmmmmmssssss(s5,s4,s3,s2,s1,s0, a5,a4,a3,a2,a1,a0, b5,b4,b3,b2,b1,b0)  \
  __asm__ ("subq %17,%q5\nsbbq %15,%q4\n\tsbbq %13,%q3\n\tsbbq %11,%q2\n\tsbbq %9,%q1\n\tsbbq %7,%q0"    \
       : "=r" (s5), "=&r" (s4), "=&r" (s3), "=&r" (s2), "=&r" (s1), "=&r" (s0)                    \
       : "0"  ((mp_limb_t)(a5)), "rme" ((mp_limb_t)(b5)),                 \
         "1"  ((mp_limb_t)(a4)), "rme" ((mp_limb_t)(b4)),                 \
         "2"  ((mp_limb_t)(a3)), "rme" ((mp_limb_t)(b3)),                 \
         "3"  ((mp_limb_t)(a2)), "rme" ((mp_limb_t)(b2)),                 \
         "4"  ((mp_limb_t)(a1)), "rme" ((mp_limb_t)(b1)),                 \
         "5"  ((mp_limb_t)(a0)), "rme" ((mp_limb_t)(b0)))

#define sub_dddddddmmmmmmmsssssss(s6,s5,s4,s3,s2,s1,s0, a6,a5,a4,a3,a2,a1,a0, b6,b5,b4,b3,b2,b1,b0)  \
  __asm__ ("subq %20,%q6\nsbbq %18,%q5\nsbbq %16,%q4\n\tsbbq %14,%q3\n\tsbbq %12,%q2\n\tsbbq %10,%q1\n\tsbbq %8,%q0"    \
       : "=r" (s6), "=&r" (s5), "=&r" (s4), "=&r" (s3), "=&r" (s2), "=&r" (s1), "=&r" (s0)                    \
       : "0"  ((mp_limb_t)(a6)), "rme" ((mp_limb_t)(b6)),                 \
         "1"  ((mp_limb_t)(a5)), "rme" ((mp_limb_t)(b5)),                 \
         "2"  ((mp_limb_t)(a4)), "rme" ((mp_limb_t)(b4)),                 \
         "3"  ((mp_limb_t)(a3)), "rme" ((mp_limb_t)(b3)),                 \
         "4"  ((mp_limb_t)(a2)), "rme" ((mp_limb_t)(b2)),                 \
         "5"  ((mp_limb_t)(a1)), "rme" ((mp_limb_t)(b1)),                 \
         "6"  ((mp_limb_t)(a0)), "rme" ((mp_limb_t)(b0)))

#define sub_ddddddddmmmmmmmmssssssss(s7,s6,s5,s4,s3,s2,s1,s0, a7,a6,a5,a4,a3,a2,a1,a0, b7,b6,b5,b4,b3,b2,b1,b0)  \
  __asm__ ("subq %23,%q7\nsbbq %21,%q6\nsbbq %19,%q5\n\tsbbq %17,%q4\n\tsbbq %15,%q3\n\tsbbq %13,%q2\n\tsbbq %11,%q1\n\tsbbq %9,%q0"    \
       : "=r" (s7), "=&r" (s6), "=&r" (s5), "=&r" (s4), "=&r" (s3), "=&r" (s2), "=&r" (s1), "=&r" (s0)                    \
       : "0"  ((mp_limb_t)(a7)), "rme" ((mp_limb_t)(b7)),                 \
         "1"  ((mp_limb_t)(a6)), "rme" ((mp_limb_t)(b6)),                 \
         "2"  ((mp_limb_t)(a5)), "rme" ((mp_limb_t)(b5)),                 \
         "3"  ((mp_limb_t)(a4)), "rme" ((mp_limb_t)(b4)),                 \
         "4"  ((mp_limb_t)(a3)), "rme" ((mp_limb_t)(b3)),                 \
         "5"  ((mp_limb_t)(a2)), "rme" ((mp_limb_t)(b2)),                 \
         "6"  ((mp_limb_t)(a1)), "rme" ((mp_limb_t)(b1)),                 \
         "7"  ((mp_limb_t)(a0)), "rme" ((mp_limb_t)(b0)))

FLINT_FORCE_INLINE void multi_add_2(ulong z[], const ulong a[])
{
    add_ssaaaa(z[1],z[0],
               z[1],z[0],
               a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_add_3(ulong z[], const ulong a[])
{
    add_sssaaaaaa(z[2],z[1],z[0],
                  z[2],z[1],z[0],
                  a[2],a[1],a[0]);
}


FLINT_FORCE_INLINE void multi_add_4(ulong z[], const ulong a[])
{
    add_ssssaaaaaaaa(z[3],z[2],z[1],z[0],
                     z[3],z[2],z[1],z[0],
                     a[3],a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_add_5(ulong z[], const ulong a[])
{
    add_sssssaaaaaaaaaa(z[4],z[3],z[2],z[1],z[0],
                        z[4],z[3],z[2],z[1],z[0],
                        a[4],a[3],a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_add_6(ulong z[], const ulong a[])
{
    add_ssssssaaaaaaaaaaaa(z[5],z[4],z[3],z[2],z[1],z[0],
                           z[5],z[4],z[3],z[2],z[1],z[0],
                           a[5],a[4],a[3],a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_add_7(ulong z[], const ulong a[])
{
    add_sssssssaaaaaaaaaaaaaa(z[6],z[5],z[4],z[3],z[2],z[1],z[0],
                              z[6],z[5],z[4],z[3],z[2],z[1],z[0],
                              a[6],a[5],a[4],a[3],a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_add_8(ulong z[], const ulong a[])
{
    add_ssssssssaaaaaaaaaaaaaaaa(z[7],z[6],z[5],z[4],z[3],z[2],z[1],z[0],
                                 z[7],z[6],z[5],z[4],z[3],z[2],z[1],z[0],
                                 a[7],a[6],a[5],a[4],a[3],a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_sub_2(ulong z[], const ulong a[])
{
    sub_ddmmss(z[1],z[0],
               z[1],z[0],
               a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_sub_3(ulong z[], const ulong a[])
{
    sub_dddmmmsss(z[2],z[1],z[0],
                  z[2],z[1],z[0],
                  a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_sub_4(ulong z[], const ulong a[])
{
    sub_ddddmmmmssss(z[3],z[2],z[1],z[0],
                     z[3],z[2],z[1],z[0],
                     a[3],a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_sub_5(ulong z[], const ulong a[])
{
    sub_dddddmmmmmsssss(z[4],z[3],z[2],z[1],z[0],
                        z[4],z[3],z[2],z[1],z[0],
                        a[4],a[3],a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_sub_6(ulong z[], const ulong a[])
{
    sub_ddddddmmmmmmssssss(z[5],z[4],z[3],z[2],z[1],z[0],
                           z[5],z[4],z[3],z[2],z[1],z[0],
                           a[5],a[4],a[3],a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_sub_7(ulong z[], const ulong a[])
{
    sub_dddddddmmmmmmmsssssss(z[6],z[5],z[4],z[3],z[2],z[1],z[0],
                              z[6],z[5],z[4],z[3],z[2],z[1],z[0],
                              a[6],a[5],a[4],a[3],a[2],a[1],a[0]);
}

FLINT_FORCE_INLINE void multi_sub_8(ulong z[], const ulong a[])
{
    sub_ddddddddmmmmmmmmssssssss(z[7],z[6],z[5],z[4],z[3],z[2],z[1],z[0],
                                 z[7],z[6],z[5],z[4],z[3],z[2],z[1],z[0],
                                 a[7],a[6],a[5],a[4],a[3],a[2],a[1],a[0]);
}

#else


#define DEFINE_IT(n) \
FLINT_FORCE_INLINE void CAT(multi_sub, n)(ulong z[], const ulong a[]) \
{ \
    unsigned char cf = 0; \
    for (ulong i = 0; i < n; i++) \
        cf = _subborrow_ulong(cf, z[i], a[i], &z[i]); \
} \
FLINT_FORCE_INLINE void CAT(multi_add, n)(ulong z[], const ulong a[]) \
{ \
    unsigned char cf = 0; \
    for (ulong i = 0; i < n; i++) \
        cf = _addcarry_ulong(cf, z[i], a[i], &z[i]); \
}

DEFINE_IT(2)
DEFINE_IT(3)
DEFINE_IT(4)
DEFINE_IT(5)
DEFINE_IT(6)
DEFINE_IT(7)
DEFINE_IT(8)
#undef DEFINE_IT

#endif


FLINT_FORCE_INLINE void _mul(ulong* hi, ulong* lo, ulong y, ulong x)
{
    __uint128_t p = ((__uint128_t) x) * ((__uint128_t) y);
    *lo = (ulong) (p);
    *hi = (ulong) (p >> 64);
}

FLINT_FORCE_INLINE void _madd(ulong* hi, ulong* lo, ulong y, ulong x)
{
    __uint128_t p = ((__uint128_t) *lo) | (((__uint128_t) *hi) << 64);
    p += ((__uint128_t) x) * ((__uint128_t) y);
    *lo = (ulong) (p);
    *hi = (ulong) (p >> 64);
}

#define DEFINE_IT(n, m) \
FLINT_FORCE_INLINE void CAT3(_big_mul, n, m)(ulong r[], ulong t[], ulong C[], ulong y) \
{ \
    for (ulong k = 0; k < n; k += 2) \
    { \
        if (k + 1 < n) \
        { \
            FLINT_ASSERT(k < m); \
            _mul(&r[k+1],&r[k+0], C[k+0], y); \
        } \
        else \
        { \
            FLINT_ASSERT(k + 1 == n); \
            if (k < m) \
                r[k+0] = C[k+0]*y; \
            else \
                r[k+0] = 0; \
        } \
 \
        if (k + 2 < n) \
        { \
            FLINT_ASSERT(k + 1 < m); \
            _mul(&t[k+2],&t[k+1], C[k+1], y); \
        } \
        else if (k + 1 < n) \
        { \
            if (k + 1 < m) \
                t[k+1] = C[k+1]*y; \
            else \
                t[k+1] = 0; \
        } \
    } \
} \
FLINT_FORCE_INLINE void CAT3(_big_addmul, n, m)(ulong r[], ulong t[], ulong C[], ulong y) \
{ \
    for (ulong k = 0; k < n; k += 2) \
    { \
        if (k + 1 < n) \
        { \
            FLINT_ASSERT(k < m); \
            _madd(&r[k+1],&r[k+0], C[k+0], y); \
        } \
        else \
        { \
            FLINT_ASSERT(k + 1 == n); \
            if (k < m) \
                r[k+0] += C[k+0]*y; \
        } \
 \
        if (k + 2 < n) \
        { \
            FLINT_ASSERT(k + 1 < m); \
            _madd(&t[k+2],&t[k+1], C[k+1], y); \
        } \
        else if (k + 1 < n) \
        { \
            if (k + 1 < m) \
                t[k+1] += C[k+1]*y; \
        } \
    } \
}

DEFINE_IT(3, 2)
DEFINE_IT(4, 3)
DEFINE_IT(4, 4)
DEFINE_IT(5, 4)
DEFINE_IT(6, 5)
DEFINE_IT(7, 6)
#undef DEFINE_IT



#define DEFINE_IT(n, n_minus_1) \
FLINT_FORCE_INLINE void CAT(_reduce_big_sum, n)(ulong r[], ulong t[], const ulong* limit) \
{ \
    CAT(multi_add, n_minus_1)(r+1, t+1); \
check: \
    for (ulong k = n; k > 1; k--) \
    { \
        if (LIKELY(r[k-1] > limit[k-1])) \
            goto sub; \
        if (r[k-1] < limit[k-1]) \
            return; \
    } \
    if (r[0] < limit[0]) \
        return; \
sub: \
    CAT(multi_sub, n)(r, limit); \
    goto check; \
}

DEFINE_IT(3, 2)
DEFINE_IT(4, 3)
DEFINE_IT(5, 4)
DEFINE_IT(6, 5)
DEFINE_IT(7, 6)
DEFINE_IT(8, 7)
#undef DEFINE_IT


#define DEFINE_IT(n, n_plus_1) \
FLINT_FORCE_INLINE void CAT(_add_to_answer_easy, n)(ulong z[], ulong r[], ulong zn, ulong toff, ulong tshift) \
{ \
    FLINT_ASSERT(zn > toff); \
    if (tshift == 0) \
    { \
        CAT(multi_add, n)(z + toff, r); \
    } \
    else \
    { \
        r[n] = r[n-1] >> (64-tshift); \
        for (ulong k = n; k >= 2; k--) \
            r[k-1] = (r[k-1] << (tshift)) | (r[k-2] >> (64-tshift)); \
        r[0] =  r[0] << (tshift); \
        CAT(multi_add, n_plus_1)(z + toff, r); \
    } \
} \
FLINT_FORCE_INLINE void CAT(_add_to_answer_hard, n)(ulong z[], ulong r[], ulong zn, ulong toff, ulong tshift) \
{ \
    FLINT_ASSERT(zn > toff); \
    if (tshift == 0) \
    { \
        if (zn - toff >= n) \
        { \
            CAT(multi_add, n)(z + toff, r); \
            return; \
        } \
    } \
    else \
    { \
        r[n] = r[n-1] >> (64-tshift); \
        for (ulong k = n; k >= 2; k--) \
            r[k-1] = (r[k-1] << (tshift)) | (r[k-2] >> (64-tshift)); \
        r[0] =  r[0] << (tshift); \
        if (zn - toff > n) \
        { \
            CAT(multi_add, n_plus_1)(z + toff, r); \
            return; \
        } \
    } \
    FLINT_ASSERT(zn - toff <= n); \
    mpn_add_n(z + toff, z + toff, r, zn - toff); \
}

DEFINE_IT(3, 4)
DEFINE_IT(4, 5)
DEFINE_IT(5, 6)
DEFINE_IT(6, 7)
DEFINE_IT(7, 8)
#undef DEFINE_IT

/* transpose a block */
static void _convert_block(
    ulong* Xs,
    sd_fft_ctx_struct* Rffts, double* d, ulong dstride,
    ulong np,
    ulong I)
{
    for (ulong l = 0; l < np; l++)
    {
        vec4d p = vec4d_set_d(Rffts[l].p);
        vec4d pinv = vec4d_set_d(Rffts[l].pinv);
        double* x = sd_fft_ctx_blk_index(d + l*dstride, I);
        ulong j = 0; do {
            vec4d x0, x1, x2, x3;
            vec4ui y0, y1, y2, y3;
            x0 = vec4d_load(x + j + 0*VEC_SZ);
            x1 = vec4d_load(x + j + 1*VEC_SZ);
            x2 = vec4d_load(x + j + 2*VEC_SZ);
            x3 = vec4d_load(x + j + 3*VEC_SZ);
            x0 = vec4d_reduce_to_0n(x0, p, pinv);
            x1 = vec4d_reduce_to_0n(x1, p, pinv);
            x2 = vec4d_reduce_to_0n(x2, p, pinv);
            x3 = vec4d_reduce_to_0n(x3, p, pinv);
            y0 = vec4d_convert_limited_vec4ui(x0);
            y1 = vec4d_convert_limited_vec4ui(x1);
            y2 = vec4d_convert_limited_vec4ui(x2);
            y3 = vec4d_convert_limited_vec4ui(x3);
            vec4ui_store_unaligned(Xs + l*BLK_SZ + j + 0*VEC_SZ, y0);
            vec4ui_store_unaligned(Xs + l*BLK_SZ + j + 1*VEC_SZ, y1);
            vec4ui_store_unaligned(Xs + l*BLK_SZ + j + 2*VEC_SZ, y2);
            vec4ui_store_unaligned(Xs + l*BLK_SZ + j + 3*VEC_SZ, y3);
        } while (j += 4*VEC_SZ, j < BLK_SZ);
        FLINT_ASSERT(j == BLK_SZ);
    }
}

/*
    The "n" here is the limb count Rcrts[np-1].coeff_len, which is big enough
    to hold (product of primes)*(number of primes), so it can hold the
    intermediate dot products f[0]*x[0] + ... + f[np-1]*x[np-1]. The x[i] are
    single limb and the f[i] are of length "m". The number of primes is "np".

    The coefficient of X^i, 0 <= i < zlen needs to be reconstructed and added
    to the answer mpn (z, zn). This involves the limbs

       z[floor(i*bits/64)] ... z[floor(i*bits/64)+n]

    so is easy if floor(i*bits/64)+n < zn.

    The the l^th fft ctx Rffts[l] is expected to have data at d + l*dstride
*/
#define DEFINE_IT(NP, N, M) \
static void CAT4(mpn_from_ffts, NP, N, M)( \
    ulong* z, ulong zn, ulong zlen, \
    sd_fft_ctx_struct* Rffts, double* d, ulong dstride, \
    crt_data_struct* Rcrts, \
    ulong bits) \
{ \
    ulong np = NP; \
    ulong n = N; \
    ulong m = M; \
 \
    FLINT_ASSERT(n == Rcrts[np-1].coeff_len); \
 \
    if (n == m + 1) \
    { \
        for (ulong l = 0; l < np; l++) { \
            FLINT_ASSERT(crt_data_co_prime(Rcrts + np - 1, l)[m] == 0); \
        } \
    } \
    else \
    { \
        FLINT_ASSERT(n == m); \
    } \
 \
    memset(z, 0, zn*sizeof(ulong)); \
 \
    ulong i = 0; \
 \
    ulong end_easy = (zn >= n+1 ? zn - (n+1) : (ulong)(0))*FLINT_BITS/bits; \
 \
    ulong Xs[BLK_SZ*NP]; \
 \
    end_easy &= -BLK_SZ; \
 \
    for (; i < end_easy; i += BLK_SZ) \
    { \
        _convert_block(Xs, Rffts, d, dstride, np, i/BLK_SZ); \
 \
        for (ulong j = 0; j < BLK_SZ; j += 1) \
        { \
            ulong r[N + 1]; \
            ulong t[N + 1]; \
            ulong l = 0; \
 \
            CAT3(_big_mul, N, M)(r, t, _crt_data_co_prime(Rcrts + np - 1, l, n), Xs[l*BLK_SZ + j]); \
            for (l++; l < np; l++) \
                CAT3(_big_addmul, N, M)(r, t, _crt_data_co_prime(Rcrts + np - 1, l, n), Xs[l*BLK_SZ + j]); \
 \
            CAT(_reduce_big_sum, N)(r, t, crt_data_prod_primes(Rcrts + np - 1)); \
 \
            ulong toff = ((i+j)*bits)/FLINT_BITS; \
            ulong tshift = ((i+j)*bits)%FLINT_BITS; \
 \
            FLINT_ASSERT(zn > n + toff); \
 \
            CAT(_add_to_answer_easy, N)(z, r, zn, toff, tshift); \
        } \
    } \
 \
    for (; i < zlen; i++) \
    { \
        ulong r[N + 1]; \
        ulong t[N + 1]; \
        ulong l = 0; \
        double xx = sd_fft_ctx_get_index(d + l*dstride, i); \
        ulong x = vec1d_reduce_to_0n(xx, Rffts[l].p, Rffts[l].pinv); \
 \
        CAT3(_big_mul, N, M)(r, t, crt_data_co_prime(Rcrts + np - 1, l), x); \
        for (l++; l < np; l++) \
        { \
            xx = sd_fft_ctx_get_index(d + l*dstride, i); \
            x = vec1d_reduce_to_0n(xx, Rffts[l].p, Rffts[l].pinv); \
            CAT3(_big_addmul, N, M)(r, t, crt_data_co_prime(Rcrts + np - 1, l), x); \
        } \
 \
        CAT(_reduce_big_sum, N)(r, t, crt_data_prod_primes(Rcrts + np - 1)); \
 \
        ulong toff = (i*bits)/FLINT_BITS; \
        ulong tshift = (i*bits)%FLINT_BITS; \
 \
        if (toff >= zn) \
            break; \
 \
        CAT(_add_to_answer_hard, N)(z, r, zn, toff, tshift); \
    } \
}
/*
DEFINE_IT(3, 3, 2)
DEFINE_IT(4, 4, 3)
DEFINE_IT(5, 4, 4)
DEFINE_IT(6, 5, 4)
DEFINE_IT(7, 6, 5)
DEFINE_IT(8, 7, 6)
*/
#undef DEFINE_IT


/*
    if overhang = NULL

        handle output coefficients from [start_easy, zlen)

    if overhang != NULL

        overhang has space for n words

        handle output coefficients from [start_easy, end_easy) where
        start_easy and stop_easy are divisible by BLK_SZ

        write to output words
        [start_easy*bits/64, stop_easy*bits/64) [overhang+0, overhang+n)
*/
#define DEFINE_IT(NP, N, M) \
static void CAT4(new_mpn_from_ffts, NP, N, M)( \
    ulong* z, ulong zn, ulong zlen, \
    sd_fft_ctx_struct* Rffts, double* d, ulong dstride, \
    crt_data_struct* Rcrts, \
    ulong bits, \
    ulong start_easy, ulong stop_easy, \
    ulong* overhang) \
{ \
    ulong np = NP; \
    ulong n = N; \
    ulong m = M; \
    ulong zn_start = start_easy*bits/64; \
    ulong zn_stop  = (overhang == NULL) ? zn : stop_easy*bits/64; \
 \
    FLINT_ASSERT(n == Rcrts[np-1].coeff_len); \
    FLINT_ASSERT(start_easy <= end_easy); \
 \
    if (n == m + 1) \
    { \
        for (ulong l = 0; l < np; l++) { \
            FLINT_ASSERT(crt_data_co_prime(Rcrts + np - 1, l)[m] == 0); \
        } \
    } \
    else \
    { \
        FLINT_ASSERT(n == m); \
    } \
 \
    memset(z + zn_start, 0, (zn_stop - zn_start)*sizeof(ulong)); \
 \
    ulong Xs[BLK_SZ*NP]; \
 \
    if (overhang != NULL) \
    { \
        for (ulong i = 0; i < n; i++) \
            overhang[i] = 0; \
        if (start_easy >= stop_easy) \
            return; \
        stop_easy -= BLK_SZ; \
    } \
 \
    for (ulong i = start_easy; i < stop_easy; i += BLK_SZ) \
    { \
        _convert_block(Xs, Rffts, d, dstride, np, i/BLK_SZ); \
 \
        for (ulong j = 0; j < BLK_SZ; j += 1) \
        { \
            ulong r[N + 1]; \
            ulong t[N + 1]; \
            ulong l = 0; \
 \
            CAT3(_big_mul, N, M)(r, t, _crt_data_co_prime(Rcrts + np - 1, l, n), Xs[l*BLK_SZ + j]); \
            for (l++; l < np; l++) \
                CAT3(_big_addmul, N, M)(r, t, _crt_data_co_prime(Rcrts + np - 1, l, n), Xs[l*BLK_SZ + j]); \
 \
            CAT(_reduce_big_sum, N)(r, t, crt_data_prod_primes(Rcrts + np - 1)); \
 \
            ulong toff = ((i+j)*bits)/FLINT_BITS; \
            ulong tshift = ((i+j)*bits)%FLINT_BITS; \
 \
            FLINT_ASSERT(zn_stop > n + toff); \
 \
            CAT(_add_to_answer_easy, N)(z, r, zn_stop, toff, tshift); \
        } \
    } \
 \
    if (overhang != NULL) \
    { \
        ulong i = stop_easy; \
        _convert_block(Xs, Rffts, d, dstride, np, i/BLK_SZ); \
 \
        for (ulong j = 0; j < BLK_SZ; j += 1) \
        { \
            ulong r[N + 1]; \
            ulong t[N + 1]; \
            ulong l = 0; \
 \
            CAT3(_big_mul, N, M)(r, t, _crt_data_co_prime(Rcrts + np - 1, l, n), Xs[l*BLK_SZ + j]); \
            for (l++; l < np; l++) \
                CAT3(_big_addmul, N, M)(r, t, _crt_data_co_prime(Rcrts + np - 1, l, n), Xs[l*BLK_SZ + j]); \
 \
            CAT(_reduce_big_sum, N)(r, t, crt_data_prod_primes(Rcrts + np - 1)); \
 \
            ulong toff = ((i+j)*bits)/FLINT_BITS; \
            ulong tshift = ((i+j)*bits)%FLINT_BITS; \
 \
            if (n + toff < zn_stop) \
            { \
                CAT(_add_to_answer_easy, N)(z, r, zn_stop, toff, tshift); \
            } \
            else \
            { \
                if (tshift == 0) \
                { \
                    r[n] = 0; \
                } \
                else \
                { \
                    r[n] = r[n-1] >> (64-tshift); \
                    for (ulong k = n; k >= 2; k--) \
                        r[k-1] = (r[k-1] << (tshift)) | (r[k-2] >> (64-tshift)); \
                    r[0] =  r[0] << (tshift); \
                } \
                /* add zn_stop - toff words to the answer */ \
                /* and n + 1 + toff - zn_stop words to the overhang */ \
                unsigned char cf = 0; \
                ulong k = 0; \
                for (; k < zn_stop - toff; k++) \
                    cf = _addcarry_ulong(cf, z[toff + k], r[k], &z[toff + k]); \
                for (; k <= n; k++) \
                    cf = _addcarry_ulong(cf, overhang[k-(zn_stop-toff)], r[k], &overhang[k-(zn_stop-toff)]); \
            } \
        } \
    } \
    else \
    { \
        for (ulong i = stop_easy; i < zlen; i++) \
        { \
            ulong r[N + 1]; \
            ulong t[N + 1]; \
            ulong l = 0; \
            double xx = sd_fft_ctx_get_index(d + l*dstride, i); \
            ulong x = vec1d_reduce_to_0n(xx, Rffts[l].p, Rffts[l].pinv); \
 \
            CAT3(_big_mul, N, M)(r, t, crt_data_co_prime(Rcrts + np - 1, l), x); \
            for (l++; l < np; l++) \
            { \
                xx = sd_fft_ctx_get_index(d + l*dstride, i); \
                x = vec1d_reduce_to_0n(xx, Rffts[l].p, Rffts[l].pinv); \
                CAT3(_big_addmul, N, M)(r, t, crt_data_co_prime(Rcrts + np - 1, l), x); \
            } \
 \
            CAT(_reduce_big_sum, N)(r, t, crt_data_prod_primes(Rcrts + np - 1)); \
 \
            ulong toff = (i*bits)/FLINT_BITS; \
            ulong tshift = (i*bits)%FLINT_BITS; \
 \
            if (toff >= zn) \
                break; \
 \
            CAT(_add_to_answer_hard, N)(z, r, zn, toff, tshift); \
        } \
    } \
}

DEFINE_IT(3, 3, 2)
DEFINE_IT(4, 4, 3)
DEFINE_IT(5, 4, 4)
DEFINE_IT(6, 5, 4)
DEFINE_IT(7, 6, 5)
DEFINE_IT(8, 7, 6)
#undef DEFINE_IT

ulong next_fft_number(ulong p)
{
    ulong bits, l, q;
    bits = n_nbits(p);
    l = n_trailing_zeros(p - 1);
    q = p - (UWORD(2) << l);
    if (bits < 15)
        flint_abort();
    if (n_nbits(q) == bits)
        return q;
    if (l < 5)
        return n_pow2(bits - 2) + 1;
    return n_pow2(bits) - n_pow2(l - 1) + 1;
}


static void fill_two_pow_table(double* x, ulong len, double p, double pinv)
{
    x[0] = 1;

    for (ulong i = 1; i < len; i++)
    {
        double t = x[i-1];
        double q = vec1d_round(vec1d_mul(t, 2*pinv));
        x[i] = vec1d_fnmadd(q, p, vec1d_add(t, t));
    }
}


void mpn_ctx_init(mpn_ctx_t R, ulong p)
{
    slong i;

    R->buffer = NULL;
    R->buffer_alloc = 0;

    for (i = 0; i < MPN_CTX_NSLOTS; i++)
    {
        if (i > 0)
            p = next_fft_number(p);

        while (!n_is_prime(p))
            p = next_fft_number(p);

        /* ffts */
        sd_fft_ctx_init_prime(R->ffts + i, p);

        /* crts */
        if (i == 0)
        {
            crt_data_init(R->crts + 0, p, 1, 1);
            *crt_data_co_prime_red(R->crts + 0, 0) = 1;
            crt_data_co_prime(R->crts + 0, 0)[0] = 1;
            crt_data_prod_primes(R->crts + 0)[0] = p;
        }
        else
        {
            ulong pi;
            ulong len = R->crts[i - 1].coeff_len;
            ulong* t, * tt;

            t = FLINT_ARRAY_ALLOC(2*(len + 2), ulong);
            tt = t + (len + 2);

            t[len + 1] = 0;
            t[len] = mpn_mul_1(t, crt_data_prod_primes(R->crts + i - 1), len, p);

            /* leave enough room for (product of primes)*(number of primes) */
            len += 2;
            mpn_mul_1(tt, t, len, i + 1);
            while (tt[len - 1] == 0)
                len--;

            crt_data_init(R->crts + i, p, len, i + 1);

            /* set product of primes */
            flint_mpn_copyi(crt_data_prod_primes(R->crts + i), t, len);

            /* set cofactors */
            for (pi = 0; pi < i + 1; pi++)
            {
                ulong* cofac = crt_data_co_prime(R->crts + i, pi);
                mpn_divexact_1(cofac, t, len, R->crts[pi].prime);
                *crt_data_co_prime_red(R->crts + i, pi) =
                                       mpn_mod_1(cofac, len, R->crts[pi].prime);
            }

            flint_free(t);
        }

        /* two_powers */
        R->two_powers[i].data = NULL;
        R->two_powers[i].length = 0;

        R->two_pow_tab[i] = FLINT_ARRAY_ALLOC(256, double);
        fill_two_pow_table(R->two_pow_tab[i], 256, R->ffts[i].p, R->ffts[i].pinv);
    }

    R->profiles_size = 0;

#ifdef USE_ORG
#define PUSH_PROFILE(np_, bits_, n, m) \
    i = R->profiles_size; \
    R->profiles[i].np        = np_; \
    R->profiles[i].bits      = bits_; \
    R->profiles[i].bn_bound  = crt_data_find_bound(R->crts + np_ - 1, bits_); \
    R->profiles[i].to_ffts   = CAT3(mpn_to_ffts, np_, bits_); \
    R->profiles[i].from_ffts = CAT4(new_mpn_from_ffts, np_, n, m); \
    R->profiles_size = i + 1;
#else
#define PUSH_PROFILE(np_, bits_, n, m) \
    i = R->profiles_size; \
    R->profiles[i].np        = np_; \
    R->profiles[i].bits      = bits_; \
    R->profiles[i].bn_bound  = crt_data_find_bound(R->crts + np_ - 1, bits_); \
    R->profiles[i].to_ffts   = NULL; \
    R->profiles[i].from_ffts = CAT4(new_mpn_from_ffts, np_, n, m); \
    R->profiles_size = i + 1;
#endif

    /* first must "always" work */
    PUSH_PROFILE(4, 64, 4,3);
    PUSH_PROFILE(3, 64, 3,2);
    PUSH_PROFILE(3, 66, 3,2);
    PUSH_PROFILE(3, 68, 3,2);
    PUSH_PROFILE(4, 78, 4,3);
    PUSH_PROFILE(4, 80, 4,3);
    PUSH_PROFILE(4, 82, 4,3);
    PUSH_PROFILE(4, 84, 4,3);
    PUSH_PROFILE(4, 86, 4,3);
    PUSH_PROFILE(4, 88, 4,3);
    PUSH_PROFILE(4, 90, 4,3);
    PUSH_PROFILE(4, 92, 4,3);
    PUSH_PROFILE(5,106, 4,4);
    PUSH_PROFILE(5,110, 4,4);
    PUSH_PROFILE(5,112, 4,4);
    PUSH_PROFILE(5,114, 4,4);
    PUSH_PROFILE(5,116, 4,4);
    PUSH_PROFILE(6,132, 5,4);
    PUSH_PROFILE(6,134, 5,4);
    PUSH_PROFILE(6,136, 5,4);
    PUSH_PROFILE(6,138, 5,4);
    PUSH_PROFILE(6,140, 5,4);
    PUSH_PROFILE(6,142, 5,4);
    PUSH_PROFILE(7,156, 6,5);
    PUSH_PROFILE(7,160, 6,5);
    PUSH_PROFILE(7,162, 6,5);
    PUSH_PROFILE(7,164, 6,5);
    PUSH_PROFILE(7,166, 6,5);
    PUSH_PROFILE(8,176, 7,6);
    PUSH_PROFILE(8,178, 7,6);
    PUSH_PROFILE(8,180, 7,6);
    PUSH_PROFILE(8,182, 7,6);
    PUSH_PROFILE(8,184, 7,6);
    PUSH_PROFILE(8,186, 7,6);
    PUSH_PROFILE(8,188, 7,6);
    FLINT_ASSERT(R->profiles_size <= MAX_NPROFILES);
}

#define VEC_SZ 4

const vec4d* mpn_ctx_two_pow_table(mpn_ctx_t R, ulong len, ulong np)
{
    ulong i, l;
    ulong nvs = n_cdiv(np, VEC_SZ);

    FLINT_ASSERT(MPN_CTX_NSLOTS >= nvs*VEC_SZ);

    if (R->two_powers[nvs-1].length >= len*nvs)
        return R->two_powers[nvs-1].data;

    flint_aligned_free(R->two_powers[nvs-1].data);
    vec4d* d = (vec4d*) flint_aligned_alloc(32, len*nvs*sizeof(vec4d));
    R->two_powers[nvs-1].data = d;

    vec4d* ps = (vec4d*) flint_aligned_alloc(32, 2*nvs*sizeof(vec4d));
    for (l = 0; l < nvs; l++)
    {
        /* just p */
        ps[2*l+0] = vec4d_set_d4(R->ffts[4*l+0].p,
                                 R->ffts[4*l+1].p,
                                 R->ffts[4*l+2].p,
                                 R->ffts[4*l+3].p);
        /* 2/p */
        ps[2*l+1] = vec4d_set_d4(R->ffts[4*l+0].pinv,
                                 R->ffts[4*l+1].pinv,
                                 R->ffts[4*l+2].pinv,
                                 R->ffts[4*l+3].pinv);
        ps[2*l+1] = vec4d_add(ps[2*l+1], ps[2*l+1]);
    }

    for (l = 0; l < nvs; l++)
        d[0*nvs + l] = vec4d_one();

    for (i = 1; i < len; i++)
    for (l = 0; l < nvs; l++)
    {
        vec4d t = d[(i-1)*nvs+l];
        vec4d p = ps[2*l+0];
        vec4d two_over_p = ps[2*l+1];
        vec4d q = vec4d_round(vec4d_mul(t, two_over_p));
        d[i*nvs+l] = vec4d_fnmadd(q, p, vec4d_add(t, t));
    }

    flint_aligned_free(ps);

    return d;
}

void mpn_ctx_clear(mpn_ctx_t R)
{
    slong i;

    for (i = 0; i < MPN_CTX_NSLOTS; i++)
    {
        sd_fft_ctx_clear(R->ffts + i);
        crt_data_clear(R->crts + i);
        flint_aligned_free(R->two_powers[i].data);
        flint_free(R->two_pow_tab[i]);
    }

    flint_aligned_free(R->buffer);
}


const profile_entry_struct* mpn_ctx_best_profile(
    const mpn_ctx_t R,
    thread_pool_handle** handles, slong* nhandles,
    ulong an,
    ulong bn)
{
    ulong i = 0;
    ulong best_i = 0;
    double best_score = 100000000.0*(an + bn);
    ulong nthreads;

    ulong thread_limit = 8;
    ulong zn = an + bn;

    if (zn < n_pow2(16-6))
        thread_limit = 1;
    else if (zn < n_pow2(17-6))
        thread_limit = 4;
    else if (zn < n_pow2(18-6))
        thread_limit = 5;
    else if (zn < n_pow2(19-6))
        thread_limit = 6;
    else if (zn < n_pow2(20-6))
        thread_limit = 7;

    *nhandles = flint_request_threads(handles, thread_limit);
    nthreads = 1 + *nhandles;

find_next:

    do {
        i++;
        if (i >= R->profiles_size)
            return R->profiles + best_i;
    } while (bn > R->profiles[i].bn_bound);

    /* maximize R->profiles[i].bits */

    FLINT_ASSERT(i < R->profiles_size);
    FLINT_ASSERT(bn <= R->profiles[i].bn_bound);

    while (i+1 < R->profiles_size &&
           bn <= R->profiles[i+1].bn_bound &&
           R->profiles[i+1].np == R->profiles[i].np)
    {
        i++;
    }

    ulong np = R->profiles[i].np;

    if (np % nthreads != 0)
        goto find_next;

    ulong bits = R->profiles[i].bits;
    ulong alen = n_cdiv(64*an, bits);
    ulong blen = n_cdiv(64*bn, bits);
    ulong zlen = alen + blen - 1;
    ulong ztrunc = n_round_up(zlen, BLK_SZ);
    ulong depth = n_max(LG_BLK_SZ, n_clog2(ztrunc));

    double ratio = (double)(ztrunc)/(double)(n_pow2(depth));
    double score = (1-0.25*ratio)*(1.0/1000000);
    score *= np*depth;
    score *= ztrunc;
    if (score < best_score)
    {
        best_i = i;
        best_score = score;
    }

    goto find_next;
}

void* mpn_ctx_fit_buffer(mpn_ctx_t R, ulong n)
{
    if (n > R->buffer_alloc)
    {
        flint_aligned_free(R->buffer);
        n = n_round_up(n_max(n, R->buffer_alloc*17/16), 4096);
        R->buffer = flint_aligned_alloc(4096, n);
        R->buffer_alloc = n;
    }
    return R->buffer;
}

/* pointwise mul of a with b and m */
void sd_fft_ctx_point_mul(
    const sd_fft_ctx_t Q,
    double* a,
    const double* b,
    ulong mm,
    ulong depth)
{
    double m = mm;
    if (m > 0.5*Q->p)
        m -= Q->p;

    vec8d M = vec8d_set_d(m);
    vec8d n    = vec8d_set_d(Q->p);
    vec8d ninv = vec8d_set_d(Q->pinv);
    for (ulong I = 0; I < n_pow2(depth - LG_BLK_SZ); I++)
    {
        double* ax = a + sd_fft_ctx_blk_offset(I);
        const double* bx = b + sd_fft_ctx_blk_offset(I);
        ulong j = 0; do {
            vec8d x0, x1, b0, b1;
            x0 = vec8d_load(ax+j+0);
            x1 = vec8d_load(ax+j+8);
            b0 = vec8d_load(bx+j+0);
            b1 = vec8d_load(bx+j+8);
            x0 = vec8d_mulmod2(x0, M, n, ninv);
            x1 = vec8d_mulmod2(x1, M, n, ninv);
            x0 = vec8d_mulmod2(x0, b0, n, ninv);
            x1 = vec8d_mulmod2(x1, b1, n, ninv);
            vec8d_store(ax+j+0, x0);
            vec8d_store(ax+j+8, x1);
        } while (j += 16, j < BLK_SZ);
    }
}

typedef struct {
    to_ffts_func to_ffts;
    sd_fft_ctx_struct* ffts;
    ulong stride;
    const vec4d* two_pow;
    double* abuf;
    const ulong* a;
    ulong an;
    ulong atrunc;
    ulong a_start_easy;
    ulong a_stop_easy;
    ulong a_start_hard;
    ulong a_stop_hard;
    double* bbuf;
    const ulong* b;
    ulong bn;
    ulong btrunc;
    ulong b_start_easy;
    ulong b_stop_easy;
    ulong b_start_hard;
    ulong b_stop_hard;
} mod_worker_struct;

void mod_worker_func(void* varg)
{
    mod_worker_struct* X = (mod_worker_struct*) varg;

    X->to_ffts(X->ffts, X->abuf, X->stride, X->a, X->an, X->atrunc, X->two_pow,
             X->a_start_easy, X->a_stop_easy, X->a_start_hard, X->a_stop_hard);

    X->to_ffts(X->ffts, X->bbuf, X->stride, X->b, X->bn, X->btrunc, X->two_pow,
             X->b_start_easy, X->b_stop_easy, X->b_start_hard, X->b_stop_hard);
}

typedef struct fft_worker_struct {
    sd_fft_ctx_struct* fctx;
    ulong cop;
    ulong depth;
    ulong ztrunc;
    double* abuf;
    ulong atrunc;
    double* bbuf;
    ulong btrunc;
    struct fft_worker_struct* next;
} fft_worker_struct;

void fft_worker_func(void* varg)
{
    fft_worker_struct* X = (fft_worker_struct*) varg;
    ulong m;

    do {
        sd_fft_ctx_fit_depth(X->fctx, X->depth);
        sd_fft_ctx_fft_trunc(X->fctx, X->bbuf, X->depth, X->btrunc, X->ztrunc);
        sd_fft_ctx_fft_trunc(X->fctx, X->abuf, X->depth, X->atrunc, X->ztrunc);
        NMOD_RED2(m, X->cop >> (64 - X->depth), X->cop << X->depth, X->fctx->mod);
        m = nmod_inv(m, X->fctx->mod);
        sd_fft_ctx_point_mul(X->fctx, X->abuf, X->bbuf, m, X->depth);
        sd_fft_ctx_ifft_trunc(X->fctx, X->abuf, X->depth, X->ztrunc);
    } while (X = X->next, X != NULL);
}

unsigned char _addto(ulong* z, ulong zn, ulong* a, ulong an, unsigned char cf)
{
    FLINT_ASSERT(zn >= an);
    FLINT_ASSERT(an > 0);

    ulong i = 0;
    do {
        cf = _addcarry_ulong(cf, z[i], a[i], &z[i]);
    } while (i++, i < an);

    while (i < zn && cf != 0)
        cf = _addcarry_ulong(cf, z[i], 0, &z[i]);

    return cf;    
}

typedef struct {
    new_from_ffts_func from_ffts;
    ulong* z;
    ulong zn;
    ulong zlen;
    sd_fft_ctx_struct* fctxs;
    double* abuf;
    ulong stride;
    crt_data_struct* crts;
    ulong bits;
    ulong start_easy;
    ulong stop_easy;
    ulong* overhang;
} crt_worker_struct;

void crt_worker_func(void* varg)
{
    crt_worker_struct* X = (crt_worker_struct*) varg;

    X->from_ffts(X->z, X->zn, X->zlen, X->fctxs, X->abuf, X->stride, X->crts,
                X->bits, X->start_easy, X->stop_easy, X->overhang);
}



#ifdef USE_ORG
void mpn_ctx_mpn_mul(mpn_ctx_t R, ulong* z, ulong* a, ulong an, ulong* b, ulong bn)
{
    thread_pool_handle* handles;
    slong nhandles;
    const profile_entry_struct* P = mpn_ctx_best_profile(R, &handles, &nhandles, an, bn);
    ulong np = P->np;
    ulong bits = P->bits;
    ulong zn = an + bn;
    ulong alen = n_cdiv(FLINT_BITS*an, bits);
    ulong blen = n_cdiv(FLINT_BITS*bn, bits);
    ulong zlen = alen + blen - 1;
    ulong atrunc = n_round_up(alen, BLK_SZ);
    ulong btrunc = n_round_up(blen, BLK_SZ);
    ulong ztrunc = n_round_up(zlen, BLK_SZ);
    ulong depth = n_max(LG_BLK_SZ, n_clog2(ztrunc));
    ulong nthreads = nhandles + 1;

    FLINT_ASSERT(an > 0);
    FLINT_ASSERT(bn > 0);
    FLINT_ASSERT(nthreads <= 8);
    FLINT_ASSERT(flint_mpn_cmp_ui_2exp(crt_data_prod_primes(R->crts+np-1),
                                  R->crts[np-1].coeff_len, blen, 2*bits) >= 0);

#define TIME_THIS 0

#if TIME_THIS
timeit_t timer, timer_overall;
flint_printf("\n------------ zn = %wu, bits = %wu, np = %wu, nthreads = %wu -------------\n", zn, bits, np, nthreads);
#endif

#if TIME_THIS
timeit_start(timer_overall);
#endif

    ulong stride = n_round_up(sd_fft_ctx_data_size(depth), 128);
    double* abuf = (double*) mpn_ctx_fit_buffer(R, 2*np*stride*sizeof(double));
    double* bbuf = abuf + np*stride;
    const vec4d* two_pow = mpn_ctx_two_pow_table(R, bits+5, np);

#if TIME_THIS
timeit_start(timer);
#endif

    {
        mod_worker_struct w[8];
        /* if i*bits + 32 < 64*an, then the index into a is always in bounds */
        ulong a_stop_easy = n_min(atrunc, (64*an - 33)/bits);
        /* if i*bits >= 64*an, then the index into a is always out of bounds */
        ulong a_stop_hard = n_min(atrunc, (64*an + bits - 1)/bits);
        ulong b_stop_easy = n_min(btrunc, (64*bn - 33)/bits);
        ulong b_stop_hard = n_min(btrunc, (64*bn + bits - 1)/bits);
        ulong rounding;

        /* some fixups for loop unrollings: round down the easy stops */
        if (bits%8 == 0)
        {
            rounding = 4;
        }
        else if (bits%4 == 0)
        {
            rounding = 8;
        }
        else
        {
            FLINT_ASSERT(bits%2 == 0);
            rounding = 16;
        }

        a_stop_easy &= -rounding;
        b_stop_easy &= -rounding;

        for (ulong i = 0; i < nthreads; i++)
        {
            w[i].to_ffts = P->to_ffts;
            w[i].ffts = R->ffts;
            w[i].stride = stride;
            w[i].two_pow = two_pow;
            w[i].abuf = abuf;
            w[i].a = a;
            w[i].an = an,
            w[i].atrunc = atrunc;
            w[i].bbuf = bbuf;
            w[i].b = b;
            w[i].bn = bn,
            w[i].btrunc = btrunc;

            w[i].a_start_easy = n_round_up((i+0)*a_stop_easy/nthreads, rounding);
            w[i].a_stop_easy  = n_round_up((i+1)*a_stop_easy/nthreads, rounding);
            w[i].b_start_easy = n_round_up((i+0)*b_stop_easy/nthreads, rounding);
            w[i].b_stop_easy  = n_round_up((i+1)*b_stop_easy/nthreads, rounding);

            /* only the last thread i = nthreads - 1 does the hard ends */
            if (i + 1 == nthreads)
            {
                w[i].a_start_hard = a_stop_easy;
                w[i].a_stop_hard = a_stop_hard;
                w[i].b_start_hard = b_stop_easy;
                w[i].b_stop_hard = b_stop_hard;
            }
            else
            {
                w[i].a_start_hard = atrunc;
                w[i].a_stop_hard = atrunc;
                w[i].b_start_hard = btrunc;
                w[i].b_stop_hard = btrunc;
            }
        }

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wake(global_thread_pool, handles[i - 1], 0,
                                                       mod_worker_func, w + i);
        mod_worker_func(w + 0);

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wait(global_thread_pool, handles[i - 1]);
    }

#if TIME_THIS
timeit_stop(timer);
if (timer->wall > 5)
flint_printf("mod: %wd\n", timer->wall);
#endif

#if TIME_THIS
timeit_start(timer);
#endif

    {
        fft_worker_struct w[8];

        FLINT_ASSERT(np >= nthreads);

        for (ulong l = 0; l < np; l++)
        {
            fft_worker_struct* X = w + l;

            X->fctx = R->ffts + l;
            X->cop = *crt_data_co_prime_red(R->crts + np - 1, l);
            X->depth = depth;
            X->ztrunc = ztrunc;
            X->abuf = abuf + l*stride;
            X->atrunc = atrunc;
            X->bbuf = bbuf + l*stride;
            X->btrunc = btrunc;
            X->next = (l + nthreads < np) ? X + nthreads : NULL;
        }

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wake(global_thread_pool, handles[i - 1], 0,
                                                       fft_worker_func, w + i);
        fft_worker_func(w + 0);

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wait(global_thread_pool, handles[i - 1]);
    }

#if TIME_THIS
timeit_stop(timer);
if (timer->wall > 5)
flint_printf("fft: %wd\n", timer->wall);
#endif

#if TIME_THIS
timeit_start(timer);
#endif

    ulong n = R->crts[np-1].coeff_len;

    ulong end_easy = (zn >= n+1 ? zn - (n+1) : (ulong)(0))*64/P->bits;
    end_easy &= -BLK_SZ;

    {
        ulong overhang[10][8];
        crt_worker_struct w[10];

        for (ulong l = 0; l <= nhandles; l++)
        {
            crt_worker_struct* X = w + l;
            X->from_ffts = P->from_ffts;
            X->z = z;
            X->zn = zn;
            X->zlen = zlen;
            X->fctxs = R->ffts;
            X->abuf = abuf;
            X->stride = stride;
            X->crts = R->crts;
            X->bits = bits;
            X->start_easy = n_round_up((l+0)*end_easy/(nthreads), BLK_SZ);
            X->stop_easy  = n_round_up((l+1)*end_easy/(nthreads), BLK_SZ);
            X->overhang = (l == nhandles) ? NULL : overhang[l];
        }

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wake(global_thread_pool, handles[i - 1], 0,
                                                   crt_worker_func, w + i);
        crt_worker_func(w + 0);

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wait(global_thread_pool, handles[i - 1]);

        unsigned char cf = 0;
        for (ulong i = 1; i <= nhandles; i++)
        {
            ulong word_start = w[i].start_easy*bits/64;
            if (i == nhandles)
            {
                cf = _addto(z + word_start, zn - word_start, overhang[i-1], n, cf);
            }
            else
            {
                ulong word_stop = w[i].stop_easy*bits/64;
                if (word_stop > word_start)
                {
                    cf = _addto(z + word_start, word_stop - word_start, overhang[i-1], n, cf);
                }
                else
                {
                    for (ulong k = 0; k < n; k++)
                    {
                        FLINT_ASSERT(overhang[i][k] == 0);
                        overhang[i][k] = overhang[i-1][k];
                    }
                }
            }
        }
    }

#if TIME_THIS
timeit_stop(timer);
if (timer->wall > 5)
flint_printf("crt: %wd\n", timer->wall);
timeit_stop(timer_overall);
if (timer_overall->wall > 5)
flint_printf("   : %wd\n", timer_overall->wall);
#endif

#undef TIME_THIS


    flint_give_back_threads(handles, nhandles);

}




#else
/*********************************************************************************************/

#define aindex(i) (a[i])

FLINT_FORCE_INLINE void vec4ui_print(vec4ui a)
{
flint_printf("[hi %016llx_%016llx_%016llx_%016llx lo]", _mm256_extract_epi64(a, 3)
                                    , _mm256_extract_epi64(a, 2)
                                    , _mm256_extract_epi64(a, 1)
                                    , _mm256_extract_epi64(a, 0));

}


/*
flint_printf("\nak0: "); vec4ui_print(ak0); flint_printf("\n");
*/

/*
if (_mm256_extract_epi64(ak0, 2) != aindex(k+2*BLK_SZ/8/32*bits)>>j)
{
    flint_printf("\n");
    flint_printf("_mm256_extract_epi64(ak0, 2): %016llx\n", _mm256_extract_epi64(ak0, 2));
    flint_printf("aindex(k+2*BLK_SZ/8/32*bits)>>j: %016llx\n", aindex(k+2*BLK_SZ/8/32*bits)>>j);
    flint_printf("oops2\n");
    fflush(stdout);
    flint_abort();
}
*/


FLINT_FORCE_INLINE vec4d vec4ui_convert_limited_vec4d(vec4ui a) {
    __m256d t = _mm256_set1_pd(0x0010000000000000);
    return _mm256_sub_pd(_mm256_or_pd(_mm256_castsi256_pd(a), t), t);
}

FLINT_FORCE_INLINE vec8d vec4i32_convert_vec8d(__m256i a)
{
    __m256i mask = _mm256_set1_epi32(0x43300000);
    __m256i ak0 = _mm256_unpacklo_epi32(a, mask);
    __m256i ak1 = _mm256_unpackhi_epi32(a, mask);
    __m256d t = _mm256_set1_pd(0x0010000000000000);
    vec8d z;
    z.e1 = _mm256_sub_pd(_mm256_castsi256_pd(ak0), t);
    z.e2 = _mm256_sub_pd(_mm256_castsi256_pd(ak1), t);
    return z;
}



#define vec4ui_part(a, i) _mm256_extract_epi64(a, i)

FLINT_FORCE_INLINE vec4ui vec4ui_set_ui4(ulong a0, ulong a1, ulong a2, ulong a3) {
    return _mm256_set_epi64x(a3, a2, a1, a0);
}

#if 1
void new_mpn_to_fft_inner(
    sd_fft_ctx_t Q,
    double* z,
    const uint32_t* a,
    ulong iq_stop_easy,
    ulong bits,
    const double* two_pow)
{
    vec8d p    = vec8d_set_d(Q->p);
    vec8d pinv = vec8d_set_d(Q->pinv);

    ulong s = BLK_SZ/8/32*bits;

    __m256i tab[200];

    for (ulong iq = 0; iq < iq_stop_easy; iq++)
    {
        double* zI = sd_fft_ctx_blk_index(z, iq);

        const uint32_t* aa = a + iq*(BLK_SZ/32)*bits;


        ulong k = 0;
#if 1
        do {
            __m256i A, B, C, D, E, F, G, H, X0, X1, Y0, Y1, Z0, Z1, W0, W1, P, Q, R, S, T, U, V, W;

            A = _mm256_loadu_si256((const __m256i*) (aa + k + 0*s));
            B = _mm256_loadu_si256((const __m256i*) (aa + k + 1*s));
            C = _mm256_loadu_si256((const __m256i*) (aa + k + 2*s));
            D = _mm256_loadu_si256((const __m256i*) (aa + k + 3*s));
            E = _mm256_loadu_si256((const __m256i*) (aa + k + 4*s));
            F = _mm256_loadu_si256((const __m256i*) (aa + k + 5*s));
            G = _mm256_loadu_si256((const __m256i*) (aa + k + 6*s));
            H = _mm256_loadu_si256((const __m256i*) (aa + k + 7*s));

            X0 = _mm256_unpacklo_epi32(A, B); // a0 b0 | a1 b1 | a4 b4 | a5 b5
            Y0 = _mm256_unpacklo_epi32(C, D); // c0 d0 | c1 d1 | c4 d4 | c5 d5
            Z0 = _mm256_unpacklo_epi32(E, F); // e0 f0 | e1 f1 | e4 f4 | e5 f5
            W0 = _mm256_unpacklo_epi32(G, H); // g0 h0 | g1 h1 | g4 h4 | g5 h5
            X1 = _mm256_unpackhi_epi32(A, B); // a2 b2 | a3 b3 | a6 b6 | a7 b7
            Y1 = _mm256_unpackhi_epi32(C, D); // c2 d2 | c3 d3 | c6 d6 | c7 d7
            Z1 = _mm256_unpackhi_epi32(E, F); // e2 f2 | e3 f3 | e6 f6 | e7 f7
            W1 = _mm256_unpackhi_epi32(G, H); // g2 h2 | g3 h3 | g6 h6 | g7 h7

            P = _mm256_unpacklo_epi64(X0, Y0); // a0 b0 | c0 d0 | a4 b4 | c4 d4
            Q = _mm256_unpackhi_epi64(X0, Y0); // a1 b1 | c1 d1 | a5 b5 | c5 d5
            R = _mm256_unpacklo_epi64(Z0, W0); // e0 f0 | g0 h0 | e4 f4 | g4 h4
            S = _mm256_unpackhi_epi64(Z0, W0); // e1 f1 | g1 h1 | e5 f5 | g5 h5
            T = _mm256_unpacklo_epi64(X1, Y1); // a2 b2 | c2 d2 | a6 b6 | c6 d6
            U = _mm256_unpackhi_epi64(X1, Y1); // a3 b3 | c3 d3 | a7 b7 | c7 d7
            V = _mm256_unpacklo_epi64(Z1, W1); // e2 f2 | g2 h2 | e6 f6 | g6 h6
            W = _mm256_unpackhi_epi64(Z1, W1); // e3 f3 | g3 h3 | e7 f7 | g7 h7

            tab[k+0] = _mm256_permute2x128_si256(P, R, 0 + 16*2);
            tab[k+4] = _mm256_permute2x128_si256(P, R, 1 + 16*3);
            tab[k+1] = _mm256_permute2x128_si256(Q, S, 0 + 16*2);
            tab[k+5] = _mm256_permute2x128_si256(Q, S, 1 + 16*3);
            tab[k+2] = _mm256_permute2x128_si256(T, V, 0 + 16*2);
            tab[k+6] = _mm256_permute2x128_si256(T, V, 1 + 16*3);
            tab[k+3] = _mm256_permute2x128_si256(U, W, 0 + 16*2);
            tab[k+7] = _mm256_permute2x128_si256(U, W, 1 + 16*3);

        } while (k += 8, k + 8 <= s);
#endif

        while (k < s) {
            tab[k] = _mm256_set_epi32(aa[k + 7*s], aa[k + 6*s],
                                      aa[k + 5*s], aa[k + 4*s],
                                      aa[k + 3*s], aa[k + 2*s],
                                      aa[k + 1*s], aa[k + 0*s]);
            k++;
        }

        for (ulong ir = 0; ir < BLK_SZ/8; ir++)
        {
            ulong k = (ir*bits)/32;
            ulong j = (ir*bits)%32;

            __m256i AK = tab[k];/*_mm256_i32gather_epi32((const int*)aa+k, index, 4);*/
            __m256i AKJ = _mm256_srl_epi32(AK, _mm_set_epi32(0,0,0,j));
            vec8d ak = vec4i32_convert_vec8d(AKJ);

            vec8d X = ak;
            k++;
            j = 32 - j;
            do {
                AK = tab[k];/*_mm256_i32gather_epi32((const int*)aa+k, index, 4);*/
                AKJ = AK;
                ak = vec4i32_convert_vec8d(AKJ);
                X = vec8d_add(X, vec8d_mulmod2(ak, vec8d_set_d(two_pow[j]), p, pinv));
                k++;
                j += 32;
            } while (j + 32 <= bits);

            if ((bits-j) != 0)
            {
                AK = tab[k];/*_mm256_i32gather_epi32((const int*)aa+k, index, 4);*/
                AKJ = _mm256_sll_epi32(AK, _mm_set_epi32(0,0,0,32-(bits-j)));
                ak = vec4i32_convert_vec8d(AKJ);
                X = vec8d_add(X, vec8d_mulmod2(ak, vec8d_set_d(two_pow[bits-32]), p, pinv));
                k++;
            }

            X = vec8d_reduce_to_pm1n(X, p, pinv);

            // vec4i32_convert_vec8d make the Xs slightly out of order
            zI[ir+0*BLK_SZ/8] = X.e1[0];
            zI[ir+1*BLK_SZ/8] = X.e1[1];
            zI[ir+4*BLK_SZ/8] = X.e1[2];
            zI[ir+5*BLK_SZ/8] = X.e1[3];
            zI[ir+2*BLK_SZ/8] = X.e2[0];
            zI[ir+3*BLK_SZ/8] = X.e2[1];
            zI[ir+6*BLK_SZ/8] = X.e2[2];
            zI[ir+7*BLK_SZ/8] = X.e2[3];
        }
    }
}

#else
void new_mpn_to_fft_inner(
    sd_fft_ctx_t Q,
    double* z,
    const uint32_t* a,
    ulong iq_stop_easy,
    ulong bits,
    const double* two_pow)
{
    vec8d p    = vec8d_set_d(Q->p);
    vec8d pinv = vec8d_set_d(Q->pinv);

    for (ulong iq = 0; iq < iq_stop_easy; iq++)
    {
        double* zI = sd_fft_ctx_blk_index(z, iq);

        for (ulong ir = 0; ir < BLK_SZ/8; ir++)
        {
            ulong k = iq*(BLK_SZ/32)*bits + (ir*bits)/32;
            ulong j = (ir*bits)%32;

            vec8d ak = vec8d_set_d8(aindex(k+0*BLK_SZ/8/32*bits) >> j,
                                    aindex(k+1*BLK_SZ/8/32*bits) >> j,
                                    aindex(k+2*BLK_SZ/8/32*bits) >> j,
                                    aindex(k+3*BLK_SZ/8/32*bits) >> j,
                                    aindex(k+4*BLK_SZ/8/32*bits) >> j,
                                    aindex(k+5*BLK_SZ/8/32*bits) >> j,
                                    aindex(k+6*BLK_SZ/8/32*bits) >> j,
                                    aindex(k+7*BLK_SZ/8/32*bits) >> j);
            vec8d X = ak;
            k++;
            j = 32 - j;
            while (j + 32 <= bits)
            {
                ak = vec8d_set_d8(aindex(k+0*BLK_SZ/8/32*bits),
                                  aindex(k+1*BLK_SZ/8/32*bits),
                                  aindex(k+2*BLK_SZ/8/32*bits),
                                  aindex(k+3*BLK_SZ/8/32*bits),
                                  aindex(k+4*BLK_SZ/8/32*bits),
                                  aindex(k+5*BLK_SZ/8/32*bits),
                                  aindex(k+6*BLK_SZ/8/32*bits),
                                  aindex(k+7*BLK_SZ/8/32*bits));
                X = vec8d_add(X, vec8d_mulmod2(ak, vec8d_set_d(two_pow[j]), p, pinv));
                k++;
                j += 32;
            }

            if ((bits-j) != 0)
            {
                ak = vec8d_set_d8(aindex(k+0*BLK_SZ/8/32*bits) << (32-(bits-j)),
                                  aindex(k+1*BLK_SZ/8/32*bits) << (32-(bits-j)),
                                  aindex(k+2*BLK_SZ/8/32*bits) << (32-(bits-j)),
                                  aindex(k+3*BLK_SZ/8/32*bits) << (32-(bits-j)),
                                  aindex(k+4*BLK_SZ/8/32*bits) << (32-(bits-j)),
                                  aindex(k+5*BLK_SZ/8/32*bits) << (32-(bits-j)),
                                  aindex(k+6*BLK_SZ/8/32*bits) << (32-(bits-j)),
                                  aindex(k+7*BLK_SZ/8/32*bits) << (32-(bits-j)));
                X = vec8d_add(X, vec8d_mulmod2(ak, vec8d_set_d(two_pow[bits-32]), p, pinv));
            }

            X = vec8d_reduce_to_pm1n(X, p, pinv);

            zI[ir+0*BLK_SZ/8] = X.e1[0];
            zI[ir+1*BLK_SZ/8] = X.e1[1];
            zI[ir+2*BLK_SZ/8] = X.e1[2];
            zI[ir+3*BLK_SZ/8] = X.e1[3];
            zI[ir+4*BLK_SZ/8] = X.e2[0];
            zI[ir+5*BLK_SZ/8] = X.e2[1];
            zI[ir+6*BLK_SZ/8] = X.e2[2];
            zI[ir+7*BLK_SZ/8] = X.e2[3];
        }

    }
}
#endif

#undef aindex


void new_mpn_to_fft(
    sd_fft_ctx_t Q,
    double* z, ulong ztrunc,
    const ulong* a_, ulong an_,
    ulong bits,
    const double* two_pow)
{
    const uint32_t* a = (const uint32_t*)(a_);
    ulong an = 2*an_;

    ulong iq;

    /* if i*bits + 32 < 32*an, then the index into a is always in bounds */
    ulong i_stop_easy = n_min(ztrunc, (32*an - 33)/bits);
    i_stop_easy = n_min(i_stop_easy, ztrunc);
    ulong iq_stop_easy = i_stop_easy/BLK_SZ;

    new_mpn_to_fft_inner(Q, z, a, iq_stop_easy, bits, two_pow);

    /* now the hard ones */
{
    vec1d p = vec1d_set_d(Q->p);
    vec1d pinv = vec1d_set_d(Q->pinv);
#define aindex(i) (((i) < an) ? a[i] : (uint32_t)(0))
    for (iq = iq_stop_easy; iq < ztrunc/BLK_SZ; iq++)
    {
        double* zI = sd_fft_ctx_blk_index(z, iq);

        for (ulong ir = 0; ir < BLK_SZ; ir++)
        {
            ulong k = ((iq*BLK_SZ+ir)*bits)/32;
            ulong j = ((iq*BLK_SZ+ir)*bits)%32;

            vec1d ak = vec1d_set_d(aindex(k) >> j);
            vec1d X = ak;
            k++;
            j = 32 - j;
            while (j + 32 <= bits)
            {
                ak = vec1d_set_d(aindex(k));
                X = vec1d_add(X, vec1d_mulmod2(ak, two_pow[j], p, pinv));
                k++;
                j += 32;
            }

            if ((bits-j) != 0)
            {
                ak = vec1d_set_d(aindex(k) << (32-(bits-j)));
                X = vec1d_add(X, vec1d_mulmod2(ak, two_pow[bits-32], p, pinv));
            }

            X = vec1d_reduce_to_pm1n(X, p, pinv);

            zI[ir] = X;
        }
    }
#undef aindex
}
}






typedef struct new_fft_worker_struct {
    ulong bits;
    sd_fft_ctx_struct* fctx;
    const double* two_pow_tab;
    ulong cop;
    ulong depth;
    ulong ztrunc;
    const ulong* a;
    ulong an;
    double* abuf;
    ulong atrunc;
    const ulong* b;
    ulong bn;
    double* bbuf;
    ulong btrunc;
    struct new_fft_worker_struct* next;
} new_fft_worker_struct;

void new_fft_worker_func(void* varg)
{
    new_fft_worker_struct* X = (new_fft_worker_struct*) varg;
    ulong m;

    do {
        sd_fft_ctx_fit_depth(X->fctx, X->depth);

        new_mpn_to_fft(X->fctx, X->bbuf, X->btrunc, X->b, X->bn, X->bits, X->two_pow_tab);
        sd_fft_ctx_fft_trunc(X->fctx, X->bbuf, X->depth, X->btrunc, X->ztrunc);

        new_mpn_to_fft(X->fctx, X->abuf, X->atrunc, X->a, X->an, X->bits, X->two_pow_tab);
        sd_fft_ctx_fft_trunc(X->fctx, X->abuf, X->depth, X->atrunc, X->ztrunc);

        NMOD_RED2(m, X->cop >> (64 - X->depth), X->cop << X->depth, X->fctx->mod);
        m = nmod_inv(m, X->fctx->mod);
        sd_fft_ctx_point_mul(X->fctx, X->abuf, X->bbuf, m, X->depth);

        sd_fft_ctx_ifft_trunc(X->fctx, X->abuf, X->depth, X->ztrunc);

    } while (X = X->next, X != NULL);
}


void mpn_ctx_mpn_mul(mpn_ctx_t R, ulong* z, ulong* a, ulong an, ulong* b, ulong bn)
{
    thread_pool_handle* handles;
    slong nhandles;
    const profile_entry_struct* P = mpn_ctx_best_profile(R, &handles, &nhandles, an, bn);
    ulong np = P->np;
    ulong bits = P->bits;
    ulong zn = an + bn;
    ulong alen = n_cdiv(FLINT_BITS*an, bits);
    ulong blen = n_cdiv(FLINT_BITS*bn, bits);
    ulong zlen = alen + blen - 1;
    ulong atrunc = n_round_up(alen, BLK_SZ);
    ulong btrunc = n_round_up(blen, BLK_SZ);
    ulong ztrunc = n_round_up(zlen, BLK_SZ);
    ulong depth = n_max(LG_BLK_SZ, n_clog2(ztrunc));
    ulong nthreads = nhandles + 1;

    FLINT_ASSERT(an > 0);
    FLINT_ASSERT(bn > 0);
    FLINT_ASSERT(nthreads <= 8);
    FLINT_ASSERT(flint_mpn_cmp_ui_2exp(crt_data_prod_primes(R->crts+np-1),
                                  R->crts[np-1].coeff_len, blen, 2*bits) >= 0);

#define TIME_THIS 0

#if TIME_THIS
timeit_t timer, timer_overall;
flint_printf("\n------------ zn = %wu, bits = %wu, np = %wu, nthreads = %wu -------------\n", zn, bits, np, nthreads);
#endif

#if TIME_THIS
timeit_start(timer_overall);
#endif

    ulong stride = n_round_up(sd_fft_ctx_data_size(depth) + 65, 128);
    double* abuf = (double*) mpn_ctx_fit_buffer(R, (np+nthreads)*stride*sizeof(double));
    double* bbuf = abuf + np*stride;

#if TIME_THIS
timeit_start(timer);
#endif

    {
        new_fft_worker_struct w[8];

        FLINT_ASSERT(np >= nthreads);

        for (ulong l = 0; l < np; l++)
        {
            new_fft_worker_struct* X = w + l;

            X->bits = bits;
            X->fctx = R->ffts + l;
            X->cop = *crt_data_co_prime_red(R->crts + np - 1, l);
            X->depth = depth;
            X->ztrunc = ztrunc;
            X->a = a;
            X->an = an;
            X->abuf = abuf + l*stride;
            X->atrunc = atrunc;
            X->b = b;
            X->bn = bn;
            X->bbuf = bbuf + (l%nthreads)*stride;
            X->btrunc = btrunc;
            X->two_pow_tab = R->two_pow_tab[l];
            X->next = (l + nthreads < np) ? X + nthreads : NULL;
        }

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wake(global_thread_pool, handles[i - 1], 0,
                                                   new_fft_worker_func, w + i);
        new_fft_worker_func(w + 0);

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wait(global_thread_pool, handles[i - 1]);
    }

#if TIME_THIS
timeit_stop(timer);
if (timer->wall > 5)
flint_printf("mod & fft: %wd\n", timer->wall);
#endif

#if TIME_THIS
timeit_start(timer);
#endif


    ulong n = R->crts[np-1].coeff_len;

    ulong end_easy = (zn >= n+1 ? zn - (n+1) : (ulong)(0))*64/P->bits;
    end_easy &= -BLK_SZ;

    {
        ulong overhang[10][8];
        crt_worker_struct w[10];

        for (ulong l = 0; l <= nhandles; l++)
        {
            crt_worker_struct* X = w + l;
            X->from_ffts = P->from_ffts;
            X->z = z;
            X->zn = zn;
            X->zlen = zlen;
            X->fctxs = R->ffts;
            X->abuf = abuf;
            X->stride = stride;
            X->crts = R->crts;
            X->bits = bits;
            X->start_easy = n_round_up((l+0)*end_easy/(nthreads), BLK_SZ);
            X->stop_easy  = n_round_up((l+1)*end_easy/(nthreads), BLK_SZ);
            X->overhang = (l == nhandles) ? NULL : overhang[l];
        }

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wake(global_thread_pool, handles[i - 1], 0,
                                                   crt_worker_func, w + i);
        crt_worker_func(w + 0);

        for (slong i = nhandles; i > 0; i--)
            thread_pool_wait(global_thread_pool, handles[i - 1]);

        unsigned char cf = 0;
        for (ulong i = 1; i <= nhandles; i++)
        {
            ulong word_start = w[i].start_easy*bits/64;
            if (i == nhandles)
            {
                cf = _addto(z + word_start, zn - word_start, overhang[i-1], n, cf);
            }
            else
            {
                ulong word_stop = w[i].stop_easy*bits/64;
                if (word_stop > word_start)
                {
                    cf = _addto(z + word_start, word_stop - word_start, overhang[i-1], n, cf);
                }
                else
                {
                    for (ulong k = 0; k < n; k++)
                    {
                        FLINT_ASSERT(overhang[i][k] == 0);
                        overhang[i][k] = overhang[i-1][k];
                    }
                }
            }
        }
    }


#if TIME_THIS
timeit_stop(timer);
if (timer->wall > 5)
flint_printf("crt: %wd\n", timer->wall);
timeit_stop(timer_overall);
if (timer_overall->wall > 5)
flint_printf("   : %wd\n", timer_overall->wall);
#endif

#undef TIME_THIS


    flint_give_back_threads(handles, nhandles);

}

#endif