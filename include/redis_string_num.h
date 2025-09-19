/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under either of the following two licenses:
 *    1. GNU Affero General Public License, version 3, as published by the Free
 *    Software Foundation.
 *    2. GNU General Public License as published by the Free Software
 *    Foundation; version 2 of the License.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License or GNU General Public License for more
 *    details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    and GNU General Public License V2 along with this program.  If not, see
 *    <http://www.gnu.org/licenses/>.
 *
 */
#pragma once
#include <limits.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

extern "C"
{
#include "fpconv/fpconv_dtoa.h"
}

/**
 * In this file, it will realize the conversition from string to numeric or
 * numeric to string
 */

#define LONG_STR_SIZE 21

namespace EloqKV
{
/* Convert a string into a long long. Returns 1 if the string could be parsed
 * into a (non-overflowing) long long, 0 otherwise. The value will be set to
 * the parsed value when appropriate.
 *
 * Note that this function demands that the string strictly represents
 * a long long: no spaces or other characters before or after the string
 * representing the number are accepted, nor zeroes at the start if not
 * for the string "0" representing the zero number.
 *
 * Because of its strictness, it is safe to use this function to check if
 * you can convert a string into a long long, and obtain back the string
 * from the number without any loss in the string representation. */
static inline bool string2ll(const char *s, size_t slen, int64_t &value)
{
    const char *p = s;
    size_t plen = 0;
    int negative = 0;
    uint64_t v;

    /* A string of zero length or excessive length is not a valid number. */
    if (plen == slen || slen >= LONG_STR_SIZE)
        return 0;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0')
    {
        value = 0;
        return 1;
    }

    /* Handle negative numbers: just set a flag and continue like if it
     * was a positive number. Later convert into negative. */
    if (p[0] == '-')
    {
        negative = 1;
        p++;
        plen++;

        /* Abort on only a negative sign. */
        if (plen == slen)
            return 0;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9')
    {
        v = p[0] - '0';
        p++;
        plen++;
    }
    else
    {
        return 0;
    }

    /* Parse all the other digits, checking for overflow at every step. */
    while (plen < slen && p[0] >= '0' && p[0] <= '9')
    {
        if (v > (ULLONG_MAX / 10)) /* Overflow. */
            return 0;
        v *= 10;

        if (v > (ULLONG_MAX - (p[0] - '0'))) /* Overflow. */
            return 0;
        v += p[0] - '0';

        p++;
        plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen)
        return 0;

    /* Convert to negative if needed, and do the final overflow check when
     * converting from unsigned long long to long long. */
    if (negative)
    {
        if (v > ((unsigned long long) (-(LLONG_MIN + 1)) + 1)) /* Overflow. */
            return 0;
        value = -v;
    }
    else
    {
        if (v > LLONG_MAX) /* Overflow. */
            return 0;

        value = v;
    }
    return 1;
}

/* Helper function to convert a string to an unsigned long long value.
 * The function attempts to use the faster string2ll() function inside
 * Redis: if it fails, strtoull() is used instead. The function returns
 * 1 if the conversion happened successfully or 0 if the number is
 * invalid or out of range. */
static inline bool string2ull(const char *s, size_t slen, uint64_t &value)
{
    int64_t ll;
    if (string2ll(s, slen, ll))
    {
        if (ll < 0)
            return 0; /* Negative values are out of range. */
        value = ll;
        return 1;
    }
    errno = 0;
    char *endptr = NULL;
    value = strtoull(s, &endptr, 10);
    if (errno == EINVAL || errno == ERANGE || !(*s != '\0' && *endptr == '\0'))
        return 0; /* strtoull() failed. */
    return 1;     /* Conversion done! */
}

/**
 * This function will convert a string into double with fixed size.
 * This implementation is temporary and will change in following time.
 */
/* Convert a string into a double. Returns true if the string could be parsed
 * into a (non-overflowing) double, false otherwise. The value will be set to
 * the parsed value when appropriate.
 *
 * Note that this function demands that the string strictly represents
 * a double: no spaces or other characters before or after the string
 * representing the number are accepted. */
static inline bool string2double(const char *s, size_t slen, double &value)
{
    std::string str(s, slen);

    if (str == "-inf")
    {
        value = std::numeric_limits<double>::infinity() * -1;
    }
    else if (str == "+inf")
    {
        value = std::numeric_limits<double>::infinity();
    }
    else
    {
        const char *ptr = str.c_str();
        char *eptr;

        value = strtod(ptr, &eptr);
        if (slen == 0 || isspace(s[0]) ||
            static_cast<size_t>(eptr - ptr) != slen || std::isnan(value))
            return false;
    }
    return true;
}

/**
 * This function will convert a string into a float with fixed size.
 */
static inline bool string2float(const char *s, size_t slen, float &value)
{
    std::string str(s, slen);
    if (str == "-inf")
    {
        value = std::numeric_limits<float>::infinity() * -1;
    }
    else if (str == "+inf")
    {
        value = std::numeric_limits<float>::infinity();
    }
    else
    {
        const char *ptr = str.c_str();
        char *eptr;
        value = strtof(ptr, &eptr);
        if (slen == 0 || isspace(s[0]) ||
            static_cast<size_t>(eptr - ptr) != slen || std::isnan(value))
            return false;
    }
    return true;
}

/**
 * Convert double to string
 */
static inline std::string d2string(double value)
{
    if (std::isnan(value))
    {
        /* Libc in some systems will format nan in a different way,
         * like nan, -nan, NAN, nan(char-sequence).
         * So we normalize it and create a single nan form in an explicit way.
         */
        return "nan";
    }
    else if (std::isinf(value))
    {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        if (value < 0)
        {
            return "-inf";
        }
        else
        {
            return "inf";
        }
    }
    else if (value == 0)
    {
        return "0";
    }
    else
    {
        char buf[24];
        int len = fpconv_dtoa(value, buf);
        return std::string(buf, len);
    }
}

/* Convert a string into a double. Returns 1 if the string could be parsed
 * into a (non-overflowing) double, 0 otherwise. The value will be set to
 * the parsed value when appropriate.
 *
 * Note that this function demands that the string strictly represents
 * a double: no spaces or other characters before or after the string
 * representing the number are accepted. */
static inline bool string2ld(const char *s, size_t slen, long double &dp)
{
    const size_t MAX_LONG_DOUBLE_CHARS = 5 * 1024;
    char buf[MAX_LONG_DOUBLE_CHARS];
    long double value;
    char *eptr;

    if (slen == 0 || slen >= sizeof(buf))
        return 0;
    memcpy(buf, s, slen);
    buf[slen] = '\0';

    errno = 0;
    value = strtold(buf, &eptr);
    if (isspace(buf[0]) || eptr[0] != '\0' ||
        static_cast<size_t>(eptr - buf) != slen ||
        (errno == ERANGE && (value == HUGE_VAL || value == -HUGE_VAL ||
                             fpclassify(value) == FP_ZERO)) ||
        errno == EINVAL || isnan(value))
        return false;

    dp = value;
    return true;
}

/**
 * Convert long double to string
 */
static inline std::string ld2string(long double value)
{
    if (std::isnan(value))
    {
        /* Libc in some systems will format nan in a different way,
         * like nan, -nan, NAN, nan(char-sequence).
         * So we normalize it and create a single nan form in an explicit way.
         */
        return "nan";
    }
    else if (std::isinf(value))
    {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        if (value < 0)
        {
            return "-inf";
        }
        else
        {
            return "inf";
        }
    }
    else if (value == 0)
    {
        return "0";
    }
    else
    {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(17) << value;
        const std::string &str = ss.str();
        size_t pos;
        if (str.find('.') == std::string::npos)
        {
            pos = str.size();
        }
        else
        {
            pos = str.find_last_not_of('0');
            if (pos == std::string::npos)
            {
                pos = 1;
            }
            else if (str[pos] != '.')
            {
                pos++;
            }
        }
        // offset used to resolve -0.0, for example: value=-0.000000000000000001
        size_t offset = (pos == 2 && str[0] == '-' && str[1] == '0') ? 1 : 0;
        return std::string(str.data() + offset, pos - offset);
    }
}

static inline bool will_addition_overflow(int64_t a, int64_t b)
{
    // Check for positive overflow
    if (a > 0 && b > 0 && a > (std::numeric_limits<int64_t>::max() - b))
    {
        return true;  // Positive overflow
    }
    // Check for negative overflow
    if (a < 0 && b < 0 && a < (std::numeric_limits<int64_t>::min() - b))
    {
        return true;  // Negative overflow
    }
    return false;  // No overflow
}

static inline bool will_addition_overflow(uint64_t a, uint64_t b)
{
    // Check for positive overflow
    if (a > 0 && b > 0 && a > (std::numeric_limits<uint64_t>::max() - b))
    {
        return true;  // Positive overflow
    }
    return false;  // No overflow
}

static inline bool will_multiplication_overflow(int64_t a, int64_t b)
{
    // Handle zero cases (multiplication by zero never overflows)
    if (a == 0 || b == 0)
    {
        return false;
    }

    // Check for overflow without performing the multiplication
    if (a > 0 && b > 0)
    {
        return a > (std::numeric_limits<int64_t>::max() / b);
    }
    if (a > 0 && b < 0)
    {
        return b < (std::numeric_limits<int64_t>::min() / a);
    }
    if (a < 0 && b > 0)
    {
        return a < (std::numeric_limits<int64_t>::min() / b);
    }
    if (a < 0 && b < 0)
    {
        return a < (std::numeric_limits<int64_t>::max() / b);
    }

    return false;  // Should never reach here
}

static inline bool will_multiplication_overflow(uint64_t a, uint64_t b)
{
    // Handle zero cases
    if (a == 0 || b == 0)
    {
        return false;
    }

    // Check for overflow without performing the multiplication
    return a > (std::numeric_limits<uint64_t>::max() / b);
}

}  // namespace EloqKV
