// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lualib.h"

#include "lcommon.h"
#include "lnumutils.h"
#include "lobject.h"
#include "lwide.h"

#include <climits>
#include <cmath>
#include <stdlib.h>
#include <string.h>

// Decimal values are stored as `mantissa * 10^exponent` with a signed mantissa of at most 90 bits (27 digits always fit, 28 up to 1.24e27)
// and an exponent that is the negated scale: it is always in [-28, 0], so the scale (number of digits after the decimal point) is in [0, 28],
// as in .NET.
//
// The scale is preserved: 2.50 has mantissa 250 and exponent -2, 2.5 has mantissa 25 and exponent -1. Equality and ordering are numeric.
// Arithmetic is computed exactly in 256 bits (see lwide.h) and then rounded half-even to the 90 bits that fit in the mantissa, which lowers
// the scale. Values that cannot be represented even at scale 0 raise an overflow error.

namespace
{

// sign and magnitude, which is what the rounding rules and the 256-bit helper work with
struct SignMagnitude
{
    Wide mag;
    bool neg;
};

}

static Wide maxMantissaWide()
{
    // 2^90 - 1
    Wide w = wideZero();
    w.w[0] = 0xFFFFFFFFu;
    w.w[1] = 0xFFFFFFFFu;
    w.w[2] = 0x03FFFFFFu;
    return w;
}

static SignMagnitude toSignMagnitude(DecimalMantissa m)
{
    bool neg;
    uint64_t hi, lo;
    decimalMantissaSplit(m, &neg, &hi, &lo);
    return {wideFromU128(lo, hi), neg};
}

// the magnitude must not exceed 2^90 - 1
static DecimalMantissa fromMagnitude(const Wide& mag, bool neg)
{
    uint64_t lo = uint64_t(mag.w[0]) | (uint64_t(mag.w[1]) << 32);
    uint64_t hi = uint64_t(mag.w[2]);
    return decimalMantissaJoin(neg, hi, lo);
}

// divides a by 10^drop, rounding half-even
static Wide dropDigits(const Wide& a, int drop)
{
    if (drop <= 0)
        return a;
    if (drop > kWideMaxPow10)
        return wideZero(); // a < 2^256 < 10^78, so a / 10^drop < 0.5

    const Wide& div = widePow10(drop);
    Wide q, r;
    wideDivMod(a, div, &q, &r);

    int c = wideCmp(wideAdd(r, r), div);
    if (c > 0 || (c == 0 && (q.w[0] & 1)))
        q = wideAdd(q, wideFromU64(1));
    return q;
}

// Reduces mag * 10^e to a magnitude that fits the mantissa and an exponent in [-28, 0], rounding half-even once, to the finest scale that
// makes it fit. Returns false if the value does not fit even at scale 0: an integer part that does not fit is an overflow, it is never
// rounded.
static bool reduce(Wide& mag, int& e)
{
    static const Wide maxm = maxMantissaWide();

    // the common case: already in range
    if (e <= 0 && e >= -kDecimalMaxScale && wideCmp(mag, maxm) <= 0)
        return true;

    if (wideIsZero(mag))
    {
        e = e > 0 ? 0 : (e < -kDecimalMaxScale ? -kDecimalMaxScale : e);
        return true;
    }

    // a positive exponent means an integer written in scientific notation: it fits only if the full number does
    if (e > 0)
    {
        if (e > kWideMaxPow10 || wideDigits(mag) + e > 28)
            return false;

        mag = wideMul(mag, widePow10(e));
        e = 0;
        return wideCmp(mag, maxm) <= 0;
    }

    // the finest allowed scale, and the scale at which the number has few enough digits (the largest mantissa has 28)
    int target = e < -kDecimalMaxScale ? -kDecimalMaxScale : e;
    int fit = e + (wideDigits(mag) - 28);
    if (fit > target)
        target = fit;

    for (;;)
    {
        if (target > 0)
            return false;

        Wide q = dropDigits(mag, target - e);
        if (wideCmp(q, maxm) <= 0)
        {
            mag = q;
            e = target;
            return true;
        }

        target++; // rounding up carried into a 29th digit, or the digit count estimate was one short
    }
}

// removes trailing zeros while the scale is above the preferred one; zeros come off in chunks, so a quotient with many of them (1/8 has
// 24) needs a handful of short divisions instead of one per digit
static void stripZeros(Wide& mag, int& e, int preferredScale)
{
    static const uint32_t pow10small[10] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

    int allow = -e - preferredScale;
    if (allow <= 0 || wideIsZero(mag))
        return;

    // 9, 4, 2 and 1 digit chunks are enough to take off any number of zeros below 18
    const int chunks[4] = {9, 4, 2, 1};
    for (int chunk : chunks)
    {
        while (allow >= chunk)
        {
            uint32_t rem;
            Wide q = wideDivSmall(mag, pow10small[chunk], &rem);
            if (rem != 0)
                break;

            mag = q;
            e += chunk;
            allow -= chunk;
        }
    }
}

static bool finish(Wide mag, bool neg, int e, DecimalMantissa* om, int* oe)
{
    if (!reduce(mag, e))
        return false;

    *om = fromMagnitude(mag, neg && !wideIsZero(mag));
    *oe = e;
    return true;
}

// a + b of two sign-magnitude values at the same exponent
static SignMagnitude addMagnitudes(const SignMagnitude& a, const SignMagnitude& b)
{
    if (a.neg == b.neg)
        return {wideAdd(a.mag, b.mag), a.neg};

    int c = wideCmp(a.mag, b.mag);
    if (c == 0)
        return {wideZero(), false};
    if (c > 0)
        return {wideSub(a.mag, b.mag), a.neg};
    return {wideSub(b.mag, a.mag), b.neg};
}

static bool addSub(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb, bool subtract, DecimalMantissa* om, int* oe)
{
    SignMagnitude a = toSignMagnitude(ma);
    SignMagnitude b = toSignMagnitude(mb);
    if (subtract)
        b.neg = !b.neg;

    // bring both to the larger scale; the exponents are in [-28, 0], so the factor is at most 10^28
    int e = ea < eb ? ea : eb;
    a.mag = wideMul(a.mag, widePow10(ea - e));
    b.mag = wideMul(b.mag, widePow10(eb - e));

    SignMagnitude r = addMagnitudes(a, b);
    return finish(r.mag, r.neg, e, om, oe);
}

bool luai_decadd(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb, DecimalMantissa* om, int* oe)
{
    return addSub(ma, ea, mb, eb, false, om, oe);
}

bool luai_decsub(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb, DecimalMantissa* om, int* oe)
{
    return addSub(ma, ea, mb, eb, true, om, oe);
}

bool luai_decmul(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb, DecimalMantissa* om, int* oe)
{
    SignMagnitude a = toSignMagnitude(ma);
    SignMagnitude b = toSignMagnitude(mb);
    return finish(wideMul(a.mag, b.mag), a.neg != b.neg, ea + eb, om, oe);
}

bool luai_decneg(DecimalMantissa m, int e, DecimalMantissa* om, int* oe)
{
    *om = -m;
    *oe = e;
    return true;
}

// the result has as many digits as fit, then trailing zeros are removed down to the preferred scale max(scale(a) - scale(b), 0)
int luai_decdiv(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb, DecimalMantissa* om, int* oe)
{
    SignMagnitude a = toSignMagnitude(ma);
    SignMagnitude b = toSignMagnitude(mb);

    if (wideIsZero(b.mag))
        return LUAI_DEC_DIVZERO;

    int s0 = eb - ea; // scale(a) - scale(b), since the exponent is the negated scale
    int preferred = s0 > 0 ? s0 : 0;

    if (wideIsZero(a.mag))
    {
        *om = 0;
        *oe = -preferred;
        return LUAI_DEC_OK;
    }

    static const Wide maxm = maxMantissaWide();
    static const Wide maxDiv10 = []
    {
        uint32_t rem;
        return wideDivSmall(maxMantissaWide(), 10, &rem);
    }();

    int na = wideDigits(a.mag);
    int nb = wideDigits(b.mag);

    // the quotient a / b * 10^k has scale s0 + k, which must be in [0, 28], and the scaled numerator must fit in 256 bits
    int kmin = s0 < 0 ? -s0 : 0;
    int kmax = kDecimalMaxScale - s0;
    if (kmax > kWideMaxPow10 - na)
        kmax = kWideMaxPow10 - na;

    if (kmin > kmax)
        return LUAI_DEC_OVERFLOW;

    // the rounded quotient grows with k, and we want the largest k for which it still fits in the mantissa; the digit counts give a close
    // estimate, so only a couple of 256-bit divisions are needed
    auto quotient = [&](int k, Wide* out)
    {
        Wide q, r;
        wideDivMod(wideMul(a.mag, widePow10(k)), b.mag, &q, &r);

        // round half-even using the remainder
        int c = wideCmp(wideAdd(r, r), b.mag);
        if (c > 0 || (c == 0 && (q.w[0] & 1)))
            q = wideAdd(q, wideFromU64(1));

        *out = q;
        return wideCmp(q, maxm) <= 0;
    };

    // aim for 27 digits, which fit unless the quotient starts with a high digit
    int k = 27 - (na - nb);
    k = k < kmin ? kmin : (k > kmax ? kmax : k);

    Wide q;
    while (!quotient(k, &q))
    {
        if (k == kmin)
            return LUAI_DEC_OVERFLOW;
        k--;
    }

    // a quotient above a tenth of the largest mantissa cannot fit one scale finer (it grows about tenfold), so only try if it is small enough
    while (k < kmax && wideCmp(q, maxDiv10) <= 0)
    {
        Wide next;
        if (!quotient(k + 1, &next))
            break;

        k++;
        q = next;
    }

    // a quotient that rounds to zero is plain zero, as in .NET (a zero dividend keeps the preferred scale, handled above)
    if (wideIsZero(q))
    {
        *om = 0;
        *oe = 0;
        return LUAI_DEC_OK;
    }

    int e = -(s0 + k);
    stripZeros(q, e, preferred);

    *om = fromMagnitude(q, (a.neg != b.neg) && !wideIsZero(q));
    *oe = e;
    return LUAI_DEC_OK;
}

// remainder with the sign of the dividend (truncated division), the scale is the larger of the two operand scales
int luai_decrem(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb, DecimalMantissa* om, int* oe)
{
    SignMagnitude a = toSignMagnitude(ma);
    SignMagnitude b = toSignMagnitude(mb);

    if (wideIsZero(b.mag))
        return LUAI_DEC_DIVZERO;

    int e = ea < eb ? ea : eb;
    Wide x = wideMul(a.mag, widePow10(ea - e));
    Wide y = wideMul(b.mag, widePow10(eb - e));

    Wide q, r;
    wideDivMod(x, y, &q, &r);

    return finish(r, a.neg, e, om, oe) ? LUAI_DEC_OK : LUAI_DEC_OVERFLOW;
}

int luai_deccmp(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb)
{
    int sa = (ma > 0) - (ma < 0);
    int sb = (mb > 0) - (mb < 0);
    if (sa != sb)
        return sa < sb ? -1 : 1;
    if (sa == 0)
        return 0;

    SignMagnitude a = toSignMagnitude(ma);
    SignMagnitude b = toSignMagnitude(mb);

    int e = ea < eb ? ea : eb;
    int r = wideCmp(wideMul(a.mag, widePow10(ea - e)), wideMul(b.mag, widePow10(eb - e)));

    return sa < 0 ? -r : r;
}

static void pushdec(lua_State* L, DecimalMantissa m, int e)
{
    bool neg;
    uint64_t hi, lo;
    decimalMantissaSplit(m, &neg, &hi, &lo);
    lua_pushdecimal(L, neg, hi, lo, e);
}

static void checkdecimalarg(lua_State* L, int narg, DecimalMantissa& m, int& e)
{
    int neg;
    uint64_t hi, lo;
    if (lua_todecimal(L, narg, &neg, &hi, &lo, &e))
    {
        m = decimalMantissaJoin(neg != 0, hi, lo);
        return;
    }

    if (lua_isinteger64(L, narg))
    {
        m = lua_tointeger64(L, narg, nullptr);
        e = 0;
        return;
    }

    luaL_typeerror(L, narg, "decimal");
}

static int decimal_binop(lua_State* L, bool (*op)(DecimalMantissa, int, DecimalMantissa, int, DecimalMantissa*, int*))
{
    DecimalMantissa ma, mb, om;
    int ea, eb, oe;
    checkdecimalarg(L, 1, ma, ea);
    checkdecimalarg(L, 2, mb, eb);

    if (!op(ma, ea, mb, eb, &om, &oe))
        luaL_error(L, "decimal overflow");

    pushdec(L, om, oe);
    return 1;
}

static int decimal_divop(lua_State* L, int (*op)(DecimalMantissa, int, DecimalMantissa, int, DecimalMantissa*, int*))
{
    DecimalMantissa ma, mb, om;
    int ea, eb, oe;
    checkdecimalarg(L, 1, ma, ea);
    checkdecimalarg(L, 2, mb, eb);

    switch (op(ma, ea, mb, eb, &om, &oe))
    {
    case LUAI_DEC_OK:
        pushdec(L, om, oe);
        return 1;
    case LUAI_DEC_DIVZERO:
        luaL_error(L, "decimal division by zero");
    default:
        luaL_error(L, "decimal overflow");
    }
}

static bool decimal_addfast(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb, DecimalMantissa* om, int* oe)
{
    return luai_decaddfast(ma, ea, mb, eb, om, oe) || luai_decadd(ma, ea, mb, eb, om, oe);
}

static bool decimal_subfast(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb, DecimalMantissa* om, int* oe)
{
    return luai_decsubfast(ma, ea, mb, eb, om, oe) || luai_decsub(ma, ea, mb, eb, om, oe);
}

static bool decimal_mulfast(DecimalMantissa ma, int ea, DecimalMantissa mb, int eb, DecimalMantissa* om, int* oe)
{
    return luai_decmulfast(ma, ea, mb, eb, om, oe) || luai_decmul(ma, ea, mb, eb, om, oe);
}

static int decimal_add(lua_State* L)
{
    return decimal_binop(L, decimal_addfast);
}

static int decimal_sub(lua_State* L)
{
    return decimal_binop(L, decimal_subfast);
}

static int decimal_mul(lua_State* L)
{
    return decimal_binop(L, decimal_mulfast);
}

static int decimal_div(lua_State* L)
{
    return decimal_divop(L, luai_decdiv);
}

static int decimal_rem(lua_State* L)
{
    return decimal_divop(L, luai_decrem);
}

static int decimal_neg(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);

    pushdec(L, -m, e);
    return 1;
}

static int decimal_abs(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);

    pushdec(L, m < 0 ? -m : m, e);
    return 1;
}

static int decimal_sign(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);

    lua_pushnumber(L, (m > 0) - (m < 0));
    return 1;
}

static int decimal_scale(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);

    lua_pushnumber(L, -e);
    return 1;
}

enum RoundMode
{
    HalfEven,
    AwayFromZero,
    ToZero,
    ToNegativeInfinity,
    ToPositiveInfinity,
};

// rounds to the given number of digits after the decimal point; values that already have at most that many are unchanged
static void roundto(lua_State* L, DecimalMantissa m, int e, int places, RoundMode mode)
{
    int target = -places;
    if (e >= target)
    {
        pushdec(L, m, e);
        return;
    }

    SignMagnitude a = toSignMagnitude(m);

    // the scale is at most 28, so this is at most 10^28
    const Wide& div = widePow10(target - e);
    Wide q, rem;
    wideDivMod(a.mag, div, &q, &rem);

    int half = wideCmp(wideAdd(rem, rem), div);

    bool up = false;
    switch (mode)
    {
    case HalfEven:
        up = half > 0 || (half == 0 && (q.w[0] & 1));
        break;
    case AwayFromZero:
        up = half >= 0;
        break;
    case ToZero:
        break;
    case ToNegativeInfinity:
        up = a.neg && !wideIsZero(rem);
        break;
    case ToPositiveInfinity:
        up = !a.neg && !wideIsZero(rem);
        break;
    }

    if (up)
        q = wideAdd(q, wideFromU64(1));

    DecimalMantissa om;
    int oe;
    if (!finish(q, a.neg, target, &om, &oe))
        luaL_error(L, "decimal overflow");

    pushdec(L, om, oe);
}

static int decimal_round(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);
    int places = luaL_checkinteger(L, 2);
    const char* mode = luaL_optstring(L, 3, "halfeven");

    luaL_argcheck(L, places >= 0 && places <= kDecimalMaxScale, 2, "number of places out of range [0, 28]");

    RoundMode rounding;
    if (strcmp(mode, "halfeven") == 0)
        rounding = HalfEven;
    else if (strcmp(mode, "awayfromzero") == 0)
        rounding = AwayFromZero;
    else if (strcmp(mode, "tozero") == 0)
        rounding = ToZero;
    else if (strcmp(mode, "tonegativeinfinity") == 0)
        rounding = ToNegativeInfinity;
    else if (strcmp(mode, "topositiveinfinity") == 0)
        rounding = ToPositiveInfinity;
    else
        luaL_argerror(L, 3, "invalid rounding mode, expected 'halfeven', 'awayfromzero', 'tozero', 'tonegativeinfinity' or 'topositiveinfinity'");

    roundto(L, m, e, places, rounding);
    return 1;
}

static int decimal_floor(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);
    roundto(L, m, e, 0, ToNegativeInfinity);
    return 1;
}

static int decimal_ceil(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);
    roundto(L, m, e, 0, ToPositiveInfinity);
    return 1;
}

static int decimal_trunc(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);
    roundto(L, m, e, 0, ToZero);
    return 1;
}

static int decimal_minmax(lua_State* L, bool wantMax)
{
    int n = lua_gettop(L);

    DecimalMantissa bm;
    int be;
    checkdecimalarg(L, 1, bm, be);

    for (int i = 2; i <= n; i++)
    {
        DecimalMantissa m;
        int e;
        checkdecimalarg(L, i, m, e);

        int c = luai_deccmp(m, e, bm, be);
        // for equal values max keeps the earlier argument and min takes the later one, like Math.Max and Math.Min in .NET
        if (wantMax ? c > 0 : c <= 0)
        {
            bm = m;
            be = e;
        }
    }

    pushdec(L, bm, be);
    return 1;
}

static int decimal_min(lua_State* L)
{
    return decimal_minmax(L, false);
}

static int decimal_max(lua_State* L)
{
    return decimal_minmax(L, true);
}

static int decimal_lt(lua_State* L)
{
    DecimalMantissa ma, mb;
    int ea, eb;
    checkdecimalarg(L, 1, ma, ea);
    checkdecimalarg(L, 2, mb, eb);

    lua_pushboolean(L, luai_deccmp(ma, ea, mb, eb) < 0);
    return 1;
}

static int decimal_le(lua_State* L)
{
    DecimalMantissa ma, mb;
    int ea, eb;
    checkdecimalarg(L, 1, ma, ea);
    checkdecimalarg(L, 2, mb, eb);

    lua_pushboolean(L, luai_deccmp(ma, ea, mb, eb) <= 0);
    return 1;
}

static int decimal_tostring(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);

    char s[LUAI_MAXDEC2STR];
    char* end = luai_dec2str(s, m, e);
    lua_pushlstring(L, s, end - s);
    return 1;
}

static int decimal_tonumber(lua_State* L)
{
    DecimalMantissa m;
    int e;
    checkdecimalarg(L, 1, m, e);

    char s[LUAI_MAXDEC2STR];
    luai_dec2str(s, m, e);
    lua_pushnumber(L, luai_str2num(s, nullptr));
    return 1;
}

// accepts [+-]digits[.digits][(e|E)[+-]digits]; trailing zeros after the decimal point are kept; surrounding whitespace is allowed
static bool parsedec(const char* s, size_t n, Wide& mag, bool& neg, int& e)
{
    // surrounding whitespace is allowed, as in .NET's decimal.Parse
    while (n > 0 && (s[n - 1] == ' ' || (s[n - 1] >= '\t' && s[n - 1] <= '\r')))
        n--;

    size_t i = 0;
    while (i < n && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r')))
        i++;

    neg = false;
    if (i < n && (s[i] == '-' || s[i] == '+'))
        neg = s[i++] == '-';

    // digits beyond this are far below the precision we keep
    const Wide& cap = widePow10(70);

    mag = wideZero();
    e = 0;
    bool any = false, dot = false;

    for (; i < n; i++)
    {
        char c = s[i];
        if (c >= '0' && c <= '9')
        {
            any = true;
            if (wideCmp(mag, cap) < 0)
            {
                mag = wideAdd(wideMulSmall(mag, 10), wideFromU64(uint64_t(c - '0')));
                if (dot)
                    e--;
            }
            else if (!dot)
            {
                e++;
            }
        }
        else if (c == '.' && !dot)
        {
            dot = true;
        }
        else
        {
            break;
        }
    }

    if (!any)
        return false;

    if (i < n && (s[i] == 'e' || s[i] == 'E'))
    {
        i++;
        bool eneg = false;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            eneg = s[i++] == '-';

        if (i >= n)
            return false;

        long ex = 0;
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        {
            if (ex < 100000)
                ex = ex * 10 + (s[i] - '0');
        }

        e += int(eneg ? -ex : ex);
    }

    return i == n;
}

static int pushparsed(lua_State* L, const char* s, size_t n)
{
    Wide mag;
    bool neg;
    int e;
    DecimalMantissa om;
    int oe;

    if (!parsedec(s, n, mag, neg, e) || !finish(mag, neg, e, &om, &oe))
    {
        lua_pushnil(L);
        return 1;
    }

    pushdec(L, om, oe);
    return 1;
}

static int decimal_fromstring(lua_State* L)
{
    size_t n;
    const char* s = luaL_checklstring(L, 1, &n);
    return pushparsed(L, s, n);
}

static int decimal_create(lua_State* L)
{
    switch (lua_type(L, 1))
    {
    case LUA_TDECIMAL:
        lua_pushvalue(L, 1);
        return 1;
    case LUA_TINTEGER:
        pushdec(L, lua_tointeger64(L, 1, nullptr), 0);
        return 1;
    case LUA_TNUMBER:
    {
        double v = lua_tonumber(L, 1);
        if (!std::isfinite(v))
        {
            lua_pushnil(L);
            return 1;
        }

        // shortest representation that round-trips, so decimal.create(0.1) is exactly 0.1
        char s[LUAI_MAXNUM2STR];
        char* end = luai_num2str(s, v);
        return pushparsed(L, s, end - s);
    }
    case LUA_TSTRING:
    {
        size_t n;
        const char* s = lua_tolstring(L, 1, &n);
        return pushparsed(L, s, n);
    }
    default:
        luaL_typeerror(L, 1, "number, integer, string or decimal");
    }
}

static const luaL_Reg declib[] = {
    {"create", decimal_create},
    {"fromstring", decimal_fromstring},
    {"tostring", decimal_tostring},
    {"tonumber", decimal_tonumber},
    {"neg", decimal_neg},
    {"abs", decimal_abs},
    {"sign", decimal_sign},
    {"scale", decimal_scale},
    {"add", decimal_add},
    {"sub", decimal_sub},
    {"mul", decimal_mul},
    {"div", decimal_div},
    {"rem", decimal_rem},
    {"round", decimal_round},
    {"floor", decimal_floor},
    {"ceil", decimal_ceil},
    {"trunc", decimal_trunc},
    {"min", decimal_min},
    {"max", decimal_max},
    {"lt", decimal_lt},
    {"le", decimal_le},
    {NULL, NULL},
};

int luaopen_decimal(lua_State* L)
{
    luaL_register(L, LUA_DECLIBNAME, declib);
    return 1;
}
