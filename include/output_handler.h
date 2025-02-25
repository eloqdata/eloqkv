#pragma once

#include <cstdint>
#include <string_view>

namespace EloqKV
{
/**
 * The handler class to handle command results. For now, only RESP2 protocol is
 * supported.
 */
class OutputHandler
{
public:
    virtual ~OutputHandler() = default;

    virtual void OnBool(bool b) = 0;
    virtual void OnString(std::string_view str) = 0;
    virtual void OnInt(int64_t val) = 0;
    virtual void OnArrayStart(unsigned len) = 0;
    virtual void OnArrayEnd() = 0;
    virtual void OnNil() = 0;
    virtual void OnStatus(std::string_view str) = 0;
    virtual void OnError(std::string_view str) = 0;
    virtual void OnFormatError(const char *fmt, ...) = 0;
};
}  // namespace EloqKV
