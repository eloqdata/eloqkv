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

namespace EloqKV
{
static const int RD_ERR_FIRST = 0;
static const int RD_OK = RD_ERR_FIRST;
static const int RD_NIL = RD_ERR_FIRST + 1;
static const int RD_ERR_WRONG_TYPE = RD_ERR_FIRST + 2;
static const int RD_ERR_DIGITAL_INVALID = RD_ERR_FIRST + 3;
static const int RD_ERR_POSITIVE_INVALID = RD_ERR_FIRST + 4;
static const int RD_ERR_SYNTAX = RD_ERR_FIRST + 5;
static const int RD_ERR_LT_GT_NX = RD_ERR_FIRST + 6;
static const int RD_ERR_XX_NX = RD_ERR_FIRST + 7;
static const int RD_ERR_LIMIT = RD_ERR_FIRST + 8;
static const int RD_ERR_WITHSCORE_BYLEX = RD_ERR_FIRST + 9;
static const int RD_ERR_MIN_MAX_NOT_INT = RD_ERR_FIRST + 10;
static const int RD_ERR_MIN_MAX_NOT_FLOAT = RD_ERR_FIRST + 11;
static const int RD_ERR_MIN_MAX_NOT_STRING = RD_ERR_FIRST + 12;
static const int RD_ERR_SCORE_NAN = RD_ERR_FIRST + 13;
static const int RD_ERR_HASH_VAL_ERROR = RD_ERR_FIRST + 14;
static const int RD_ERR_INVALID_CURSOR = RD_ERR_FIRST + 15;
static const int RD_ERR_INCR_OVERFLOW = RD_ERR_FIRST + 16;
static const int RD_ERR_FLOAT_VALUE = RD_ERR_FIRST + 17;
static const int RD_ERR_INCR_NAN_OR_INFINITY = RD_ERR_FIRST + 18;
static const int RD_ERR_NUMBER_GREATER_ARGS = RD_ERR_FIRST + 19;
static const int RD_ERR_NUMBER_GREATER_ZERO = RD_ERR_FIRST + 20;
static const int RD_ERR_LIMIT_NEGATIVE = RD_ERR_FIRST + 21;
static const int RD_ERR_WRONG_PASS = RD_ERR_FIRST + 22;
static const int RD_ERR_SELECT_OUT_OF_RANGE = RD_ERR_FIRST + 23;
static const int RD_ERR_NO_SUCH_KEY = RD_ERR_FIRST + 24;
static const int RD_ERR_INDEX_OUT_OF_RANGE = RD_ERR_FIRST + 25;
static const int RD_ERR_ZERO_RANK = RD_ERR_FIRST + 26;
static const int RD_ERR_COUNT_GREATER_ZERO = RD_ERR_FIRST + 27;
static const int RD_ERR_EXEC_ABORT_FOR_PREV_ERROR = RD_ERR_FIRST + 28;
static const int RD_ERR_BUSY_KEY_EXIST = RD_ERR_FIRST + 29;
static const int RD_ERR_VALUE_OUT_OF_RANGE = RD_ERR_FIRST + 30;
static const int RD_SYNTAX = RD_ERR_FIRST + 31;
static const int RD_LIMIT_NEGATIVE = RD_ERR_FIRST + 32;
static const int RD_ERR_WEIGHT_FLOAT_VALUE = RD_ERR_FIRST + 33;
static const int RD_ERR_KEY_TOO_BIG = RD_ERR_FIRST + 34;
static const int RD_ERR_OBJECT_TOO_BIG = RD_ERR_FIRST + 35;
static const int RD_ERR_TTL_NOT_EXIST_ON_KEY = RD_ERR_FIRST + 36;
static const int RD_ERR_ClUSTER_IS_SHUTTING_DOWN = RD_ERR_FIRST + 37;
static const int RD_ERR_LEADER_IS_FAILING_OVER = RD_ERR_FIRST + 38;
static const int RD_ERR_VECTOR_INDX_CREATE_FAILED = RD_ERR_FIRST + 39;
static const int RD_ERR_VECTOR_INDX_INFO_FAILED = RD_ERR_FIRST + 40;
static const int RD_ERR_VECTOR_INDX_DROP_FAILED = RD_ERR_FIRST + 41;
static const int RD_ERR_VECTOR_INDX_ADD_FAILED = RD_ERR_FIRST + 42;
static const int RD_ERR_VECTOR_INDX_UPDATE_FAILED = RD_ERR_FIRST + 43;
static const int RD_ERR_VECTOR_INDX_DELETE_FAILED = RD_ERR_FIRST + 44;
static const int RD_ERR_VECTOR_INDX_SEARCH_FAILED = RD_ERR_FIRST + 45;

static const int RD_ERR_LAST = RD_ERR_VECTOR_INDX_SEARCH_FAILED;

extern const char *redis_error_messages[];
extern const char *redis_get_error_messages(int nr);

}  // namespace EloqKV
