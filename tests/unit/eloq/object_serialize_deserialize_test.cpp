#define CATCH_CONFIG_MAIN

#include <time.h>

#include <catch2/catch_all.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "redis_command.h"
#include "redis_errors.h"
#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_zset_object.h"

absl::flat_hash_map<std::string_view, double> EloqKV::RedisZsetObject::*
z_hash_map_();

template <
    absl::flat_hash_map<std::string_view, double> EloqKV::RedisZsetObject::*M>
struct Rob
{
    friend absl::flat_hash_map<std::string_view, double>
        EloqKV::RedisZsetObject::*FiledPtr()
    {
        return M;
    }
};

TEST_CASE("SET serialization preserves the command image and prefix",
          "[set-serialization]")
{
    const size_t value_size = GENERATE(size_t{0},
                                       size_t{1},
                                       size_t{15},
                                       size_t{16},
                                       size_t{1024},
                                       size_t{1 << 20});
    const std::string prefix = GENERATE(std::string{}, std::string("p\0fx", 4));
    std::string value(value_size, 'v');
    if (!value.empty())
    {
        value[value.size() / 2] = '\0';
    }
    const int32_t flags = OBJ_SET_EX | OBJ_SET_XX;
    const uint64_t expiry = 1788869029841;
    EloqKV::SetCommand command(value, flags, expiry);
    std::string image = prefix;
    command.Serialize(image);

    // Encode the established wire layout independently, including embedded
    // NULs.
    const uint8_t type = static_cast<uint8_t>(EloqKV::RedisCommandType::SET);
    const uint32_t length = value.size();
    std::string expected = prefix;
    expected.append(reinterpret_cast<const char *>(&type), sizeof(type));
    expected.append(reinterpret_cast<const char *>(&flags), sizeof(flags));
    expected.append(reinterpret_cast<const char *>(&length), sizeof(length));
    expected.append(value);
    expected.append(reinterpret_cast<const char *>(&expiry), sizeof(expiry));
    REQUIRE(image == expected);

    EloqKV::SetCommand replay;
    replay.Deserialize(std::string_view(image).substr(prefix.size() + 1));
    REQUIRE(replay.value_.StringView() == value);
    REQUIRE(replay.flag_ == flags);
    REQUIRE(replay.obj_expire_ts_ == expiry);
}

TEST_CASE("SET serialization reserves the complete large image",
          "[set-serialization]")
{
    const std::string prefix = GENERATE(std::string{}, std::string(127, 'p'));
    const std::string value(1 << 20, 'v');
    EloqKV::SetCommand command(value);
    const size_t image_size = sizeof(uint8_t) + sizeof(int32_t) +
                              sizeof(uint32_t) + value.size() +
                              sizeof(uint64_t);
    std::string image = prefix;
    std::string reserved = prefix;
    reserved.reserve(prefix.size() + image_size);
    command.Serialize(image);

    REQUIRE(image.size() == prefix.size() + image_size);
    // Compare against this standard library's reserve behavior, not a fixed
    // allocator size class. Without the reservation, the trailing TTL grows
    // the 1 MiB image a second time.
    REQUIRE(image.capacity() == reserved.capacity());

    image.reserve(image.size() + image_size);
    const char *reserved_data = image.data();
    const size_t reserved_capacity = image.capacity();
    command.Serialize(image);
    REQUIRE(image.size() == prefix.size() + 2 * image_size);
    REQUIRE(image.data() == reserved_data);
    REQUIRE(image.capacity() == reserved_capacity);
    REQUIRE(std::string_view(image).substr(prefix.size(), image_size) ==
            std::string_view(image).substr(prefix.size() + image_size));
}

TEST_CASE("zset_object-string")
{
    LOG(INFO) << "running: zset_object-string: ";

    EloqKV::RedisZsetObject before, after;
    std::vector<std::string> s;
    for (int i = 0; i < 26; i++)
    {
        std::variant<std::monostate,
                     std::pair<double, EloqKV::EloqString>,
                     std::vector<std::pair<double, EloqKV::EloqString>>>
            tmp;
        s.emplace_back(std::to_string('a' + i));
        tmp = std::pair<double, EloqKV::EloqString>{
            i, EloqKV::EloqString(s.back().data(), s.back().size())};
        EloqKV::ZParams tmp_params;
        before.CommitZAdd(tmp, tmp_params, false);
    }

    std::string st;
    before.Serialize(st);
    size_t offset = 0;
    after.Deserialize(st.data(), offset);
    REQUIRE((before == after) == true);
}

TEST_CASE("zset_object-vector<char>")
{
    LOG(INFO) << "running: zset_object-vector<char>: ";
    EloqKV::RedisZsetObject before, after;
    std::vector<std::string> s;
    for (int i = 0; i < 26; i++)
    {
        std::variant<std::monostate,
                     std::pair<double, EloqKV::EloqString>,
                     std::vector<std::pair<double, EloqKV::EloqString>>>
            tmp;
        s.emplace_back(std::to_string('a' + i));
        tmp = std::pair<double, EloqKV::EloqString>{
            i, EloqKV::EloqString(s.back().data(), s.back().size())};
        EloqKV::ZParams tmp_params;
        before.CommitZAdd(tmp, tmp_params, false);
    }

    size_t offset = 0;
    std::vector<char> v;
    before.Serialize(v, offset);
    offset = 0;
    std::string st(v.begin(), v.end());
    after.Deserialize(st.data(), offset);
    REQUIRE((before == after) == true);
}

TEST_CASE("hash_object-string")
{
    LOG(INFO) << "running: hash_object-string: ";
    EloqKV::RedisHashObject before, after;
    std::vector<std::pair<EloqKV::EloqString, EloqKV::EloqString>> s;
    for (int i = 0; i < 26; i++)
    {
        std::string ss = std::to_string('a' + i);
        s.emplace_back(EloqKV::EloqString(ss.data(), ss.size()),
                       EloqKV::EloqString(ss.data(), ss.size()));
    }
    before.CommitHset(s);

    std::string st;
    before.Serialize(st);
    size_t offset = 0;
    after.Deserialize(st.data(), offset);
    REQUIRE((before == after) == true);
}

TEST_CASE("hash_object-vector<char>")
{
    LOG(INFO) << "running: hash_object-vector<char>: ";
    EloqKV::RedisHashObject before, after;
    std::vector<std::pair<EloqKV::EloqString, EloqKV::EloqString>> s;
    for (int i = 0; i < 26; i++)
    {
        std::string ss = std::to_string('a' + i);
        s.emplace_back(EloqKV::EloqString(ss.data(), ss.size()),
                       EloqKV::EloqString(ss.data(), ss.size()));
    }
    before.CommitHset(s);

    size_t offset = 0;
    std::vector<char> v;
    before.Serialize(v, offset);
    offset = 0;
    std::string st(v.begin(), v.end());
    after.Deserialize(st.data(), offset);
    REQUIRE((before == after) == true);
}

TEST_CASE("list_object-string")
{
    LOG(INFO) << "running: list_object-string: ";
    EloqKV::RedisListObject before, after;
    std::vector<EloqKV::EloqString> s;
    for (int i = 0; i < 26; i++)
    {
        std::string ss = std::to_string('a' + i);
        s.emplace_back(EloqKV::EloqString(ss.data(), ss.size()));
    }
    before.CommitRPush(s);

    std::string st;
    before.Serialize(st);
    size_t offset = 0;
    after.Deserialize(st.data(), offset);
    REQUIRE((before == after) == true);
}

TEST_CASE("list_object-vector<char>")
{
    LOG(INFO) << "running: list_object-vector<char>: ";
    EloqKV::RedisListObject before, after;
    std::vector<EloqKV::EloqString> s;
    for (int i = 0; i < 26; i++)
    {
        std::string ss = std::to_string('a' + i);
        s.emplace_back(EloqKV::EloqString(ss.data(), ss.size()));
    }
    before.CommitRPush(s);

    size_t offset = 0;
    std::vector<char> v;
    before.Serialize(v, offset);
    offset = 0;
    std::string st(v.begin(), v.end());
    after.Deserialize(st.data(), offset);
    REQUIRE((before == after) == true);
}

TEST_CASE("string_result")
{
    LOG(INFO) << "running: string_result: ";
    unsigned int seed = time(NULL);

    auto generateRandomString = [](int minLength, int maxLength) -> std::string
    {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> lengthDist(minLength, maxLength);

        int length = lengthDist(rng);

        std::uniform_int_distribution<char> charDist('a', 'z');

        std::string randomString;
        randomString.reserve(length);

        for (int i = 0; i < length; ++i)
        {
            randomString += charDist(rng);
        }

        return randomString;
    };

    auto equal = [](EloqKV::RedisStringResult &a, EloqKV::RedisStringResult &b)
    {
        if (a.err_code_ != b.err_code_)
        {
            return false;
        }

        if (a.int_ret_ != b.int_ret_)
        {
            return false;
        }

        if (a.str_ != b.str_)
        {
            return false;
        }

        return true;
    };

    bool flag = true;
    for (int i = 0; i < 1000; i++)
    {
        EloqKV::RedisStringResult before, after;
        before.int_ret_ = rand_r(&seed) % INT32_MAX;
        before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);
        before.str_ = generateRandomString(0, rand_r(&seed) % 1000);

        std::string buf;
        size_t size = 0;
        before.Serialize(buf);

        after.Deserialize(buf.data(), size);

        if (!equal(before, after))
        {
            flag = false;
            break;
        }
    }

    REQUIRE(flag == true);
}

TEST_CASE("int_result")
{
    LOG(INFO) << "running: int_result: ";
    unsigned int seed = time(NULL);

    auto equal = [](EloqKV::RedisIntResult &a, EloqKV::RedisIntResult &b)
    {
        if (a.err_code_ != b.err_code_)
        {
            return false;
        }

        if (a.int_val_ != b.int_val_)
        {
            return false;
        }

        if (a.int_val_ != b.int_val_)
        {
            return false;
        }

        return true;
    };

    bool flag = true;
    for (int i = 0; i < 1000; i++)
    {
        EloqKV::RedisIntResult before, after;
        before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);
        before.int_val_ = rand_r(&seed) % INT64_MAX;

        std::string buf;
        size_t size = 0;
        before.Serialize(buf);

        after.Deserialize(buf.data(), size);

        if (!equal(before, after))
        {
            flag = false;
            break;
        }
    }

    REQUIRE(flag == true);
}

TEST_CASE("list_result")
{
    LOG(INFO) << "running: list_result: ";
    unsigned int seed = time(NULL);
    auto generateRandomString = [](int minLength, int maxLength) -> std::string
    {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> lengthDist(minLength, maxLength);

        int length = lengthDist(rng);

        std::uniform_int_distribution<char> charDist('a', 'z');

        std::string randomString;
        randomString.reserve(length);

        for (int i = 0; i < length; ++i)
        {
            randomString += charDist(rng);
        }

        return randomString;
    };

    auto equal = [](EloqKV::RedisListResult &a, EloqKV::RedisListResult &b)
    {
        if (a.err_code_ != b.err_code_)
        {
            return false;
        }
        if (!(a.result_ == b.result_))
        {
            return false;
        }

        return true;
    };

    EloqKV::RedisListResult before, after;
    before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);

    std::vector<std::string> vs;
    for (int i = 0; i < 1000; i++)
    {
        vs.emplace_back(generateRandomString(0, 1000));
    }
    before.result_ = std::move(vs);

    std::string buf;
    size_t size = 0;
    before.Serialize(buf);
    after.Deserialize(buf.data(), size);

    REQUIRE(equal(before, after) == true);
}

TEST_CASE("zset_result")
{
    LOG(INFO) << "running: zset_result: ";
    unsigned int seed = time(NULL);
    auto generateRandomString = [](int minLength, int maxLength) -> std::string
    {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> lengthDist(minLength, maxLength);

        int length = lengthDist(rng);

        std::uniform_int_distribution<char> charDist('a', 'z');

        std::string randomString;
        randomString.reserve(length);

        for (int i = 0; i < length; ++i)
        {
            randomString += charDist(rng);
        }

        return randomString;
    };

    auto equal = [](EloqKV::RedisZsetResult &a, EloqKV::RedisZsetResult &b)
    {
        if (a.err_code_ != b.err_code_)
        {
            return false;
        }
        if (!(a.result_ == b.result_))
        {
            return false;
        }

        return true;
    };

    EloqKV::RedisZsetResult before, after;
    std::string buf;
    size_t size = 0;

    // string
    before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);
    std::string str = generateRandomString(0, 1000);
    before.result_ = EloqKV::EloqString(str.data(), str.size());

    buf.clear();
    size = 0;
    before.Serialize(buf);
    after.Deserialize(buf.data(), size);
    REQUIRE(equal(before, after) == true);

    // double
    before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);
    before.result_ = static_cast<double>(rand_r(&seed) % INT32_MAX);

    buf.clear();
    size = 0;
    before.Serialize(buf);
    after.Deserialize(buf.data(), size);
    REQUIRE(equal(before, after) == true);

    // int
    before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);
    before.result_ = rand_r(&seed) % INT32_MAX;

    buf.clear();
    size = 0;
    before.Serialize(buf);
    after.Deserialize(buf.data(), size);
    REQUIRE(equal(before, after) == true);

    // vector<string>
    before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);
    std::vector<EloqKV::EloqString> vs;
    for (int i = 0; i < 1000; i++)
    {
        std::string str = generateRandomString(0, 1000);
        vs.emplace_back(str.data(), str.size());
    }
    before.result_ = std::move(vs);

    buf.clear();
    size = 0;
    before.Serialize(buf);
    after.Deserialize(buf.data(), size);
    REQUIRE(equal(before, after) == true);
}

TEST_CASE("hash_result")
{
    LOG(INFO) << "running: hash_result: ";
    unsigned int seed = time(NULL);
    auto generateRandomString = [](int minLength, int maxLength) -> std::string
    {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> lengthDist(minLength, maxLength);

        int length = lengthDist(rng);

        std::uniform_int_distribution<char> charDist('a', 'z');

        std::string randomString;
        randomString.reserve(length);

        for (int i = 0; i < length; ++i)
        {
            randomString += charDist(rng);
        }

        return randomString;
    };

    auto equal = [](EloqKV::RedisHashResult &a, EloqKV::RedisHashResult &b)
    {
        if (a.err_code_ != b.err_code_)
        {
            return false;
        }
        if (!(a.result_ == b.result_))
        {
            return false;
        }

        return true;
    };

    EloqKV::RedisHashResult before, after;
    std::string buf;
    size_t size = 0;

    // string
    before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);
    before.result_ = generateRandomString(0, 1000);

    buf.clear();
    size = 0;
    before.Serialize(buf);
    after.Deserialize(buf.data(), size);
    REQUIRE(equal(before, after) == true);

    buf.clear();
    size = 0;
    before.Serialize(buf);
    after.Deserialize(buf.data(), size);
    REQUIRE(equal(before, after) == true);

    // int64
    before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);
    before.result_ = rand_r(&seed) % INT64_MAX;

    buf.clear();
    size = 0;
    before.Serialize(buf);
    after.Deserialize(buf.data(), size);
    REQUIRE(equal(before, after) == true);

    // vector<string>
    before.err_code_ = rand_r(&seed) % (EloqKV::RD_ERR_LAST + 1);
    std::vector<std::string> vs;
    for (int i = 0; i < 1000; i++)
    {
        vs.emplace_back(generateRandomString(0, 1000));
    }
    before.result_ = std::move(vs);

    buf.clear();
    size = 0;
    before.Serialize(buf);
    after.Deserialize(buf.data(), size);
    REQUIRE(equal(before, after) == true);
}
