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

#include <cfloat>
#include <nlohmann/json.hpp>

#include "vector_type.h"

namespace EloqVec
{
static inline void sort_string(char *ptr, char *to, size_t len)
{
#if WORDS_BIGENDIAN
    std::memcpy(to, ptr, len);
#else
    // sign bit
    to[0] = (char) (ptr[len - 1] ^ 128);
    for (size_t i = 1; i < len; ++i)
    {
        to[i] = ptr[len - i - 1];
    }
#endif
}

#define DBL_EXP_DIG (sizeof(double) * 8 - DBL_MANT_DIG)
static inline void change_double_for_sort(double nr, char *to)
{
    unsigned char *tmp = (unsigned char *) to;
    if (nr == 0.0)
    {
        /* Change to zero string */
        tmp[0] = (unsigned char) 128;
        memset(tmp + 1, 0, sizeof(nr) - 1);
    }
    else
    {
#if WORDS_BIGENDIAN
        /* Big endian */
        std::memcpy(tmp, &nr, sizeof(nr));
#else
        /* Little endian */
        unsigned char *ptr = (unsigned char *) &nr;
#if defined(__FLOAT_WORD_ORDER) && (__FLOAT_WORD_ORDER == __BIG_ENDIAN)
        tmp[0] = ptr[3];
        tmp[1] = ptr[2];
        tmp[2] = ptr[1];
        tmp[3] = ptr[0];
        tmp[4] = ptr[7];
        tmp[5] = ptr[6];
        tmp[6] = ptr[5];
        tmp[7] = ptr[4];
#else
        tmp[0] = ptr[7];
        tmp[1] = ptr[6];
        tmp[2] = ptr[5];
        tmp[3] = ptr[4];
        tmp[4] = ptr[3];
        tmp[5] = ptr[2];
        tmp[6] = ptr[1];
        tmp[7] = ptr[0];
#endif
#endif

        if (tmp[0] & 128)
        {
            /* Negative */
            /* make complement */
            for (size_t i = 0; i < sizeof(nr); ++i)
            {
                tmp[i] = tmp[i] ^ (unsigned char) 255;
            }
        }
        else
        { /* Set high and move exponent one up */
            uint16_t exp_part = (((uint16_t) tmp[0] << 8) | (uint16_t) tmp[1] |
                                 (uint16_t) 32768);
            exp_part += (uint16_t) 1 << (16 - 1 - DBL_EXP_DIG);
            tmp[0] = (unsigned char) (exp_part >> 8);
            tmp[1] = (unsigned char) exp_part;
        }
    }
}

// Parse json field value to binary value based on field type
static inline bool ParseJSONFieldValue(const nlohmann::json &json_value,
                                       MetadataFieldType field_type,
                                       std::vector<char> &buf)
{
    switch (field_type)
    {
    case MetadataFieldType::Int32:
    {
        if (!json_value.is_number_integer())
        {
            return false;
        }
        int64_t val64 = json_value.get<int64_t>();
        if (val64 < std::numeric_limits<int32_t>::min() ||
            val64 > std::numeric_limits<int32_t>::max())
        {
            // Overflow
            return false;
        }
        int32_t val = static_cast<int32_t>(val64);
        char *val_ptr = reinterpret_cast<char *>(&val);
        size_t value_size = sizeof(int32_t);
        buf.resize(buf.size() + value_size);
        sort_string(val_ptr, buf.data() + buf.size() - value_size, value_size);
        break;
    }
    case MetadataFieldType::Int64:
    {
        if (!json_value.is_number_integer())
        {
            return false;
        }
        int64_t val = json_value.get<int64_t>();
        if (val < std::numeric_limits<int64_t>::min() ||
            val > std::numeric_limits<int64_t>::max())
        {
            // Overflow
            return false;
        }
        char *val_ptr = reinterpret_cast<char *>(&val);
        size_t value_size = sizeof(int64_t);
        buf.resize(buf.size() + value_size);
        sort_string(val_ptr, buf.data() + buf.size() - value_size, value_size);
        break;
    }
    case MetadataFieldType::Double:
    {
        if (!json_value.is_number_float())
        {
            return false;
        }
        double val = json_value.get<double>();
        size_t value_size = sizeof(double);
        buf.resize(buf.size() + value_size);
        change_double_for_sort(val, buf.data() + buf.size() - value_size);
        break;
    }
    case MetadataFieldType::Bool:
    {
        if (!json_value.is_boolean())
        {
            return false;
        }
        uint8_t val = json_value.get<bool>() ? 1 : 0;
        size_t value_size = sizeof(uint8_t);
        buf.resize(buf.size() + value_size);
        buf[buf.size() - value_size] = val;
        break;
    }
    case MetadataFieldType::String:
    {
        if (!json_value.is_string())
        {
            return false;
        }
        std::string str = json_value.get<std::string>();
        size_t str_len = str.size();
        char *str_len_ptr = reinterpret_cast<char *>(&str_len);
        size_t len_size = sizeof(size_t);
        buf.resize(buf.size() + len_size + str_len);
        std::memcpy(buf.data() + buf.size() - len_size - str_len,
                    str_len_ptr,
                    len_size);
        std::copy(str.begin(), str.end(), buf.data() + buf.size() - str_len);
        break;
    }
    default:
        return false;  // Unknown type
    }
    return true;
}

}  // namespace EloqVec