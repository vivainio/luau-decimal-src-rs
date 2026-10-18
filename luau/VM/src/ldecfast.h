// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "lobject.h"
#include "lnumutils.h"

// Fast paths for decimals that work directly on the packed representation of a value slot (see luaO_setdec in lobject.h):
//   Value::l   the low 64 bits of the mantissa magnitude
//   extra[0]   bits 0-25: the high 26 bits of the magnitude, bits 26-30: the scale, bit 31: the sign
// They work on the magnitude and the sign, which avoids converting to and from a signed 128-bit mantissa, and they are exact: they return
// false for everything they cannot do exactly (different scales, a result that does not fit, a scale above 28) and the caller falls back to
// the general functions in ldeclib.cpp. Zero is never negative.

static const uint32_t kDecScaleMask = 0x7C000000u;
static const uint32_t kDecSignBit = 0x80000000u;

static inline uint32_t luaO_decbits(const TValue* o)
{
    return uint32_t(o->extra[0]);
}

static inline DecimalMagnitude luaO_decmagnitude(const TValue* o)
{
    return (DecimalMagnitude(luaO_decbits(o) & 0x3FFFFFFu) << 64) | uint64_t(o->value.l);
}

// stores a magnitude below 2^90 with the given sign and (already shifted) scale bits
static inline void luaO_setdecparts(TValue* o, DecimalMagnitude mag, bool negative, uint32_t scalebits)
{
    o->value.l = int64_t(uint64_t(mag));
    o->extra[0] = int(uint32_t(uint64_t(mag >> 64)) | scalebits | ((negative && mag != 0) ? kDecSignBit : 0u));
    o->tt = LUA_TDECIMAL;
}

static inline bool luaO_decsamescale(const TValue* a, const TValue* b)
{
    return ((luaO_decbits(a) ^ luaO_decbits(b)) & kDecScaleMask) == 0;
}

// ra = rb + rc, or rb - rc when subtracting; both must be decimals
static inline bool luaO_decaddsubfast(TValue* ra, const TValue* rb, const TValue* rc, bool subtract)
{
    if (!luaO_decsamescale(rb, rc))
        return false;

    DecimalMagnitude mb = luaO_decmagnitude(rb);
    DecimalMagnitude mc = luaO_decmagnitude(rc);
    bool nb = (luaO_decbits(rb) & kDecSignBit) != 0;
    bool nc = ((luaO_decbits(rc) & kDecSignBit) != 0) != subtract;

    DecimalMagnitude r;
    bool neg;
    if (nb == nc)
    {
        r = mb + mc;
        neg = nb;
        if ((r >> 90) != 0)
            return false;
    }
    else if (mb >= mc)
    {
        r = mb - mc;
        neg = nb;
    }
    else
    {
        r = mc - mb;
        neg = nc;
    }

    luaO_setdecparts(ra, r, neg, luaO_decbits(rb) & kDecScaleMask);
    return true;
}

// ra = rb * rc when the exact product fits and the scale stays in range; both must be decimals
static inline bool luaO_decmulfast(TValue* ra, const TValue* rb, const TValue* rc)
{
    uint32_t scale = ((luaO_decbits(rb) & kDecScaleMask) >> 26) + ((luaO_decbits(rc) & kDecScaleMask) >> 26);
    if (scale > 28)
        return false;

    DecimalMagnitude r;
    if (!decimalMagnitudeMultiply(luaO_decmagnitude(rb), luaO_decmagnitude(rc), &r) || (r >> 90) != 0)
        return false;

    bool neg = ((luaO_decbits(rb) ^ luaO_decbits(rc)) & kDecSignBit) != 0;
    luaO_setdecparts(ra, r, neg, scale << 26);
    return true;
}

// -1, 0 or 1 for two decimals with the same scale
static inline int luaO_deccmpsame(const TValue* a, const TValue* b)
{
    bool na = (luaO_decbits(a) & kDecSignBit) != 0;
    bool nb = (luaO_decbits(b) & kDecSignBit) != 0;
    if (na != nb)
        return na ? -1 : 1;

    DecimalMagnitude ma = luaO_decmagnitude(a);
    DecimalMagnitude mb = luaO_decmagnitude(b);
    int c = (ma > mb) - (ma < mb);
    return na ? -c : c;
}

// numeric equality of two decimals: with the same scale the bits are the value, otherwise compare numerically
static inline bool luaO_deceq(const TValue* a, const TValue* b)
{
    if (luaO_decsamescale(a, b))
        return a->value.l == b->value.l && luaO_decbits(a) == luaO_decbits(b);

    return luai_deccmp(decmantissa(a), decexponent(a), decmantissa(b), decexponent(b)) == 0;
}
