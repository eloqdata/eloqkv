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
#include "redis_string_object.h"

#include <mimalloc.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

#include "eloq_string.h"
#include "redis_command.h"
#include "redis_errors.h"
#include "redis_string_num.h"
#include "tx_service/include/tx_record.h"

#define BITS_LEN 8

namespace EloqKV
{
void RedisStringObject::Execute(GetCommand &cmd) const
{
    RedisStringResult &result = cmd.result_;

    result.err_code_ = RD_OK;
    result.str_ = str_obj_.StringView();
}

void RedisStringObject::Execute(GetDelCommand &cmd) const
{
    RedisStringResult &result = cmd.result_;

    result.err_code_ = RD_OK;
    result.str_ = str_obj_.StringView();
}

bool RedisStringObject::Execute(IntOpCommand &cmd) const
{
    RedisIntResult &result = cmd.result_;
    const std::string_view &sv = StringView();

    int64_t old_val = 0;
    if (sv.length() == 0)
    {
        old_val = 0;
    }
    else if (!string2ll(sv.data(), sv.size(), old_val))
    {
        result.err_code_ = RD_ERR_DIGITAL_INVALID;
        return false;
    }

    int64_t incr = cmd.incr_;

    if ((incr < 0 && old_val <= 0 && incr <= (LLONG_MIN - old_val)) ||
        (incr > 0 && old_val >= 0 && incr >= (LLONG_MAX - old_val)))
    {
        result.err_code_ = RD_ERR_INCR_OVERFLOW;
        return false;
    }

    result.int_val_ = old_val + incr;
    result.err_code_ = RD_OK;
    return true;
}

bool RedisStringObject::Execute(FloatOpCommand &cmd) const
{
    RedisStringResult &result = cmd.result_;
    const std::string_view &sv = StringView();

    long double old_val = 0;
    if (sv.length() == 0)
    {
        old_val = 0;
    }
    else if (!string2ld(sv.data(), sv.size(), old_val))
    {
        result.err_code_ = RD_ERR_FLOAT_VALUE;
        return false;
    }

    old_val += cmd.incr_;
    if (isnan(old_val) || isinf(old_val))
    {
        result.err_code_ = RD_ERR_INCR_NAN_OR_INFINITY;
        return false;
    }

    result.str_ = ld2string(old_val);
    result.err_code_ = RD_OK;
    return true;
}

void RedisStringObject::Execute(StrLenCommand &cmd) const
{
    RedisStringResult &result = cmd.result_;
    result.err_code_ = RD_OK;
    result.int_ret_ = str_obj_.StringView().size();
}

void RedisStringObject::Execute(GetBitCommand &cmd) const
{
    RedisStringResult &result = cmd.result_;

    result.err_code_ = RD_OK;
    size_t by_pos = cmd.offset_ / BITS_LEN;
    size_t bit_pos = 7 - cmd.offset_ % BITS_LEN;

    if (by_pos >= str_obj_.StringView().size())
    {
        result.int_ret_ = 0;
    }
    else
    {
        char c = str_obj_.StringView().data()[by_pos];
        result.int_ret_ = (c >> bit_pos) & 0x1;
    }
}

void RedisStringObject::Execute(GetRangeCommand &cmd) const
{
    RedisStringResult &result = cmd.result_;

    result.err_code_ = RD_OK;
    if (cmd.start_ < 0)
    {
        cmd.start_ = str_obj_.Length() + cmd.start_;
        if (cmd.start_ < 0)
        {
            cmd.start_ = 0;
        }
    }
    else if (cmd.start_ > str_obj_.Length())
    {
        cmd.start_ = str_obj_.Length();
    }

    if (cmd.end_ < 0)
    {
        cmd.end_ = str_obj_.Length() + cmd.end_ + 1;
    }
    else if (cmd.end_ >= str_obj_.Length())
    {
        cmd.end_ = str_obj_.Length();
    }
    else
    {
        cmd.end_++;
    }

    if (cmd.start_ < cmd.end_)
    {
        auto ptr = str_obj_.Data();
        result.str_ = std::string(ptr + cmd.start_, ptr + cmd.end_);
    }
}

bool RedisStringObject::Execute(SetBitCommand &cmd) const
{
    RedisStringResult &result = cmd.result_;
    result.err_code_ = RD_OK;
    size_t by_pos = cmd.offset_ / BITS_LEN;
    size_t bit_pos = 7 - cmd.offset_ % BITS_LEN;

    if (by_pos >= str_obj_.StringView().size())
    {
        if (!CheckSerializedLength(by_pos + 1))
        {
            result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return false;
        }
        result.int_ret_ = 0;
    }
    else
    {
        char c = str_obj_.StringView()[by_pos];
        result.int_ret_ = (c >> bit_pos) & 0x1;
    }
    return true;
}

bool RedisStringObject::Execute(SetRangeCommand &cmd) const
{
    RedisStringResult &result = cmd.result_;
    result.err_code_ = RD_OK;

    if (cmd.offset_ + cmd.value_.Length() <=
        static_cast<int64_t>(str_obj_.Length()))
    {
        result.int_ret_ = str_obj_.Length();
    }
    else
    {
        if (!CheckSerializedLength(cmd.offset_ + cmd.value_.Length()))
        {
            result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return false;
        }
        result.int_ret_ = cmd.offset_ + cmd.value_.Length();
    }
    return true;
}

bool RedisStringObject::Execute(AppendCommand &cmd) const
{
    if (SerializedLength() + cmd.value_.Length() > MAX_OBJECT_SIZE)
    {
        cmd.result_.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }
    cmd.result_.err_code_ = RD_OK;
    cmd.result_.int_ret_ = cmd.value_.Length() + str_obj_.Length();
    return true;
}

void RedisStringObject::Execute(BitCountCommand &cmd) const
{
    cmd.result_.err_code_ = RD_OK;
    cmd.result_.int_ret_ = 0;

    int64_t start_offset = 0;
    int64_t end_offset = 0;

    int64_t step = cmd.IsOffsetByBit() ? 8 : 1;
    int64_t total_step = str_obj_.Length() * step;

    if (cmd.start_ < 0)
    {
        start_offset = total_step + cmd.start_;
        start_offset = start_offset < 0 ? 0 : start_offset;
    }
    else if (cmd.start_ > total_step)
    {
        start_offset = total_step;
    }
    else
    {
        start_offset = cmd.start_;
    }

    if (cmd.end_ < 0)
    {
        end_offset = cmd.end_ + total_step + 1;
    }
    else if (cmd.end_ >= total_step)
    {
        end_offset = total_step;
    }
    else
    {
        end_offset = cmd.end_ + 1;
    }

    if (!cmd.IsOffsetByBit())
    {
        start_offset = start_offset * 8;
        end_offset = end_offset * 8;
    }

    if (start_offset < end_offset)
    {
        int64_t result = 0;
        for (int64_t idx = start_offset; idx < end_offset; ++idx)
        {
            size_t byte_pos = idx / BITS_LEN;
            size_t bit_pos = 7 - (idx % BITS_LEN);
            char c = str_obj_.StringView().data()[byte_pos];
            result += (c >> bit_pos) & 0x1;
        }

        cmd.result_.int_ret_ = result;
    }
}

// If all subcommands in cmd is GET and not update this object, return
// false, or if there has any subcommand updated this object, return true
bool RedisStringObject::Execute(BitFieldCommand &cmd) const
{
    cmd.result_.err_code_ = RD_OK;

    int64_t len = str_obj_.Length();
    int64_t b_len = 0;
    for (const auto &scmd : cmd.vct_sub_cmd_)
    {
        int64_t l = scmd.offset_ + scmd.encoding_bits_;
        if (l > b_len)
        {
            b_len = l;
        }
    }
    b_len = (b_len + 7) >> 3;

    std::vector<uint8_t> v_bys;
    v_bys.resize(len > b_len ? len : b_len);
    uint8_t *p = v_bys.data();
    std::memcpy(p, str_obj_.Data(), len);
    if (b_len > len)
    {
        memset(p + len, 0, b_len - len);
    }

    auto &vct = cmd.result_.vct_int_;
    bool b_update = false;

    for (const auto &scmd : cmd.vct_sub_cmd_)
    {
        switch (scmd.op_type_)
        {
        case BitFieldCommand::OpType::GET:
        {
            int64_t val = GetBitFieldValue(
                p, scmd.offset_, scmd.encoding_bits_, scmd.encoding_i_);
            vct.push_back(std::make_pair(true, val));
            break;
        }
        case BitFieldCommand::OpType::SET:
        {
            int64_t limit = 0;
            bool ibu = IsBitfieldUnoverflow(scmd.value_,
                                            0,
                                            scmd.encoding_bits_,
                                            scmd.encoding_i_,
                                            scmd.ovf_,
                                            limit);
            if (!ibu)
            {
                vct.push_back(std::make_pair(false, 0));
            }
            else
            {
                int64_t val = GetBitFieldValue(
                    p, scmd.offset_, scmd.encoding_bits_, scmd.encoding_i_);
                vct.push_back(std::make_pair(true, val));
                SetBitFieldValue(p,
                                 limit,
                                 scmd.offset_,
                                 scmd.encoding_bits_,
                                 scmd.encoding_i_);
            }
            b_update = true;
            break;
        }
        case BitFieldCommand::OpType::INCR:
        {
            int64_t val = GetBitFieldValue(
                p, scmd.offset_, scmd.encoding_bits_, scmd.encoding_i_);
            int64_t limit = 0;
            bool ibu = IsBitfieldUnoverflow(val,
                                            scmd.value_,
                                            scmd.encoding_bits_,
                                            scmd.encoding_i_,
                                            scmd.ovf_,
                                            limit);
            vct.push_back(std::make_pair(ibu, limit));
            if (ibu)
            {
                SetBitFieldValue(p,
                                 limit,
                                 scmd.offset_,
                                 scmd.encoding_bits_,
                                 scmd.encoding_i_);
                b_update = true;
            }
            break;
        }
        default:
            assert(false);
        }
    }

    return b_update;
}

bool RedisStringObject::Execute(BitPosCommand &cmd) const
{
    int32_t len = str_obj_.Length() << 3;
    bool bmax = (cmd.end_ == INT64_MAX);
    if (cmd.start_ < 0)
    {
        cmd.start_ += len;
        if (cmd.start_ < 0)
        {
            cmd.start_ = 0;
        }
    }
    else if (cmd.start_ > len)
    {
        cmd.start_ = len;
    }

    if (cmd.end_ < 0)
    {
        cmd.end_ += len;
    }

    if (cmd.end_ < cmd.start_)
    {
        cmd.end_ = cmd.start_ - 1;
    }
    else if (cmd.end_ >= len)
    {
        cmd.end_ = len - 1;
    }

    const char *ptr = str_obj_.Data();
    for (int64_t i = cmd.start_; i <= cmd.end_; i++)
    {
        char c = ptr[i >> 3];
        int64_t bpos = 7 - i % 8;
        if (((c >> bpos) & 1) == cmd.bit_val_)
        {
            cmd.result_.int_ret_ = i;
            return true;
        }
    }

    cmd.result_.int_ret_ = (bmax && len > 0 && cmd.bit_val_ == 0 ? len : -1);
    return true;
}

void RedisStringObject::CommitSet(EloqString &val)
{
    if (val.Type() == EloqString::StorageType::View)
    {
        // copy EloqString for normal commands which store string_view
        str_obj_ = val.Clone();
    }
    else
    {
        // move EloqString for log replay commands and cloned commands
        str_obj_ = std::move(val);
    }
}

void RedisStringObject::CommitIncrDecr(int64_t incr)
{
    const std::string_view &sv = StringView();
    int64_t old_val = 0;

    if (sv.length() == 0)
    {
        old_val = 0;
    }
    else if (!string2ll(sv.data(), sv.size(), old_val))
    {
        LOG(ERROR) << "CommitIncrDecr on a non-integer string!";
        assert(false);
        return;
    }

    if ((incr < 0 && old_val < 0 && incr < (LLONG_MIN - old_val)) ||
        (incr > 0 && old_val > 0 && incr > (LLONG_MAX - old_val)))
    {
        LOG(ERROR) << "CommitIncrDecr out of range!";
        assert(false);
        return;
    }

    int64_t new_val = old_val + incr;
    const std::string &new_val_str = std::to_string(new_val);
    str_obj_ = EloqString(new_val_str.data(), new_val_str.size());
}

void RedisStringObject::CommitFloatIncr(double incr)
{
    const std::string_view &sv = StringView();
    long double old_val = 0;

    if (sv.length() == 0)
    {
        old_val = 0;
    }
    else if (!string2ld(sv.data(), sv.size(), old_val))
    {
        LOG(ERROR) << "CommitIncrByFloat on a non-double string!";
        assert(false);
        return;
    }

    const std::string str = ld2string(old_val + incr);
    str_obj_ = EloqString(str.c_str(), str.size());
}

void RedisStringObject::CommitSetBit(int64_t offset, int8_t val)
{
    size_t by_pos = offset / BITS_LEN;
    size_t bit_pos = 7 - offset % BITS_LEN;
    char *ptr = const_cast<char *>(str_obj_.Data());
    uint32_t len = str_obj_.Length();
    char c = (1 << bit_pos);

    // TODO: remove intermediate string creation
    if (by_pos >= str_obj_.StringView().size())
    {
        std::string str(by_pos + 1, 0);
        std::copy(ptr, ptr + len, str.begin());
        char *sp = str.data();
        if (val != 0)
        {
            sp[by_pos] = sp[by_pos] | c;
        }

        str_obj_ = EloqString(str.c_str(), str.size());
    }
    else
    {
        ptr[by_pos] = (val == 0 ? ptr[by_pos] & (~c) : ptr[by_pos] | c);
    }
}

void RedisStringObject::CommitSetRange(int64_t offset, EloqString &val)
{
    char *ptr = const_cast<char *>(str_obj_.Data());
    uint32_t len = str_obj_.Length();

    if (offset + val.Length() >= len)
    {
        std::string str(offset + val.Length(), 0);
        std::copy(ptr, ptr + (len > offset ? offset : len), str.begin());
        std::copy(val.Data(), val.Data() + val.Length(), str.begin() + offset);
        str_obj_ = EloqString(str.c_str(), str.size());
    }
    else
    {
        std::copy(val.Data(), val.Data() + val.Length(), ptr + offset);
    }
}

void RedisStringObject::CommitAppend(EloqString &val)
{
    std::string str(val.Length() + str_obj_.Length(), 0);
    std::copy(
        str_obj_.Data(), str_obj_.Data() + str_obj_.Length(), str.begin());
    std::copy(
        val.Data(), val.Data() + val.Length(), str.begin() + str_obj_.Length());
    str_obj_ = EloqString(str.c_str(), str.size());
}

void RedisStringObject::CommitBitField(
    std::vector<BitFieldCommand::SubCommand> &vct)
{
    int64_t len = str_obj_.Length();
    int64_t b_len = 0;
    for (const auto &scmd : vct)
    {
        int64_t l = scmd.offset_ + scmd.encoding_bits_;
        if (l > b_len)
        {
            b_len = l;
        }
    }
    b_len = (b_len + 7) >> 3;

    bool update = false;
    std::vector<uint8_t> v_bys;
    v_bys.resize(len > b_len ? len : b_len);
    uint8_t *p = v_bys.data();
    std::memcpy(p, str_obj_.Data(), len);
    if (b_len > len)
    {
        memset(p + len, 0, b_len - len);
    }

    for (const auto &scmd : vct)
    {
        switch (scmd.op_type_)
        {
        case BitFieldCommand::OpType::GET:
        {
            break;
        }
        case BitFieldCommand::OpType::SET:
        {
            int64_t limit = 0;
            bool ibu = IsBitfieldUnoverflow(scmd.value_,
                                            0,
                                            scmd.encoding_bits_,
                                            scmd.encoding_i_,
                                            scmd.ovf_,
                                            limit);
            if (ibu)
            {
                SetBitFieldValue(p,
                                 limit,
                                 scmd.offset_,
                                 scmd.encoding_bits_,
                                 scmd.encoding_i_);
            }
            update = true;
            break;
        }
        case BitFieldCommand::OpType::INCR:
        {
            int64_t val = GetBitFieldValue(
                p, scmd.offset_, scmd.encoding_bits_, scmd.encoding_i_);
            int64_t limit = 0;
            bool ibu = IsBitfieldUnoverflow(val,
                                            scmd.value_,
                                            scmd.encoding_bits_,
                                            scmd.encoding_i_,
                                            scmd.ovf_,
                                            limit);
            if (ibu)
            {
                SetBitFieldValue(p,
                                 limit,
                                 scmd.offset_,
                                 scmd.encoding_bits_,
                                 scmd.encoding_i_);
            }
            update = true;
            break;
        }
        default:
            assert(false);
        }
    }

    if (update)
    {
        str_obj_ = EloqString(reinterpret_cast<char *>(p),
                              static_cast<uint32_t>(v_bys.size()));
    }
}
/**
 * @brief Get bits from redis string, then convert into a int64_t integer.
 * @param p The address of redis string. The lengthof string must is not less
 * than offset + enc_bits
 * @param offset The start position to get bits, it is the total bits (NOT BYTE)
 * from the begin of string.
 * @param enc_bits encoding bits
 * @param bsign encoding is sign or unsign integer
 * @return The integer value get from redis string.
 */
int64_t RedisStringObject::GetBitFieldValue(const uint8_t *p,
                                            int64_t offset,
                                            int64_t enc_bits,
                                            bool bsign)
{
    int64_t val = 0;
    int64_t pos = offset;
    int64_t end = offset + enc_bits;

    while (pos < end)
    {
        int64_t bys_p = pos >> 3;
        int64_t bit_p = 7 - (pos & 0x7);
        uint8_t bval = p[bys_p];
        bval = (bval >> bit_p) & 1;
        val = (val << 1) | bval;
        pos++;
    }

    if (bsign && enc_bits != 64 && (val & (1ULL << (enc_bits - 1))))
    {
        val |= (0xFFFFFFFFFFFFFFFFULL) << enc_bits;
    }

    return val;
}
/**
 * @brief Set bit field into redis string from int64 integer.
 * @param p The address of redis string.
 * @param val The integer that will set into redis string
 * @param offset The start position to get bits, it is the total bits (NOT BYTE)
 * from the begin of string.
 * @param enc_bits encoding bits
 * @param bsign encoding is sign or unsign integer
 */
void RedisStringObject::SetBitFieldValue(
    uint8_t *p, int64_t val, int64_t offset, int64_t enc_bits, bool bsign)
{
    int64_t pos = offset + enc_bits - 1;
    int64_t end = offset;

    while (pos >= end)
    {
        int64_t bys_p = pos >> 3;
        int64_t bit_p = 7 - (pos & 0x7);

        uint8_t bval = static_cast<uint8_t>(val & 0x1) << bit_p;
        val >>= 1;
        uint8_t bone = ~(0x1 << bit_p);
        p[bys_p] = (p[bys_p] & bone) | bval;
        pos--;
    }
}

/**
 * @brief To check if value + incr is not overflow, and return the result
 * according overflow type.
 * @param value The left integer to check.
 * @param incr The right integer to check.
 * @param enc_bits The encoding bits.
 * @param bsign The integers is sign or not.
 * @param owtype Overflow type.
 * @param limit The added result if return true, or invalid.
 * @return True: The added result is valid; False: The added result is invalid.
 */
bool RedisStringObject::IsBitfieldUnoverflow(int64_t value,
                                             int64_t incr,
                                             int64_t enc_bits,
                                             bool bsign,
                                             BitFieldCommand::Overflow owtype,
                                             int64_t &limit)
{
    int64_t bits = bsign ? 64 - enc_bits + 1 : 64 - enc_bits;
    int64_t max = bits == 64 ? 0 : (0xFFFFFFFFFFFFFFFFULL >> bits);
    int64_t min = bsign ? (0xFFFFFFFFFFFFFFFFULL << (enc_bits - 1)) : 0;

    int64_t res = value + incr;
    bool bovf64 = false;

    if (enc_bits >= 63)
    {
        if ((value > 0 && incr > 0 && (res & 0x8000000000000000) != 0) ||
            (value < 0 && incr < 0 && (res & 0x8000000000000000) == 0))
        {
            bovf64 = true;
        }
    }

    if (!bovf64 && res <= max && res >= min)
    {
        limit = res;
        return true;
    }

    switch (owtype)
    {
    case BitFieldCommand::Overflow::WRAP:
    {
        uint64_t msb = (uint64_t) 1 << (enc_bits - 1);
        uint64_t mask = ((uint64_t) -1) << enc_bits;
        if (res & msb)
        {
            res |= mask;
        }
        else
        {
            res &= ~mask;
        }

        limit = res;
        return true;
    }
    case BitFieldCommand::Overflow::SAT:
    {
        if (res > max)
        {
            limit = max;
        }
        else if (res < min)
        {
            limit = min;
        }

        return true;
    }
    case BitFieldCommand::Overflow::FAIL:
        return false;
    default:
        assert(false);
        return false;
    }
}

bool RedisStringObject::NeedsDefrag(mi_heap_t *heap)
{
    bool defraged = false;
    float obj_utilization =
        mi_heap_page_utilization(heap, const_cast<EloqString *>(&str_obj_));
    if (obj_utilization < 0.8)
    {
        defraged = true;
    }
    else
    {
        defraged = str_obj_.NeedsDefrag(heap);
    }

    return defraged;
}

txservice::TxRecord::Uptr RedisStringObject::AddTTL(uint64_t ttl)
{
    return std::make_unique<RedisStringTTLObject>(std::move(*this), ttl);
}

}  // namespace EloqKV
