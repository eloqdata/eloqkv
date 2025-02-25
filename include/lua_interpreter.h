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

    void CleanStack();

    static void sha1hex(char *digest, const char *script, size_t len);

    template <typename T>
    void SetScriptRedisHook(T &&t)
    {
        script_call_ = std::forward<T>(t);
    }

private:
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
