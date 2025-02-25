#include "lua_interpreter.h"

#include <butil/logging.h>
#include <butil/strings/string_piece.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lua_output_handler.h"

extern "C"
{
#include "lua/src/lauxlib.h"
#include "lua/src/lua.h"
#include "lua/src/lualib.h"
#include "redis/sha1.h"

    LUALIB_API int(luaopen_cjson)(lua_State *L);
    LUALIB_API int(luaopen_struct)(lua_State *L);
    LUALIB_API int(luaopen_cmsgpack)(lua_State *L);
    LUALIB_API int(luaopen_bit)(lua_State *L);
}

namespace EloqKV
{

int EVPDigest(const void *data,
              size_t datalen,
              unsigned char *md,
              size_t *mdlen)
{
    unsigned int temp = 0;
    int ret = EVP_Digest(data, datalen, md, &temp, EVP_sha1(), NULL);

    if (mdlen != NULL)
        *mdlen = temp;
    return ret;
}

void LuaOutputHandler::OnBool(bool b)
{
    CHECK(!b) << "Only false (nil) supported";
    lua_pushboolean(lua_, 0);
    UpdateArray();
}

void LuaOutputHandler::OnString(std::string_view str)
{
    lua_pushlstring(lua_, str.data(), str.size());
    UpdateArray();
}

void LuaOutputHandler::OnInt(int64_t val)
{
    lua_pushinteger(lua_, val);
    UpdateArray();
}

void LuaOutputHandler::OnNil()
{
    lua_pushboolean(lua_, 0);
    UpdateArray();
}

void LuaOutputHandler::OnStatus(std::string_view str)
{
    CHECK(array_index_.empty()) << "unexpected status";
    lua_newtable(lua_);
    lua_pushstring(lua_, "ok");
    lua_pushlstring(lua_, str.data(), str.size());
    lua_settable(lua_, -3);
}

void LuaOutputHandler::OnError(std::string_view str)
{
    CHECK(array_index_.empty()) << "unexpected error";
    lua_newtable(lua_);
    lua_pushstring(lua_, "err");
    lua_pushlstring(lua_, str.data(), str.size());
    lua_settable(lua_, -3);
    has_error_ = true;
}

void LuaOutputHandler::OnFormatError(const char *fmt, ...)
{
    va_list args, cpy;
    va_start(args, fmt);

    char static_buf[1024], *buf = static_buf;
    size_t buflen = sizeof(static_buf);
    int bufstrlen;

    while (true)
    {
        va_copy(cpy, args);
        bufstrlen = vsnprintf(buf, buflen, fmt, cpy);
        va_end(cpy);

        assert(bufstrlen >= 0);
        if (static_cast<size_t>(bufstrlen) >= buflen)
        {
            buflen *= 2;
            if (buf == static_buf)
            {
                buf = static_cast<char *>(malloc(buflen));
            }
            else
            {
                buf = static_cast<char *>(realloc(buf, buflen));
            }
        }
        else
        {
            break;
        }
    }

    OnString(std::string_view(buf, bufstrlen));

    if (buf != static_buf)
    {
        free(buf);
    }

    va_end(args);
}

bool LuaOutputHandler::HasError()
{
    return has_error_;
}

void LuaOutputHandler::OnArrayStart(unsigned len)
{
    lua_newtable(lua_);
    array_index_.push_back(1);
}

void LuaOutputHandler::OnArrayEnd()
{
    CHECK(!array_index_.empty());
    DCHECK(lua_istable(lua_, -1));

    array_index_.pop_back();
    UpdateArray();
}

void LoadLib(lua_State *lua, const char *libname, lua_CFunction luafunc)
{
    lua_pushcfunction(lua, luafunc);
    lua_pushstring(lua, libname);
    lua_call(lua, 1, 0);
    lua_getglobal(lua, libname);
    if (lua_istable(lua, -1))
    {
        lua_enablereadonlytable(lua, -1, 1);
    }
    lua_pop(lua, 1);
}

void InitLua(lua_State *lua)
{
    LoadLib(lua, "", luaopen_base);
    LoadLib(lua, LUA_TABLIBNAME, luaopen_table);
    LoadLib(lua, LUA_STRLIBNAME, luaopen_string);
    LoadLib(lua, LUA_MATHLIBNAME, luaopen_math);
    LoadLib(lua, LUA_DBLIBNAME, luaopen_debug);
    LoadLib(lua, "cjson", luaopen_cjson);
    LoadLib(lua, "struct", luaopen_struct);
    LoadLib(lua, "cmsgpack", luaopen_cmsgpack);
    LoadLib(lua, "bit", luaopen_bit);

    /* Add a helper function we use for pcall error reporting.
     * Note that when the error is in the C function we want to report the
     * information about the caller, that's what makes sense from the point
     * of view of the user debugging a script. */
    {
        const char *errh_func =
            "local dbg = debug\n"
            "function __redis__err__handler(err)\n"
            "  local i = dbg.getinfo(2,'nSl')\n"
            "  if i and i.what == 'C' then\n"
            "    i = dbg.getinfo(3,'nSl')\n"
            "  end\n"
            "  if i then\n"
            "    return err .. ', on ' .. i.source .. ':' .. i.currentline\n"
            "  else\n"
            "    return err\n"
            "  end\n"
            "end\n";
        luaL_loadbuffer(lua, errh_func, strlen(errh_func), "@err_handler_def");
        int err = lua_pcall(lua, 0, 0, 0);
        if (err)
        {
            const char *errstr = lua_tostring(lua, -1);
            LOG(ERROR) << "Error running err handler declaration: " << errstr;
        }
    }

    /* This code installs metamethods in the global table _G that prevent
     * the creation of globals accidentally.
     *
     * It should be the last to be called in the scripting engine initialization
     * sequence, because it may interact with creation of globals. */
    {
        const char *global_protection_code =
            "local dbg=debug\n"
            "local mt = {}\n"
            "setmetatable(_G, mt)\n"
            "mt.__newindex = function (t, n, v)\n"
            "  if dbg.getinfo(2) then\n"
            "    local w = dbg.getinfo(2, \"S\").what\n"
            "    if w ~= \"user_script\" and w ~= \"C\" then\n"
            "      error(\"Script attempted to create global variable "
            "'\"..tostring(n)..\"'\", 2)\n"
            "    end\n"
            "  end\n"
            "  rawset(t, n, v)\n"
            "end\n"
            "mt.__index = function (t, n)\n"
            "  if dbg.getinfo(2) and dbg.getinfo(2, \"S\").what ~= \"C\" then\n"
            "    error(\"Script attempted to access nonexistent global "
            "variable '\"..tostring(n)..\"'\", 2)\n"
            "  end\n"
            "  return rawget(t, n)\n"
            "end\n"
            "local original_getmetatable = getmetatable\n"
            "getmetatable = function(t)\n"
            "  if t == _G then\n"
            "    local mt = original_getmetatable(t)\n"
            "    if mt then\n"
            "      local readonly_mt = {}\n"
            "      for k, v in pairs(mt) do\n"
            "        readonly_mt[k] = v\n"
            "      end\n"
            "      readonly_mt.__newindex = function(t, n, v)\n"
            "        error(\"Attempt to modify a readonly table\", 2)\n"
            "      end\n"
            "      readonly_mt.__metatable = \"readonly\"\n"
            "      return setmetatable({}, readonly_mt)\n"
            "    else\n"
            "      return nil\n"
            "    end\n"
            "  else\n"
            "    return original_getmetatable(t)\n"
            "  end\n"
            "end\n"
            "debug = nil\n";
        luaL_loadbuffer(lua,
                        global_protection_code,
                        strlen(global_protection_code),
                        "@enable_strict_lua");
        int err = lua_pcall(lua, 0, 0, 0);
        if (err)
        {
            const char *errstr = lua_tostring(lua, -1);
            LOG(ERROR) << "Error running global protection code: " << errstr;
        }
    }

    lua_pushnil(lua);
    lua_setglobal(lua, "print");
    lua_pushnil(lua);
    lua_setglobal(lua, "loadfile");
    lua_pushnil(lua);
    lua_setglobal(lua, "dofile");
}

void PushError(lua_State *lua, const char *error)
{
    // DLOG(INFO) << "PushError: " << error;

    // need table?
    lua_newtable(lua);
    lua_pushstring(lua, "err");
    lua_pushstring(lua, error);
    lua_settable(lua, -3);
}

/* In case the error set into the Lua stack by luaPushError() was generated
 * by the non-error-trapping version of redis.pcall(), which is redis.call(),
 * this function will raise the Lua error so that the execution of the
 * script will be halted. */
int LuaError(lua_State *lua)
{
    lua_pushstring(lua, "err");
    lua_gettable(lua, -2);
    return lua_error(lua);
}

LuaInterpreter::LuaInterpreter()
{
    lua_ = luaL_newstate();
    InitLua(lua_);
    SaveOnRegistry(lua_, "interpreter", this);

    RegisterRedisAPI(lua_);
}

LuaInterpreter::~LuaInterpreter()
{
    lua_close(lua_);
}

/**
 * Define a Lua function with the specified body.
 * The function name will be generated in the following form:
 *
 *   f_<hex sha1 sum>
 *
 * On error the client should be informed with an appropriate error describing
 * the nature of the problem and the Lua interpreter error.
 *
 * @param body
 * @return true and sha, or false and the error message
 */
std::pair<bool, std::string> LuaInterpreter::CreateFunction(
    std::string_view body)
{
    std::string sha;
    std::string error;
    char funcname[43];
    funcname[0] = 'f';
    funcname[1] = '_';
    sha1hex(funcname + 2, body.data(), body.size());
    funcname[42] = '\0';

    lua_getglobal(lua_, funcname);
    if (lua_isnil(lua_, -1))
    {
        lua_pop(lua_, 1); /* remove the nil from the stack */
        // func_name not exist, add function
        if (luaL_loadbuffer(lua_, body.data(), body.size(), "@user_script"))
        {
            const char *error_msg = lua_tostring(lua_, -1);
            // the error_msg will be sent to the client
            error.assign(error_msg);
            lua_pop(lua_, 1);
            return {false, std::move(error)};
        }
        lua_setglobal(lua_, funcname);
    }

    hash_.assign(funcname + 2);
    sha.assign(funcname + 2);
    return {true, std::move(sha)};
}

/*
 * Set an array of string_view as a Lua array (table) stored into a
 * global variable
 */
void LuaInterpreter::SetGlobalArray(const char *name,
                                    std::vector<std::string_view> &args)
{
    lua_newtable(lua_);
    for (size_t j = 0; j < args.size(); j++)
    {
        lua_pushlstring(lua_, args[j].data(), args[j].size());
        lua_rawseti(lua_, -2, j + 1);
    }
    lua_setglobal(lua_, name);
};

void LuaInterpreter::SetConnectionContext(const RedisConnectionContext &ctx)
{
    lua_conn_ctx_.db_id = ctx.db_id;
}

bool LuaInterpreter::CallFunction(std::string_view sha, std::string *error)
{
    // DLOG(INFO) << "CallFunction: " << sha << " " << lua_gettop(lua_);

    assert(sha.size() == 40);

    /* Push the pcall error handler function on the stack. */
    lua_getglobal(lua_, "__redis__err__handler");
    char fname[43];
    fname[0] = 'f';
    fname[1] = '_';
    memcpy(fname + 2, sha.data(), 40);
    fname[42] = '\0';

    lua_getglobal(lua_, fname);
    if (!lua_isfunction(lua_, -1))
    {
        // DLOG(ERROR) << "Non function: fname: " << fname;
        lua_pop(lua_, 2);
        return false;
    }
    lua_enablereadonlytable(lua_, LUA_GLOBALSINDEX, 1);
    int err = lua_pcall(lua_, 0, 1, -2);
    lua_enablereadonlytable(lua_, LUA_GLOBALSINDEX, 0);

    if (err)
    {
        std::string err_msg = "ERR ";
        err_msg.append(lua_tostring(lua_, -1));
        *error = err_msg;
        // LOG(ERROR) << "error calling script: " << *error;
    }

    return err == 0 ? true : false;
}

/**
 * Convert lua return value to redis reply.
 */
void LuaInterpreter::LuaReplyToRedisReply(brpc::RedisReply *output)
{
    int t = lua_type(lua_, -1);
    if (!lua_checkstack(lua_, 4))
    {
        /* Increase the Lua stack if needed to make sure there is enough room
         * to push 4 elements to the stack. On failure, return error.
         * Notice that we need, in the worst case, 4 elements because returning
         * a map might require push 4 elements to the Lua stack.*/
        output->SetError("ERR reached lua stack limit");
        lua_pop(lua_, 1); /* pop the element from the stack */
        return;
    }

    switch (t)
    {
    case LUA_TSTRING:
    {
        // DLOG(INFO) << "lua script return string result: "
        //<< std::string_view(lua_tostring(lua_, -1),
        // lua_strlen(lua_, -1))
        //<< ", str len: " << lua_strlen(lua_, -1);
        output->SetString({lua_tostring(lua_, -1), lua_strlen(lua_, -1)});
        break;
    }
    case LUA_TBOOLEAN:
    {
        int res = lua_toboolean(lua_, -1);
        // DLOG(INFO) << "lua script return bool result: " << res;
        if (res == 0)
        {
            output->SetNullString();
        }
        else
        {
            output->SetInteger(1);
        }
        break;
    }
    case LUA_TNUMBER:
    {
        // DLOG(INFO) << "lua script return number result: "
        //<< lua_tonumber(lua_, -1);
        output->SetInteger(lua_tonumber(lua_, -1));
        break;
    }
    case LUA_TTABLE:
    {
        /* We need to check if it is an array, an error, or a status reply.
         * Error are returned as a single element table with 'err' field.
         * Status replies are returned as single element table with 'ok'
         * field. */

        /* Handle error reply. */
        /* we took care of the stack size on function start */
        lua_pushstring(lua_, "err");
        lua_rawget(lua_, -2);
        t = lua_type(lua_, -1);
        if (t == LUA_TSTRING)
        {
            std::string err_msg;
            err_msg.append(
                std::string_view{lua_tostring(lua_, -1), lua_strlen(lua_, -1)});
            // DLOG(INFO) << "lua script return err: " << err_msg;

            output->SetError(err_msg);
            lua_pop(lua_, 2); /* pop the error msg and result table */
            return;
        }
        lua_pop(lua_, 1); /* Discard field name pushed before. */

        /* Handle status reply. */
        lua_pushstring(lua_, "ok");
        lua_rawget(lua_, -2);
        t = lua_type(lua_, -1);
        if (t == LUA_TSTRING)
        {
            output->SetStatus(butil::StringPiece(lua_tostring(lua_, -1),
                                                 lua_strlen(lua_, -1)));
            lua_pop(lua_, 2);
            return;
        }
        lua_pop(lua_, 1); /* Discard field name pushed before. */

        unsigned len = lua_strlen(lua_, -1);
        // DLOG(INFO) << "lua script return table len: " << len;
        output->SetArray(len);
        for (unsigned i = 0; i < len; ++i)
        {
            lua_rawgeti(lua_, -1, i + 1);  // push table element
            t = lua_type(lua_, -1);

            /* we took care of the stack size on function start */
            LuaReplyToRedisReply(&(*output)[i]);  // pops the element
            // DLOG(INFO) << "table iterates to: " << i + 1;
        }

        break;
    }
    case LUA_TNIL:
    {
        // DLOG(INFO) << "lua script return nil result";
        output->SetNullString();
        break;
    }
    default:
        // DLOG(INFO) << "lua script return type: " << t;
        output->SetError("ERR Unsupported return type");
    }
    lua_pop(lua_, 1);
}

void LuaInterpreter::CleanStack()
{
    lua_settop(lua_, 0);
}

/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
void LuaInterpreter::sha1hex(char *digest, const char *script, size_t len)
{
    SHA1_CTX ctx;
    unsigned char hash[20];
    std::string cset = "0123456789abcdef";
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx, (unsigned char *) script, len);
    SHA1Final(hash, &ctx);

    for (j = 0; j < 20; j++)
    {
        digest[j * 2] = cset[((hash[j] & 0xF0) >> 4)];
        digest[j * 2 + 1] = cset[(hash[j] & 0xF)];
    }
    digest[40] = '\0';
}

int LuaInterpreter::RedisGenericCommand(bool raise_error)
{
    // DLOG(INFO) << "calling RedisCommand";

    if (!script_call_)
    {
        PushError(lua_, "ERR LuaState error: redis function not defined");
        return raise_error ? LuaError(lua_) : 1;
    }

    /* By using Lua debug hooks it is possible to trigger a recursive call
     * to luaRedisGenericCommand(), which normally should never happen.
     * To make this function reentrant is futile and makes it slower, but
     * we should at least detect such a misuse, and abort. */
    if (redis_cmd_in_use_)
    {
        const char *recursion_warning =
            "RedisGenericCommand() recursive call detected. "
            "Are you doing funny stuff with Lua debug hooks?";
        PushError(lua_, recursion_warning);
        return 1;
    }
    redis_cmd_in_use_++;

    int argc = lua_gettop(lua_);

    if (argc == 0)
    {
        PushError(
            lua_,
            "Please specify at least one argument for this redis lib call");
        redis_cmd_in_use_--;
        return raise_error ? LuaError(lua_) : 1;
    }

    std::vector<std::string> args;
    // extract command args from Lua stack.
    int index = 0;
    for (index = 0; index < argc; index++)
    {
        const char *obj_s;
        size_t obj_len;
        char dbuf[64];

        if (lua_type(lua_, index + 1) == LUA_TNUMBER)
        {
            /* We can't use lua_tolstring() for number -> string conversion
             * since Lua uses a format specifier that loses precision. */
            lua_Number num = lua_tonumber(lua_, index + 1);

            obj_len =
                snprintf(dbuf, sizeof(dbuf), "%.17g", static_cast<double>(num));
            obj_s = dbuf;
        }
        else
        {
            obj_s = lua_tolstring(lua_, index + 1, &obj_len);
            if (obj_s == nullptr)
            {
                PushError(lua_,
                          "Lua redis lib command arguments must be strings or "
                          "integers");
                redis_cmd_in_use_--;
                return raise_error ? LuaError(lua_) : 1;
            }
        }

        args.emplace_back(obj_s, obj_len);
        if (index == 0)
        {
            // convert command name to lower case
            std::string &str = args[index];
            std::transform(str.begin(),
                           str.end(),
                           str.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        }
    }

    /* Check if one of the arguments passed by the Lua script
     * is not a string or an integer (lua_isstring() return true for
     * integers as well). */
    if (index != argc)
    {
        PushError(
            lua_,
            "Lua redis lib command arguments must be strings or integers");
        // DLOG(WARNING)
        //<< "Lua redis lib command arguments must be strings or integers";
        redis_cmd_in_use_--;
        return raise_error ? LuaError(lua_) : 1;
    }

    /* Pop all arguments from the stack, we do not need them anymore
     * and this way we guaranty we will have room on the stack for the result.
     */
    lua_pop(lua_, argc);
    LuaOutputHandler output(lua_);
    // call redis_service's GenericCommand method
    script_call_(&lua_conn_ctx_, args, &output);

    if (output.HasError() && raise_error)
    {
        redis_cmd_in_use_--;
        return LuaError(lua_);
    }
    DCHECK_EQ(1, lua_gettop(lua_));

    redis_cmd_in_use_--;
    return 1;
}

int LuaInterpreter::RedisCallCommand(lua_State *lua)
{
    void *ptr = static_cast<void *>(GetFromRegistry(lua, "interpreter"));
    assert(ptr != nullptr);
    return reinterpret_cast<LuaInterpreter *>(ptr)->RedisGenericCommand(true);
}

int LuaInterpreter::RedisPCallCommand(lua_State *lua)
{
    void *ptr = static_cast<void *>(GetFromRegistry(lua, "interpreter"));
    assert(ptr != nullptr);
    return reinterpret_cast<LuaInterpreter *>(ptr)->RedisGenericCommand(false);
}

/* Returns a table with a single field 'field' set to the string value
 * passed as argument. This helper function is handy when returning
 * a Redis Protocol error or status reply from Lua:
 *
 * return redis.error_reply("ERR Some Error")
 * return redis.status_reply("ERR Some Error")
 */
int LuaInterpreter::RedisReturnSingleFieldTable(lua_State *lua,
                                                const char *field)
{
    if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING)
    {
        PushError(lua, "ERR wrong number or type of arguments");
        return 1;
    }

    lua_newtable(lua);
    lua_pushstring(lua, field);
    lua_pushvalue(lua, -3);
    lua_settable(lua, -3);
    return 1;
}

/* redis.error_reply() */
int LuaInterpreter::RedisErrorReplyCommand(lua_State *lua)
{
    return RedisReturnSingleFieldTable(lua, "err");
}

/* redis.status_reply() */
int LuaInterpreter::RedisStatusReplyCommand(lua_State *lua)
{
    return RedisReturnSingleFieldTable(lua, "ok");
}

int LuaInterpreter::RedisSha1HexCommand(lua_State *lua)
{
    int argc = lua_gettop(lua);
    if (argc != 1)
    {
        lua_pushstring(lua, "wrong number of arguments");
        return lua_error(lua);
    }

    size_t len;
    const char *str = lua_tolstring(lua, 1, &len);
    char hex[41];
    sha1hex(hex, const_cast<char *>(str), len);

    lua_pushstring(lua, hex);
    return 1;
}

void LuaInterpreter::RegisterRedisAPI(lua_State *lua)
{
    /* Register the redis commands table and fields */
    lua_newtable(lua);

    /* redis.pcall */
    lua_pushstring(lua, "pcall");
    lua_pushcfunction(lua, RedisPCallCommand);
    lua_settable(lua, -3);

    /* redis.call */
    lua_pushstring(lua, "call");
    lua_pushcfunction(lua, RedisCallCommand);
    lua_settable(lua, -3);

    /* redis.error_reply */
    lua_pushstring(lua, "error_reply");
    lua_pushcfunction(lua, RedisErrorReplyCommand);
    lua_settable(lua, -3);

    /* redis.status_reply */
    lua_pushstring(lua, "status_reply");
    lua_pushcfunction(lua, RedisStatusReplyCommand);
    lua_settable(lua, -3);

    /* redis.sha1hex */
    lua_pushstring(lua, "sha1hex");
    lua_pushcfunction(lua, RedisSha1HexCommand);
    lua_settable(lua, -3);

    /* Finally set the table as 'redis' global var. */
    lua_setglobal(lua, "redis");
    CHECK(lua_checkstack(lua, 64));

    /* Enable the read-only attribute for the redis table */
    lua_getglobal(lua, "redis");
    if (lua_istable(lua, -1))
    {
        lua_enablereadonlytable(lua, -1, 1);
    }
    lua_pop(lua, 1);
}

/*
 * Save the give pointer on Lua registry, used to save the Lua context so we can
 * retrieve them from lua_State when calling Redis command.
 */
void LuaInterpreter::SaveOnRegistry(lua_State *lua, const char *name, void *ptr)
{
    lua_pushstring(lua, name);
    if (ptr)
    {
        lua_pushlightuserdata(lua, ptr);
    }
    else
    {
        lua_pushnil(lua);
    }
    lua_settable(lua, LUA_REGISTRYINDEX);
}

/*
 * Get a saved pointer from registry.
 */
void *LuaInterpreter::GetFromRegistry(lua_State *lua, const char *name)
{
    lua_pushstring(lua, name);
    lua_gettable(lua, LUA_REGISTRYINDEX);

    if (lua_isnil(lua, -1))
    {
        lua_pop(lua, 1); /* pops the value */
        return NULL;
    }

    void *ptr = (void *) lua_topointer(lua, -1);

    /* pops the value */
    lua_pop(lua, 1);

    return ptr;
}

}  // namespace EloqKV
