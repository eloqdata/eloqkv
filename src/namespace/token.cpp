#include "namespace/token.h"

#include <openssl/rand.h>

#include <cstring>
#include <random>

namespace EloqKV
{

NamespaceToken GenerateRandomToken()
{
    NamespaceToken token;
    if (RAND_bytes(token.bytes, 16) != 1)
    {
        std::random_device rd;
        for (size_t i = 0; i < 4; ++i)
        {
            uint32_t val = rd();
            std::memcpy(&token.bytes[i * 4], &val, 4);
        }
    }

    // Format as UUID Version 4
    token.bytes[6] = (token.bytes[6] & 0x0f) | 0x40;  // Set version to 4
    token.bytes[8] = (token.bytes[8] & 0x3f) | 0x80;  // Set variant to RFC 4122
    token.valid = true;

    return token;
}

}  // namespace EloqKV
