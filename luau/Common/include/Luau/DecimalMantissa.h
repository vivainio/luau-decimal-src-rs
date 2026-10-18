// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include <stdint.h>

// The mantissa of a decimal is a signed integer with a magnitude of at most 2^90 - 1 (27 to 28 decimal digits). It is carried through the
// parser, compiler and bytecode as a 128-bit integer; the decimal library does its arithmetic with the portable 256-bit helper in
// VM/src/lwide.h and only uses this type to hold values.
//
// GCC/Clang keep their native fast paths. MSVC uses the portable two-word type.
// LUAU_DECIMAL_PORTABLE also exercises that path on compilers with __int128.
#if defined(__SIZEOF_INT128__) && !defined(LUAU_DECIMAL_PORTABLE)
typedef __int128 DecimalMantissa;
typedef unsigned __int128 DecimalMagnitude;
#else
#include "Luau/Decimal128.h"
typedef Decimal128<true> DecimalMantissa;
typedef Decimal128<false> DecimalMagnitude;
#endif

// 2^90 - 1, the largest magnitude of a mantissa
static const DecimalMantissa kDecimalMaxMantissa = (DecimalMantissa(1) << 90) - 1;

// the exponent of a decimal is the negated scale (digits after the decimal point), always in [-28, 0]
static const int kDecimalMaxScale = 28;

// the number of decimal digits that are always representable (10^27 < 2^90 < 10^28)
static const int kDecimalDigits = 27;

// writes the decimal digits of a mantissa (with a leading '-' for negative values) and a terminating zero, returns the end;
// the buffer needs 41 bytes
static inline char* decimalMantissaToString(char* buf, DecimalMantissa m)
{
    DecimalMagnitude mag = m < 0 ? -DecimalMagnitude(m) : DecimalMagnitude(m);

    char tmp[40];
    int n = 0;
    do
    {
        tmp[n++] = char('0' + int(mag % 10));
        mag /= 10;
    } while (mag != 0);

    char* out = buf;
    if (m < 0)
        *out++ = '-';
    while (n > 0)
        *out++ = tmp[--n];
    *out = 0;
    return out;
}

// splits a mantissa into its sign and the two halves of its magnitude, and back
static inline void decimalMantissaSplit(DecimalMantissa m, bool* negative, uint64_t* hi, uint64_t* lo)
{
    DecimalMagnitude mag = m < 0 ? -DecimalMagnitude(m) : DecimalMagnitude(m);
    *negative = m < 0;
    *hi = uint64_t(mag >> 64);
    *lo = uint64_t(mag);
}

static inline DecimalMantissa decimalMantissaJoin(bool negative, uint64_t hi, uint64_t lo)
{
    DecimalMantissa mag = (DecimalMantissa(hi) << 64) | lo;
    return negative ? -mag : mag;
}

// Exact multiplication when the result fits the 90-bit decimal magnitude.
static inline bool decimalMagnitudeMultiply(DecimalMagnitude a, DecimalMagnitude b, DecimalMagnitude* result)
{
#if defined(__SIZEOF_INT128__) && !defined(LUAU_DECIMAL_PORTABLE)
    return !__builtin_mul_overflow(a, b, result) && (*result >> 90) == 0;
#else
    // 32-bit limbs keep products and carry propagation within uint64_t.
    uint64_t alo = uint64_t(a), ahi = uint64_t(a >> 64);
    uint64_t blo = uint64_t(b), bhi = uint64_t(b >> 64);
    uint32_t x[4] = {uint32_t(alo), uint32_t(alo >> 32), uint32_t(ahi), uint32_t(ahi >> 32)};
    uint32_t y[4] = {uint32_t(blo), uint32_t(blo >> 32), uint32_t(bhi), uint32_t(bhi >> 32)};
    uint32_t product[8] = {};
    for (int i = 0; i < 4; ++i)
    {
        uint64_t carry = 0;
        for (int j = 0; j < 4; ++j)
        {
            uint64_t value = uint64_t(x[i]) * y[j] + product[i + j] + carry;
            product[i + j] = uint32_t(value);
            carry = value >> 32;
        }
        product[i + 4] = uint32_t(carry);
    }
    if ((product[2] >> 26) || product[3] || product[4] || product[5] || product[6] || product[7])
        return false;
    *result = (DecimalMagnitude(product[2]) << 64) | (uint64_t(product[1]) << 32) | product[0];
    return true;
#endif
}

static inline bool decimalMantissaMultiply(DecimalMantissa a, DecimalMantissa b, DecimalMantissa* result)
{
#if defined(__SIZEOF_INT128__) && !defined(LUAU_DECIMAL_PORTABLE)
    // Keep the original signed native fast path: no sign/magnitude conversions.
    return !__builtin_mul_overflow(a, b, result) && *result <= kDecimalMaxMantissa && *result >= -kDecimalMaxMantissa;
#else
    bool negative = (a < 0) != (b < 0);
    DecimalMagnitude ma = a < 0 ? -DecimalMagnitude(a) : DecimalMagnitude(a);
    DecimalMagnitude mb = b < 0 ? -DecimalMagnitude(b) : DecimalMagnitude(b);
    DecimalMagnitude magnitude;
    if (!decimalMagnitudeMultiply(ma, mb, &magnitude))
        return false;
    DecimalMantissa value = DecimalMantissa(magnitude);
    *result = negative ? -value : value;
    return true;
#endif
}
