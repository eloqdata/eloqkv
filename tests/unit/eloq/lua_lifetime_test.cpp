#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lua_interpreter.h"
#include "output_handler.h"
#include "redis_service.h"

extern "C"
{
#include "lua/src/lstate.h"
}

namespace EloqKV
{
struct LuaInterpreterTestPeer
{
    static lua_State *State(LuaInterpreter &interpreter)
    {
        return interpreter.lua_;
    }

    static size_t MemoryBytes(const LuaInterpreter &interpreter)
    {
        return G(interpreter.lua_)->totalbytes;
    }

    static size_t ScratchBufferBytes(const LuaInterpreter &interpreter)
    {
        return luaZ_sizebuffer(&G(interpreter.lua_)->buff);
    }
};
}  // namespace EloqKV

namespace
{
using EloqKV::LuaInterpreter;
using EloqKV::LuaInterpreterTestPeer;

class ScopedLuaAllocationFailure
{
public:
    explicit ScopedLuaAllocationFailure(LuaInterpreter &interpreter)
        : lua_(LuaInterpreterTestPeer::State(interpreter))
    {
        original_allocator_ = lua_getallocf(lua_, &original_context_);
        lua_setallocf(lua_, Allocate, this);
    }

    ~ScopedLuaAllocationFailure()
    {
        lua_setallocf(lua_, original_allocator_, original_context_);
    }

    void FailNextGrowth(size_t minimum_size)
    {
        minimum_size_ = minimum_size;
        armed_ = true;
    }

    size_t FailureCount() const
    {
        return failures_;
    }

private:
    static void *Allocate(void *context,
                          void *pointer,
                          size_t old_size,
                          size_t new_size)
    {
        auto *self = static_cast<ScopedLuaAllocationFailure *>(context);
        if (self->armed_ && new_size > old_size &&
            new_size >= self->minimum_size_)
        {
            self->armed_ = false;
            ++self->failures_;
            return nullptr;
        }
        return self->original_allocator_(
            self->original_context_, pointer, old_size, new_size);
    }

    lua_State *lua_;
    lua_Alloc original_allocator_;
    void *original_context_{nullptr};
    size_t minimum_size_{0};
    size_t failures_{0};
    bool armed_{false};
};

struct ScopedRedisStats
{
    ScopedRedisStats()
    {
        EloqKV::RedisStats::ExposeBVar();
    }

    ~ScopedRedisStats()
    {
        EloqKV::RedisStats::HideBVar();
    }
};

struct ScopedSocket
{
    ~ScopedSocket()
    {
        if (id != brpc::INVALID_SOCKET_ID)
        {
            brpc::Socket::SetFailed(id);
        }
    }

    brpc::SocketId id{brpc::INVALID_SOCKET_ID};
    brpc::SocketUniquePtr socket;
};

void Evaluate(LuaInterpreter &interpreter,
              std::string_view script,
              brpc::RedisReply &reply)
{
    auto [ok, sha] = interpreter.CreateFunction(script);
    REQUIRE(ok);
    std::string error;
    REQUIRE(interpreter.CallFunction(sha, &error));
    interpreter.LuaReplyToRedisReply(&reply);
}

int64_t MemoryKiB(LuaInterpreter &interpreter)
{
    // A Lua script that reads collectgarbage('count') can itself advance GC.
    // Peek at the real VM counter without executing Lua or changing GC state.
    return LuaInterpreterTestPeer::MemoryBytes(interpreter) / 1024;
}

int64_t MemoryWithoutScratchKiB(LuaInterpreter &interpreter)
{
    // Lua 5.1 shrinks its shared string-building buffer by only half per GC
    // cycle. Account for this workspace separately from retained Lua values.
    return (LuaInterpreterTestPeer::MemoryBytes(interpreter) -
            LuaInterpreterTestPeer::ScratchBufferBytes(interpreter)) /
           1024;
}

void RequireNoArguments(LuaInterpreter &interpreter)
{
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    Evaluate(interpreter,
             "return rawget(_G, 'KEYS') == nil and rawget(_G, 'ARGV') == nil",
             reply);
    REQUIRE(reply.is_integer());
    REQUIRE(reply.integer() == 1);
}
}  // namespace

TEST_CASE("Lua reset releases large arguments after copying the reply",
          "[lua][memory]")
{
    LuaInterpreter interpreter;
    const int64_t baseline = MemoryKiB(interpreter);
    std::string key(1024 * 1024, 'k');
    std::string value(1024 * 1024, 'v');
    value[31] = '\0';
    std::vector<std::string_view> keys{key};
    std::vector<std::string_view> args{value};

    for (int iteration = 0; iteration < 4; ++iteration)
    {
        interpreter.SetGlobalArray("KEYS", keys);
        interpreter.SetGlobalArray("ARGV", args);
        butil::Arena arena;
        brpc::RedisReply reply(&arena);
        Evaluate(interpreter, "return {KEYS[1], ARGV[1]}", reply);
        REQUIRE(MemoryKiB(interpreter) > baseline + 2000);
        REQUIRE(interpreter.Reset());
        REQUIRE(MemoryKiB(interpreter) < baseline + 128);
        RequireNoArguments(interpreter);
        REQUIRE(reply.is_array());
        REQUIRE(reply[0].data().as_string() == key);
        REQUIRE(reply[1].data().as_string() == value);
    }
}

TEST_CASE("Lua reset releases error paths and call hooks", "[lua][memory]")
{
    LuaInterpreter interpreter;
    const int64_t baseline = MemoryKiB(interpreter);
    std::string value(1024 * 1024, 'v');
    std::vector<std::string_view> args{value};
    interpreter.SetGlobalArray("ARGV", args);
    auto owner = std::make_shared<int>(1);
    std::weak_ptr<int> observer = owner;
    interpreter.SetScriptRedisHook([owner](auto *, const auto &, auto *) {});
    owner.reset();

    SECTION("runtime error")
    {
        auto [ok, sha] = interpreter.CreateFunction("error(ARGV[1])");
        REQUIRE(ok);
        std::string error;
        REQUIRE_FALSE(interpreter.CallFunction(sha, &error));
        REQUIRE(error.find(value) != std::string::npos);
    }
    SECTION("compile error")
    {
        REQUIRE_FALSE(interpreter.CreateFunction("return (").first);
    }

    REQUIRE(interpreter.Reset());
    REQUIRE(observer.expired());
    REQUIRE(MemoryWithoutScratchKiB(interpreter) < baseline + 128);
    RequireNoArguments(interpreter);
}

TEST_CASE("Lua reset collects large script-created temporaries",
          "[lua][memory]")
{
    LuaInterpreter interpreter;
    const int64_t baseline = MemoryKiB(interpreter);
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    Evaluate(interpreter, "return string.rep('x', 1048576)", reply);
    REQUIRE(reply.data().size() == 1024 * 1024);
    const size_t scratch_before =
        LuaInterpreterTestPeer::ScratchBufferBytes(interpreter);
    REQUIRE(interpreter.Reset());
    REQUIRE(MemoryWithoutScratchKiB(interpreter) < baseline + 128);
    REQUIRE(LuaInterpreterTestPeer::ScratchBufferBytes(interpreter) <
            scratch_before);
    REQUIRE(reply.data().as_string() == std::string(1024 * 1024, 'x'));
}

TEST_CASE("Script flush discards idle compiled caches", "[lua][script-flush]")
{
    EloqKV::RedisServiceImpl service("", "test");
    std::string script = "return '" + std::string(1024 * 1024, 'x') + "'";
    butil::Arena arena;
    brpc::RedisReply loaded(&arena);
    REQUIRE(service.ScriptLoad({"script", "load", script}, &loaded));
    const std::string sha = loaded.data().as_string();
    auto interpreter = service.GetLuaInterpreter();
    REQUIRE(MemoryKiB(*interpreter) > 1024);
    service.CleanAndReturnLuaInterpreter(std::move(interpreter));
    REQUIRE(service.ScriptFlush());

    interpreter = service.GetLuaInterpreter();
    REQUIRE(MemoryKiB(*interpreter) < 128);
    std::string error;
    REQUIRE_FALSE(interpreter->CallFunction(sha, &error));
    brpc::RedisReply exists(&arena);
    REQUIRE(service.ScriptExists({"script", "exists", sha}, &exists));
    REQUIRE(exists[0].integer() == 0);
    service.CleanAndReturnLuaInterpreter(std::move(interpreter));
}

TEST_CASE("Script flush lets active interpreters finish without recaching",
          "[lua][script-flush]")
{
    EloqKV::RedisServiceImpl service("", "test");
    auto active = service.GetLuaInterpreter();
    const auto [ok, sha] = active->CreateFunction("return 42");
    REQUIRE(ok);
    REQUIRE(service.ScriptFlush());
    std::string error;
    REQUIRE(active->CallFunction(sha, &error));
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    active->LuaReplyToRedisReply(&reply);
    REQUIRE(reply.integer() == 42);
    service.CleanAndReturnLuaInterpreter(std::move(active));

    auto current = service.GetLuaInterpreter();
    REQUIRE_FALSE(current->CallFunction(sha, &error));
    const auto [recompiled, same_sha] = current->CreateFunction("return 42");
    REQUIRE(recompiled);
    REQUIRE(same_sha == sha);
    REQUIRE(current->CallFunction(sha, &error));
    service.CleanAndReturnLuaInterpreter(std::move(current));
}

TEST_CASE("Lua reset contains finalizer errors and preserves copied replies",
          "[lua][memory][finalizer]")
{
    const std::string finalizer =
        GENERATE(std::string("error('gc failure')"),
                 std::string("redis.call('GET', 'k')"));
    LuaInterpreter interpreter;
    std::string value(1024 * 1024, 'v');
    std::vector<std::string_view> args{value};
    interpreter.SetGlobalArray("ARGV", args);
    int hook_calls = 0;
    auto hook_owner = std::make_shared<int>(1);
    std::weak_ptr<int> observer = hook_owner;
    interpreter.SetScriptRedisHook(
        [hook_owner, &hook_calls](auto *, const auto &, auto *)
        { ++hook_calls; });
    hook_owner.reset();
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    Evaluate(interpreter,
             "collectgarbage('stop'); local p = newproxy(true); "
             "getmetatable(p).__gc = function() " +
                 finalizer + " end; return ARGV[1]",
             reply);

    // The body succeeded. Its finalizer runs only when Reset collects the
    // completed attempt, after the Redis hook has become invalid.
    REQUIRE_FALSE(interpreter.Reset());
    REQUIRE(observer.expired());
    REQUIRE(hook_calls == 0);
    REQUIRE(reply.data().as_string() == value);
    // The failed VM is destroyed at scope exit, rather than reused.
}

TEST_CASE("Lua pool discards a VM whose reset finalizer failed",
          "[lua][finalizer]")
{
    EloqKV::RedisServiceImpl service("", "test");
    auto interpreter = service.GetLuaInterpreter();
    std::string value(1024 * 1024, 'v');
    std::vector<std::string_view> args{value};
    interpreter->SetGlobalArray("ARGV", args);
    const std::string script =
        "collectgarbage('stop'); local p = newproxy(true); "
        "getmetatable(p).__gc = function() error('gc failure') end; "
        "return ARGV[1]";
    auto [ok, sha] = interpreter->CreateFunction(script);
    REQUIRE(ok);
    std::string error;
    REQUIRE(interpreter->CallFunction(sha, &error));
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    interpreter->LuaReplyToRedisReply(&reply);
    service.CleanAndReturnLuaInterpreter(std::move(interpreter));
    REQUIRE(reply.data().as_string() == value);

    auto replacement = service.GetLuaInterpreter();
    REQUIRE_FALSE(replacement->CallFunction(sha, &error));
    service.CleanAndReturnLuaInterpreter(std::move(replacement));
}

TEST_CASE("Closing a Lua VM revokes its Redis hook before finalizers",
          "[lua][finalizer]")
{
    int hook_calls = 0;
    auto interpreter = std::make_unique<LuaInterpreter>();
    interpreter->SetScriptRedisHook([&hook_calls](auto *, const auto &, auto *)
                                    { ++hook_calls; });
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    Evaluate(*interpreter,
             "collectgarbage('stop'); local p = newproxy(true); "
             "getmetatable(p).__gc = function() redis.call('GET', 'k') end; "
             "return 42",
             reply);
    // Exercise destruction without Reset, as on direct owner teardown.
    interpreter.reset();
    REQUIRE(hook_calls == 0);
    REQUIRE(reply.integer() == 42);
}

TEST_CASE("Large Lua arguments are collected after the VM baseline falls",
          "[lua][memory][finalizer]")
{
    LuaInterpreter interpreter;
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    Evaluate(interpreter,
             "collectgarbage('stop'); local value = string.rep('x', 1048576); "
             "local p = newproxy(true); getmetatable(p).__gc = function() "
             "local keep = value end; return #value",
             reply);
    REQUIRE(reply.integer() == 1024 * 1024);
    REQUIRE(interpreter.Reset());
    // Lua 5.1 retains a finalized userdata and its captured value until a
    // subsequent collection. One successful full GC is not a zero-garbage
    // guarantee, and its high-water count must not suppress future cleanup.
    REQUIRE(MemoryKiB(interpreter) > 1024);
    Evaluate(interpreter, "collectgarbage('collect'); return 1", reply);
    REQUIRE(MemoryKiB(interpreter) < 128);

    std::string value(1024 * 1024, 'v');
    std::vector<std::string_view> args{value};
    interpreter.SetGlobalArray("ARGV", args);
    Evaluate(interpreter, "return ARGV[1]", reply);
    REQUIRE(interpreter.Reset());
    REQUIRE(MemoryKiB(interpreter) < 128);
    REQUIRE(reply.data().as_string() == value);
}

TEST_CASE("Old resolved EVALSHA bodies cannot repopulate a flushed VM pool",
          "[lua][script-flush]")
{
    EloqKV::RedisServiceImpl service("", "test");
    const std::string old_body = "return 42";
    butil::Arena arena;
    brpc::RedisReply loaded(&arena);
    REQUIRE(service.ScriptLoad({"script", "load", old_body}, &loaded));
    const std::string old_sha = loaded.data().as_string();
    // EVALSHA resolved its body in the service's initial generation, but has
    // not yet checked out a VM. FLUSH wins that interval.
    const uint64_t resolved_generation = 0;
    REQUIRE(service.ScriptFlush());
    auto current = service.GetLuaInterpreter();
    const auto [ok, current_sha] = current->CreateFunction("return 17");
    REQUIRE(ok);
    service.CleanAndReturnLuaInterpreter(std::move(current));

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        auto old_request = service.GetLuaInterpreter(resolved_generation);
        const auto [compiled, sha] = old_request->CreateFunction(old_body);
        REQUIRE(compiled);
        REQUIRE(sha == old_sha);
        std::string error;
        REQUIRE(old_request->CallFunction(old_sha, &error));
        brpc::RedisReply reply(&arena);
        old_request->LuaReplyToRedisReply(&reply);
        REQUIRE(reply.integer() == 42);
        service.CleanAndReturnLuaInterpreter(std::move(old_request));

        current = service.GetLuaInterpreter();
        REQUIRE_FALSE(current->CallFunction(old_sha, &error));
        REQUIRE(current->CallFunction(current_sha, &error));
        service.CleanAndReturnLuaInterpreter(std::move(current));
    }
    brpc::RedisReply exists(&arena);
    REQUIRE(service.ScriptExists({"script", "exists", old_sha}, &exists));
    REQUIRE(exists[0].integer() == 0);
}

TEST_CASE("Native Lua preparation and reply conversion do not run finalizers",
          "[lua][finalizer]")
{
    LuaInterpreter interpreter;
    std::string value(1024 * 1024, 'v');
    std::vector<std::string_view> args{value};
    interpreter.SetGlobalArray("ARGV", args);
    int hook_calls = 0;
    interpreter.SetScriptRedisHook([&hook_calls](auto *, const auto &, auto *)
                                   { ++hook_calls; });
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    // The proxy remains rooted throughout script execution and reply
    // conversion, but is no longer reachable once the reply table is popped.
    Evaluate(interpreter,
             "local p = newproxy(true); getmetatable(p).__gc = function() "
             "redis.call('GET', 'k') end; local reply = {ARGV[1]}; "
             "reply.keep = p; return reply",
             reply);
    REQUIRE(hook_calls == 0);
    interpreter.SetGlobalArray("KEYS", args);
    REQUIRE(hook_calls == 0);
    REQUIRE_FALSE(interpreter.Reset());
    REQUIRE(hook_calls == 0);
    REQUIRE(reply[0].data().as_string() == value);
}

TEST_CASE("Script flush contains finalizer errors from idle VMs",
          "[lua][finalizer][script-flush]")
{
    const std::string finalizer =
        GENERATE(std::string("error('gc failure')"),
                 std::string("redis.call('GET', 'k')"));
    EloqKV::RedisServiceImpl service("", "test");
    auto interpreter = service.GetLuaInterpreter();
    int hook_calls = 0;
    interpreter->SetScriptRedisHook([&hook_calls](auto *, const auto &, auto *)
                                    { ++hook_calls; });
    const std::string script =
        "collectgarbage('stop'); local p = newproxy(true); "
        "getmetatable(p).__gc = function() " +
        finalizer + " end; return 42";
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    Evaluate(*interpreter, script, reply);
    REQUIRE(interpreter->Reset());  // Small input does not force a full GC.
    service.CleanAndReturnLuaInterpreter(std::move(interpreter));
    REQUIRE(service.ScriptFlush());
    REQUIRE(hook_calls == 0);
    REQUIRE(reply.integer() == 42);
}

TEST_CASE("Lua VM retirement preserves client connection counters",
          "[lua][script-flush][stats]")
{
    ScopedRedisStats stats;
    ScopedSocket socket;
    brpc::SocketOptions options;
    // A real brpc Socket object exercises the client-context constructor,
    // without opening a network connection or a listening service.
    REQUIRE(brpc::Socket::Create(options, &socket.id) == 0);
    REQUIRE(brpc::Socket::Address(socket.id, &socket.socket) == 0);
    REQUIRE(EloqKV::RedisStats::GetConnectingCount() == 0);
    {
        EloqKV::RedisConnectionContext client(socket.socket.get(), nullptr);
        REQUIRE(EloqKV::RedisStats::GetConnReceivedCount() == 1);
        REQUIRE(EloqKV::RedisStats::GetConnectingCount() == 1);
        for (int iteration = 0; iteration < 4; ++iteration)
        {
            {
                EloqKV::RedisServiceImpl service("", "test");
                auto interpreter = service.GetLuaInterpreter();
                butil::Arena arena;
                brpc::RedisReply reply(&arena);
                Evaluate(*interpreter, "return 42", reply);
                service.CleanAndReturnLuaInterpreter(std::move(interpreter));
                REQUIRE(service.ScriptFlush());
                REQUIRE(EloqKV::RedisStats::GetConnectingCount() == 1);
                service.CleanAndReturnLuaInterpreter(
                    service.GetLuaInterpreter());
            }
            // Service destruction retires its remaining idle interpreter too.
            REQUIRE(EloqKV::RedisStats::GetConnReceivedCount() == 1);
            REQUIRE(EloqKV::RedisStats::GetConnectingCount() == 1);
        }
    }
    REQUIRE(EloqKV::RedisStats::GetConnReceivedCount() == 1);
    REQUIRE(EloqKV::RedisStats::GetConnectingCount() == 0);
}

TEST_CASE(
    "Lua pool reuse clears a Redis call interrupted by allocation failure",
    "[lua][memory][redis-call-reuse]")
{
    EloqKV::RedisServiceImpl service("", "test");
    auto interpreter = service.GetLuaInterpreter();
    LuaInterpreter *original_interpreter = interpreter.get();
    auto [compiled, sha] =
        interpreter->CreateFunction("return redis.call('GET', 'k')");
    REQUIRE(compiled);

    const std::string large_reply(4096, 'r');
    size_t failed_hook_calls = 0;
    {
        // The guard lives outside lua_pcall's frames, so the Lua OOM jump
        // cannot skip restoration of the VM's allocator.
        ScopedLuaAllocationFailure allocation_failure(*interpreter);
        interpreter->SetScriptRedisHook(
            [&](auto *, const auto &, EloqKV::OutputHandler *output)
            {
                ++failed_hook_calls;
                allocation_failure.FailNextGrowth(large_reply.size());
                output->OnString(large_reply);
            });
        std::string error;
        const bool success = interpreter->CallFunction(sha, &error);
        interpreter->SetScriptRedisHook(nullptr);
        REQUIRE_FALSE(success);
        REQUIRE(allocation_failure.FailureCount() == 1);
        REQUIRE(failed_hook_calls == 1);
        REQUIRE(error.find("not enough memory") != std::string::npos);
    }

    service.CleanAndReturnLuaInterpreter(std::move(interpreter));
    interpreter = service.GetLuaInterpreter();
    REQUIRE(interpreter.get() == original_interpreter);

    size_t recovered_hook_calls = 0;
    interpreter->SetScriptRedisHook(
        [&](auto *, const auto &, EloqKV::OutputHandler *output)
        {
            ++recovered_hook_calls;
            output->OnString("recovered");
        });
    std::string error;
    REQUIRE(interpreter->CallFunction(sha, &error));
    butil::Arena arena;
    brpc::RedisReply reply(&arena);
    interpreter->LuaReplyToRedisReply(&reply);
    INFO((reply.is_error() ? reply.error_message() : "non-error reply"));
    REQUIRE(recovered_hook_calls == 1);
    REQUIRE(reply.is_string());
    REQUIRE(reply.data().as_string() == "recovered");
    service.CleanAndReturnLuaInterpreter(std::move(interpreter));
}
