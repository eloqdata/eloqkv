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
};

extern const char *redis_get_error_messages(int nr)
{
    assert(nr >= RD_ERR_FIRST && nr <= RD_ERR_LAST);
    return redis_error_messages[nr - RD_ERR_FIRST];
}

}  // namespace EloqKV
