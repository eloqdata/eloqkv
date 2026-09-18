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

#include <brpc/redis_reply.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "redis_connection_context.h"

extern "C"
{
#include "lua/src/lua.h"
}

namespace EloqKV
{
class OutputHandler;

/**
 * Lua interpreter.
 */
class LuaInterpreter
{
public:
    LuaInterpreter();
    ~LuaInterpreter();

    std::pair<bool, std::string> CreateFunction(std::string_view body);

    void SetGlobalArray(const char *name, std::vector<std::string_view> &args);

    void SetConnectionContext(const RedisConnectionContext &ctx);

    bool CallFunction(std::string_view sha, std::string *err);

    void LuaReplyToRedisReply(brpc::RedisReply *);

    // Release completed-call roots after reply conversion or error handling.
    // Collect large transient allocations before an interpreter becomes idle.
    // A finalizer error makes the VM unsuitable for reuse; the caller must
    // destroy it instead of returning it to the interpreter pool.
    bool Reset();

    static void sha1hex(char *digest, const char *script, size_t len);

    template <typename T>
    void SetScriptRedisHook(T &&t)
    {
        script_call_ = std::forward<T>(t);
    }

private:
    friend class RedisServiceImpl;
    friend struct LuaInterpreterTestPeer;

    int RedisGenericCommand(bool raise_error);

    static int RedisCallCommand(lua_State *lua);
    static int RedisPCallCommand(lua_State *lua);

    static int RedisReturnSingleFieldTable(lua_State *lua, const char *field);
    static int RedisErrorReplyCommand(lua_State *lua);
    static int RedisStatusReplyCommand(lua_State *lua);
    static int RedisSha1HexCommand(lua_State *lua);

    static void RegisterRedisAPI(lua_State *lua);
    static void SaveOnRegistry(lua_State *lua, const char *name, void *ptr);
    static void *GetFromRegistry(lua_State *lua, const char *name);

    lua_State *lua_;
    size_t gc_baseline_bytes_{0};
    size_t input_bytes_since_reset_{0};
    // Protected by RedisServiceImpl::script_mutex_ when checking out/returning
    // this interpreter; an active interpreter remains exclusively owned.
    uint64_t script_cache_generation_{0};

    /* Recursive RedisGenericCommand calls detection. */
    // TODO(zkl): making this atomic?
    int redis_cmd_in_use_{};

    // The "fake connection context" to query Redis from Lua.
    //
    // LuaState should own a RedisConnectionContext, instead of referencing a
    // client's RedisConnectionContext. Because executing `select db` in a
    // script is allowed, and it shouldn't affect the outside
    // RedisConnectionContext.
    RedisConnectionContext lua_conn_ctx_;

    std::function<void(RedisConnectionContext *,
                       const std::vector<std::string> &,
                       OutputHandler *)>
        script_call_;
    std::string hash_;
};

}  // namespace EloqKV
