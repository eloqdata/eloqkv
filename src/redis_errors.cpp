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
#include "redis_errors.h"

#include <cassert>

namespace EloqKV
{
const char *redis_error_messages[] = {
    "OK",
    "nil",
    "WRONGTYPE Operation against a key holding the wrong kind of value",
    "ERR value is not an integer or out of range",
    "ERR value is out of range, must be positive",
    "ERR syntax error",
    "ERR GT, LT, and/or NX options at the same time are not compatible",
    "ERR XX and NX options at the same time are not compatible",
    "syntax error, LIMIT is only supported in combination with either BYSCORE "
    "or BYLEX",
    "syntax error, WITHSCORES not supported in combination with BYLEX",
    "min or max is not a int",
    "min or max is not a float",
    "min or max not valid string range item",
    "resulting score is not a number (NaN)",
    "ERR hash value is not an integer",
    "ERR invalid cursor",
    "ERR increment or decrement would overflow",
    "ERR value is not a valid float",
    "ERR value is NaN or Infinity or increment would produce NaN or Infinity",
    "ERR Number of keys can't be greater than number of args",
    "ERR numkeys should be greater than 0",
    "ERR LIMIT can't be negative",
    "WRONGPASS invalid username-password pair or user is disabled.",
    "DB index is out of range",
    "ERR no such key",
    "ERR index out of range",
    "RANK can't be zero: use 1 to start from the first match, 2 from the "
    "second ... or use negative to start",
    "ERR count should be greater than 0",
    "EXECABORT Transaction discarded because of previous errors.",
    "BUSYKEY Target key name already exists.",
    "ERR value is out of range",
    "syntax error",
    "LIMIT can't be negative",
    "ERR weight value is not a valid float",
    "ERR max key size limit of 32 MB exceeded",
    "ERR max object size limit of 256 MB exceeded",
    "ERR the key has no associated expiration time",
    "ERR cluster is shutting down",
    "ERR cluster leader is doing failover",
    "ERR create vector index failed",
    "ERR get vector index info failed",
    "ERR drop vector index failed",
    "ERR add vector to index failed",
    "ERR update vector in index failed",
    "ERR delete vector from index failed",
    "ERR search vector in index failed",
};

extern const char *redis_get_error_messages(int nr)
{
    assert(nr >= RD_ERR_FIRST && nr <= RD_ERR_LAST);
    return redis_error_messages[nr - RD_ERR_FIRST];
}

}  // namespace EloqKV
