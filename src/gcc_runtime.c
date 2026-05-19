/*
 * gcc_runtime.c - GCC 64-bit arithmetic runtime helpers for -nostdlib builds.
 *
 * GCC emits calls to these when lowering 64-bit operations on m68k.
 * 68k is big-endian: in a u64, the high word is at the lower address.
 */

typedef unsigned int       u32;
typedef unsigned long long u64;

typedef union {
    u64 v;
    struct { u32 hi; u32 lo; } s;
} U64;

u64 __ashldi3(u64 val, int cnt)
{
    U64 u; u.v = val;
    if (cnt == 0)   return val;
    if (cnt >= 64)  { u.s.hi = u.s.lo = 0; }
    else if (cnt >= 32) { u.s.hi = u.s.lo << (cnt - 32); u.s.lo = 0; }
    else { u.s.hi = (u.s.hi << cnt) | (u.s.lo >> (32 - cnt)); u.s.lo <<= cnt; }
    return u.v;
}

u64 __lshrdi3(u64 val, int cnt)
{
    U64 u; u.v = val;
    if (cnt == 0)   return val;
    if (cnt >= 64)  { u.s.hi = u.s.lo = 0; }
    else if (cnt >= 32) { u.s.lo = u.s.hi >> (cnt - 32); u.s.hi = 0; }
    else { u.s.lo = (u.s.lo >> cnt) | (u.s.hi << (32 - cnt)); u.s.hi >>= cnt; }
    return u.v;
}

long long __ashrdi3(long long val, int cnt)
{
    typedef union { long long v; struct { int hi; unsigned int lo; } s; } S64;
    S64 u; u.v = val;
    if (cnt == 0)   return val;
    if (cnt >= 64)  { u.s.lo = u.s.hi = (u.s.hi < 0) ? -1 : 0; }
    else if (cnt >= 32) { u.s.lo = (unsigned int)(u.s.hi >> (cnt - 32)); u.s.hi >>= 31; }
    else { u.s.lo = (u.s.lo >> cnt) | ((unsigned int)u.s.hi << (32 - cnt)); u.s.hi >>= cnt; }
    return u.v;
}

static u64 mul32x32(u32 a, u32 b)
{
    u32 a0 = a & 0xFFFF, a1 = a >> 16;
    u32 b0 = b & 0xFFFF, b1 = b >> 16;
    u32 p00 = a0*b0, p01 = a0*b1, p10 = a1*b0, p11 = a1*b1;
    u32 mid = p01 + p10;
    u32 carry_m = (mid < p01) ? 1u : 0u;
    u32 lo = p00 + (mid << 16);
    u32 carry_lo = (lo < p00) ? 1u : 0u;
    u32 hi = p11 + (p01 >> 16) + (p10 >> 16) + (carry_m << 16) + carry_lo;
    U64 r; r.s.hi = hi; r.s.lo = lo;
    return r.v;
}

u64 __muldi3(u64 a, u64 b)
{
    U64 ua, ub, ur;
    ua.v = a; ub.v = b;
    ur.v      = mul32x32(ua.s.lo, ub.s.lo);
    ur.s.hi  += ua.s.hi * ub.s.lo + ua.s.lo * ub.s.hi;
    return ur.v;
}

static u64 udivmod64(u64 n, u64 d, u64 *rem)
{
    U64 un, ud, uq, ur;
    un.v = n; ud.v = d;
    if (ud.s.hi == 0 && un.s.hi == 0) {
        uq.s.hi = 0; uq.s.lo = un.s.lo / ud.s.lo;
        ur.s.hi = 0; ur.s.lo = un.s.lo % ud.s.lo;
        if (rem) *rem = ur.v;
        return uq.v;
    }
    uq.s.hi = uq.s.lo = 0;
    ur.s.hi = ur.s.lo = 0;
    for (int i = 63; i >= 0; i--) {
        ur.s.hi = (ur.s.hi << 1) | (ur.s.lo >> 31);
        ur.s.lo = (ur.s.lo << 1);
        if (i >= 32) ur.s.lo |= (un.s.hi >> (i - 32)) & 1u;
        else         ur.s.lo |= (un.s.lo >> i) & 1u;
        int ge = (ur.s.hi != ud.s.hi) ? (ur.s.hi > ud.s.hi) : (ur.s.lo >= ud.s.lo);
        if (ge) {
            if (ur.s.lo < ud.s.lo) ur.s.hi--;
            ur.s.lo -= ud.s.lo; ur.s.hi -= ud.s.hi;
            if (i >= 32) uq.s.hi |= 1u << (i - 32);
            else         uq.s.lo |= 1u << i;
        }
    }
    if (rem) *rem = ur.v;
    return uq.v;
}

u64 __udivdi3(u64 n, u64 d) { return udivmod64(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; udivmod64(n, d, &r); return r; }
