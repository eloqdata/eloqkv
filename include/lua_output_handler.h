#pragma once

#include <vector>

#include "output_handler.h"

extern "C"
{
#include "lua/src/lua.h"
}

namespace EloqKV
{
/**
 * Handler of Redis command result.
 */
class LuaOutputHandler : public OutputHandler
{
public:
    explicit LuaOutputHandler(lua_State *lua) : lua_(lua)
    {
    }
    void OnBool(bool b) override;
    void OnString(std::string_view str) override;
    void OnInt(int64_t val) override;
    void OnArrayStart(unsigned len) override;
    void OnArrayEnd() override;
    void OnNil() override;
    void OnStatus(std::string_view str) override;
    void OnError(std::string_view str) override;
    void OnFormatError(const char *fmt, ...) override;

    bool HasError();

private:
    void UpdateArray()
    {
        if (!array_index_.empty())
        {
            lua_rawseti(
                lua_, -2, array_index_.back()++); /* set table at key `i' */
        }
    }

    std::vector<unsigned> array_index_;
    lua_State *lua_;
    bool has_error_{false};
};

}  // namespace EloqKV
