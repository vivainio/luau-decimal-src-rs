// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include <stdint.h>
#include <type_traits>

// Minimal two-word integer used when the compiler has no native 128-bit integer.
// Keep this trivially default-constructible/copyable: decimal constants live in unions.
// Arithmetic wraps at 128 bits. Decimal callers bound magnitudes to 90 bits.
template<bool Signed>
struct Decimal128
{
    uint64_t lo;
    uint64_t hi;

    Decimal128() = default;

    template<typename T, typename = typename std::enable_if<std::is_integral<T>::value>::type>
    constexpr Decimal128(T value)
        : lo(uint64_t(value))
        , hi(std::is_signed<T>::value && value < 0 ? ~uint64_t(0) : 0)
    {
    }

    template<bool OtherSigned>
    explicit constexpr Decimal128(Decimal128<OtherSigned> value)
        : lo(value.lo)
        , hi(value.hi)
    {
    }

    static Decimal128 words(uint64_t low, uint64_t high)
    {
        Decimal128 result;
        result.lo = low;
        result.hi = high;
        return result;
    }

    explicit operator uint64_t() const { return lo; }
    explicit operator int() const { return int(lo); }

    friend bool operator==(Decimal128 a, Decimal128 b) { return a.lo == b.lo && a.hi == b.hi; }
    friend bool operator!=(Decimal128 a, Decimal128 b) { return !(a == b); }
    friend bool operator<(Decimal128 a, Decimal128 b)
    {
        if (Signed && (a.hi >> 63) != (b.hi >> 63))
            return (a.hi >> 63) != 0;
        return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
    }
    friend bool operator>(Decimal128 a, Decimal128 b) { return b < a; }
    friend bool operator<=(Decimal128 a, Decimal128 b) { return !(b < a); }
    friend bool operator>=(Decimal128 a, Decimal128 b) { return !(a < b); }

    friend Decimal128 operator+(Decimal128 a, Decimal128 b)
    {
        uint64_t low = a.lo + b.lo;
        return words(low, a.hi + b.hi + (low < a.lo));
    }
    friend Decimal128 operator-(Decimal128 a, Decimal128 b)
    {
        return words(a.lo - b.lo, a.hi - b.hi - (a.lo < b.lo));
    }
    Decimal128 operator-() const { return words(0, 0) - *this; }
    friend Decimal128 operator|(Decimal128 a, Decimal128 b) { return words(a.lo | b.lo, a.hi | b.hi); }
    friend Decimal128 operator<<(Decimal128 a, unsigned shift)
    {
        if (shift >= 128)
            return words(0, 0);
        if (shift >= 64)
            return words(0, a.lo << (shift - 64));
        return shift == 0 ? a : words(a.lo << shift, (a.hi << shift) | (a.lo >> (64 - shift)));
    }
    friend Decimal128 operator>>(Decimal128 a, unsigned shift)
    {
        // Only unsigned magnitudes are right-shifted by decimal callers.
        if (shift >= 128)
            return words(0, 0);
        if (shift >= 64)
            return words(a.hi >> (shift - 64), 0);
        return shift == 0 ? a : words((a.lo >> shift) | (a.hi << (64 - shift)), a.hi >> shift);
    }

    friend Decimal128 operator*(Decimal128 a, uint32_t b)
    {
        uint64_t p0 = uint64_t(uint32_t(a.lo)) * b;
        uint64_t p1 = uint64_t(uint32_t(a.lo >> 32)) * b + (p0 >> 32);
        return words((p1 << 32) | uint32_t(p0), a.hi * b + (p1 >> 32));
    }
    Decimal128& operator*=(uint32_t b) { return *this = *this * b; }

    static Decimal128 divide(Decimal128 a, uint32_t divisor, uint32_t* remainder)
    {
        bool negative = Signed && (a.hi >> 63) != 0;
        if (negative)
            a = -a;
        uint32_t limbs[4] = {uint32_t(a.lo), uint32_t(a.lo >> 32), uint32_t(a.hi), uint32_t(a.hi >> 32)};
        uint64_t carry = 0;
        for (int i = 3; i >= 0; --i)
        {
            uint64_t value = (carry << 32) | limbs[i];
            limbs[i] = uint32_t(value / divisor);
            carry = value % divisor;
        }
        *remainder = uint32_t(carry);
        Decimal128 quotient = words(uint64_t(limbs[0]) | (uint64_t(limbs[1]) << 32), uint64_t(limbs[2]) | (uint64_t(limbs[3]) << 32));
        return negative ? -quotient : quotient;
    }
    friend Decimal128 operator/(Decimal128 a, uint32_t b)
    {
        uint32_t remainder;
        return divide(a, b, &remainder);
    }
    friend int64_t operator%(Decimal128 a, uint32_t b)
    {
        uint32_t remainder;
        divide(a, b, &remainder);
        return Signed && (a.hi >> 63) != 0 ? -int64_t(remainder) : int64_t(remainder);
    }
    Decimal128& operator/=(uint32_t b) { return *this = *this / b; }
};

static_assert(std::is_trivial<Decimal128<true>>::value, "decimal storage must remain trivial");
static_assert(sizeof(Decimal128<true>) == 16, "decimal storage must remain 128 bits");
