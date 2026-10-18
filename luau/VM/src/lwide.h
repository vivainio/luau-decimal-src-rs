// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include <stdint.h>
#include <string.h>

// Unsigned 256-bit integer arithmetic for the decimal library.
//
// A decimal has a mantissa of up to 90 bits. Multiplying two of them takes 180 bits, aligning scales multiplies by up to 10^28 (94 bits), and
// division scales the numerator by a power of ten before dividing, so exact intermediates need more than 128 bits. This header provides just
// what that takes: add, subtract, compare, multiply (keeping the low 256 bits), division with remainder, powers of ten and a decimal digit count.
//
// Numbers are stored as eight 32-bit limbs, least significant first. 32-bit limbs keep every intermediate product in a uint64_t, so the code
// needs no compiler extension (no __int128) and works with MSVC. Callers are responsible for not overflowing 256 bits; the functions do not
// detect it (add and multiply wrap).
//
// Everything here is checked against Python's arbitrary precision integers by demos/decimal/reference/wide-fuzz.py.

struct Wide
{
    uint32_t w[8];
};

static const int kWideLimbs = 8;
static const int kWideMaxPow10 = 77; // 10^77 < 2^256 < 10^78

static inline Wide wideZero()
{
    Wide r;
    memset(&r, 0, sizeof(r));
    return r;
}

static inline Wide wideFromU64(uint64_t v)
{
    Wide r = wideZero();
    r.w[0] = uint32_t(v);
    r.w[1] = uint32_t(v >> 32);
    return r;
}

static inline Wide wideFromU128(uint64_t lo, uint64_t hi)
{
    Wide r = wideFromU64(lo);
    r.w[2] = uint32_t(hi);
    r.w[3] = uint32_t(hi >> 32);
    return r;
}

static inline bool wideIsZero(const Wide& a)
{
    uint32_t x = 0;
    for (int i = 0; i < kWideLimbs; i++)
        x |= a.w[i];
    return x == 0;
}

// -1, 0 or 1
static inline int wideCmp(const Wide& a, const Wide& b)
{
    for (int i = kWideLimbs - 1; i >= 0; i--)
    {
        if (a.w[i] != b.w[i])
            return a.w[i] < b.w[i] ? -1 : 1;
    }
    return 0;
}

static inline Wide wideAdd(const Wide& a, const Wide& b)
{
    Wide r;
    uint64_t carry = 0;
    for (int i = 0; i < kWideLimbs; i++)
    {
        carry += uint64_t(a.w[i]) + b.w[i];
        r.w[i] = uint32_t(carry);
        carry >>= 32;
    }
    return r;
}

// requires a >= b
static inline Wide wideSub(const Wide& a, const Wide& b)
{
    Wide r;
    uint64_t borrow = 0;
    for (int i = 0; i < kWideLimbs; i++)
    {
        uint64_t t = uint64_t(a.w[i]) - b.w[i] - borrow;
        r.w[i] = uint32_t(t);
        borrow = (t >> 63) & 1; // the subtraction wrapped
    }
    return r;
}

// the low 256 bits of a * b
static inline Wide wideMul(const Wide& a, const Wide& b)
{
    Wide r = wideZero();
    for (int i = 0; i < kWideLimbs; i++)
    {
        if (a.w[i] == 0)
            continue;

        uint64_t carry = 0;
        for (int j = 0; i + j < kWideLimbs; j++)
        {
            uint64_t t = uint64_t(a.w[i]) * b.w[j] + r.w[i + j] + carry; // at most 2^64 - 1
            r.w[i + j] = uint32_t(t);
            carry = t >> 32;
        }
    }
    return r;
}

static inline Wide wideMulSmall(const Wide& a, uint32_t m)
{
    Wide r;
    uint64_t carry = 0;
    for (int i = 0; i < kWideLimbs; i++)
    {
        uint64_t t = uint64_t(a.w[i]) * m + carry;
        r.w[i] = uint32_t(t);
        carry = t >> 32;
    }
    return r;
}

// divides by a nonzero 32-bit divisor, returns the remainder
static inline Wide wideDivSmall(const Wide& a, uint32_t d, uint32_t* rem)
{
    Wide q = wideZero();
    uint64_t r = 0;

    int top = kWideLimbs - 1;
    while (top >= 0 && a.w[top] == 0)
        top--;

    for (int i = top; i >= 0; i--)
    {
        uint64_t cur = (r << 32) | a.w[i];
        q.w[i] = uint32_t(cur / d);
        r = cur % d;
    }
    *rem = uint32_t(r);
    return q;
}

static inline int wideClz32(uint32_t x)
{
    int n = 0;
    while ((x & 0x80000000u) == 0)
    {
        x <<= 1;
        n++;
    }
    return n;
}

// number of significant bits, 0 for zero
static inline int wideBitLength(const Wide& a)
{
    for (int i = kWideLimbs - 1; i >= 0; i--)
    {
        if (a.w[i] != 0)
            return i * 32 + 32 - wideClz32(a.w[i]);
    }
    return 0;
}

// Division with remainder, n = q * d + r with r < d. d must not be zero.
// This is Knuth's algorithm D (TAOCP 4.3.1) on 32-bit limbs, in the form given in Hacker's Delight (divmnu).
static inline void wideDivMod(const Wide& n, const Wide& d, Wide* q, Wide* r)
{
    int nn = kWideLimbs;
    while (nn > 0 && n.w[nn - 1] == 0)
        nn--;
    int dn = kWideLimbs;
    while (dn > 0 && d.w[dn - 1] == 0)
        dn--;

    *q = wideZero();
    *r = wideZero();

    if (nn < dn)
    {
        *r = n;
        return;
    }

    if (dn == 1)
    {
        uint32_t rem;
        *q = wideDivSmall(n, d.w[0], &rem);
        r->w[0] = rem;
        return;
    }

    const uint64_t base = uint64_t(1) << 32;

    // normalise so that the top bit of the divisor is set
    int s = wideClz32(d.w[dn - 1]);
    uint32_t vn[kWideLimbs];
    uint32_t un[kWideLimbs + 1];

    for (int i = dn - 1; i > 0; i--)
        vn[i] = (d.w[i] << s) | (s ? d.w[i - 1] >> (32 - s) : 0);
    vn[0] = d.w[0] << s;

    un[nn] = s ? n.w[nn - 1] >> (32 - s) : 0;
    for (int i = nn - 1; i > 0; i--)
        un[i] = (n.w[i] << s) | (s ? n.w[i - 1] >> (32 - s) : 0);
    un[0] = n.w[0] << s;

    for (int j = nn - dn; j >= 0; j--)
    {
        // estimate the quotient digit from the top two limbs, then correct it using the next limb
        uint64_t num = (uint64_t(un[j + dn]) << 32) | un[j + dn - 1];
        uint64_t qhat = num / vn[dn - 1];
        uint64_t rhat = num - qhat * vn[dn - 1];

        while (qhat >= base || qhat * vn[dn - 2] > ((rhat << 32) | un[j + dn - 2]))
        {
            qhat--;
            rhat += vn[dn - 1];
            if (rhat >= base)
                break;
        }

        // multiply and subtract
        int64_t borrow = 0;
        for (int i = 0; i < dn; i++)
        {
            uint64_t p = qhat * vn[i];
            int64_t t = int64_t(un[i + j]) - borrow - int64_t(p & 0xFFFFFFFFu);
            un[i + j] = uint32_t(t);
            borrow = int64_t(p >> 32) - (t >> 32);
        }
        int64_t t = int64_t(un[j + dn]) - borrow;
        un[j + dn] = uint32_t(t);

        q->w[j] = uint32_t(qhat);

        if (t < 0)
        {
            // the estimate was one too large (rare): add the divisor back
            q->w[j]--;
            uint64_t carry = 0;
            for (int i = 0; i < dn; i++)
            {
                carry += uint64_t(un[i + j]) + vn[i];
                un[i + j] = uint32_t(carry);
                carry >>= 32;
            }
            un[j + dn] += uint32_t(carry);
        }
    }

    // denormalise the remainder
    for (int i = 0; i < dn; i++)
        r->w[i] = (un[i] >> s) | (s ? un[i + 1] << (32 - s) : 0);
}

// 10^n for n in [0, 77]
struct WidePow10Table
{
    Wide v[kWideMaxPow10 + 1];

    WidePow10Table()
    {
        v[0] = wideFromU64(1);
        for (int i = 1; i <= kWideMaxPow10; i++)
            v[i] = wideMulSmall(v[i - 1], 10);
    }
};

static inline const Wide& widePow10(int n)
{
    static const WidePow10Table table;
    return table.v[n];
}

// number of decimal digits, 0 for zero
static inline int wideDigits(const Wide& a)
{
    int bits = wideBitLength(a);
    if (bits == 0)
        return 0;

    // floor(bits * log10(2)) is within one of the answer
    int n = (bits * 1233) >> 12;
    if (n > kWideMaxPow10 + 1)
        n = kWideMaxPow10 + 1;

    while (n <= kWideMaxPow10 && wideCmp(a, widePow10(n)) >= 0)
        n++;
    while (n > 0 && wideCmp(a, widePow10(n - 1)) < 0)
        n--;
    return n;
}
