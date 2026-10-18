// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include <stdint.h>

// The mantissa of a decimal is a signed integer with a magnitude of at most 2^90 - 1 (27 to 28 decimal digits). It is carried through the
// parser, compiler and bytecode as a 128-bit integer; the decimal library does its arithmetic with the portable 256-bit helper in
// VM/src/lwide.h and only uses this type to hold values.
//
// TODO: __int128 is a GCC and Clang extension. Replacing this typedef with a small struct (and the handful of operators used on it) is all it
// takes to support MSVC.
#if !defined(__SIZEOF_INT128__)
#error "decimal values currently require __int128"
#endif

typedef __int128 DecimalMantissa;
typedef unsigned __int128 DecimalMagnitude;

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
    unsigned __int128 mag = m < 0 ? -(unsigned __int128)m : (unsigned __int128)m;

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
    unsigned __int128 mag = m < 0 ? -(unsigned __int128)m : (unsigned __int128)m;
    *negative = m < 0;
    *hi = uint64_t(mag >> 64);
    *lo = uint64_t(mag);
}

static inline DecimalMantissa decimalMantissaJoin(bool negative, uint64_t hi, uint64_t lo)
{
    DecimalMantissa mag = (DecimalMantissa(hi) << 64) | lo;
    return negative ? -mag : mag;
}
