/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under either GNU Affero General Public License, version 3, or GNU
 *    General Public License, version 2.
 */

// One bounded parser entry point serves two jobs from docs/08-paged-objects-
// test-plan.md §4.1: the ordinary test target deterministically replays a
// seed corpus plus mutations, while PAGED_BUILD_FUZZERS builds the same body
// with libFuzzer. Store bytes are untrusted, so none of these calls may assert,
// read beyond the supplied span, or allocate from an unbounded encoded count.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "redis_paged_hash_object.h"
#include "tx_service/include/page_key_codec.h"

namespace
{
using namespace EloqKV;

constexpr size_t kMaxInput = 1U << 20;

uint64_t Mix(uint64_t value)
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

uint64_t FieldHash(std::string_view field)
{
    uint64_t hash = 0x504147454446555AULL;
    for (unsigned char byte : field)
    {
        hash = Mix(hash ^ byte);
    }
    return hash;
}

uint32_t LoadPrefixU32(const uint8_t *data, size_t size)
{
    uint32_t value = 0;
    std::memcpy(&value, data, std::min(size, sizeof(value)));
    return value;
}

void ExerciseOne(const uint8_t *data, size_t size)
{
    size = std::min(size, kMaxInput);

    txservice::PageKeyParts parts;
    (void) txservice::DecodePageKey(
        std::string_view(reinterpret_cast<const char *>(data), size), parts);

    // Keep both a naturally bounded count and the raw overflow-oriented count
    // in the corpus. ValidateImage performs its slot-size arithmetic in u64;
    // the raw call proves large encoded counts cannot wrap that calculation.
    uint32_t entry_count = LoadPrefixU32(data, size);
    uint32_t page_size = static_cast<uint32_t>(size);
    (void) PageView::ValidateImage(data, page_size, entry_count);
    if (page_size != 0)
    {
        uint32_t bounded_count =
            entry_count % (page_size / PageView::kSlotSize + 1);
        (void) PageView::ValidateImage(data, page_size, bounded_count);
    }

    RedisPagedHashObject object(&FieldHash);
    size_t offset = 0;
    (void) object.DeserializeBounded(
        reinterpret_cast<const char *>(data), size, offset);
}

#ifndef PAGED_LIBFUZZER
std::vector<std::string> SeedCorpus()
{
    std::vector<std::string> corpus = {
        {},
        std::string(1, '\0'),
        std::string(16, '\x80'),
        std::string(64, '\xFF'),
        std::string("\0EKVPAGE", 8),
    };

    for (txservice::PageRowKind kind : {txservice::PageRowKind::HashPage,
                                        txservice::PageRowKind::LargeValuePage})
    {
        std::string key;
        txservice::EncodePageKey(
            key, std::string_view("binary\0key", 10), kind, UINT32_MAX);
        corpus.push_back(std::move(key));
    }

    std::vector<uint8_t> page(4096);
    PageView view(page.data(), static_cast<uint32_t>(page.size()), 7);
    view.Init(3);
    (void) view.Write(1, "a", "one");
    (void) view.Write(2, std::string_view("b\0", 2), "two");
    corpus.emplace_back(reinterpret_cast<const char *>(page.data()),
                        page.size());

    RedisPagedHashObject object(512, &FieldHash);
    object.Put("a", "one");
    object.Put(std::string_view("b\0", 2), std::string_view("v\0", 2));
    std::string metadata;
    object.Serialize(metadata);
    corpus.push_back(metadata);
    for (size_t cut = 0; cut < metadata.size(); ++cut)
    {
        corpus.push_back(metadata.substr(0, cut));
    }
    return corpus;
}

void Mutate(std::string &bytes, uint64_t &state)
{
    state = Mix(state);
    switch (state % 5)
    {
    case 0:
        if (!bytes.empty())
        {
            bytes.resize(state % (bytes.size() + 1));
        }
        break;
    case 1:
        if (!bytes.empty())
        {
            bytes[state % bytes.size()] ^=
                static_cast<char>((state >> 17) | 1U);
        }
        break;
    case 2:
        if (bytes.size() < kMaxInput)
        {
            bytes.insert(bytes.begin() + (state % (bytes.size() + 1)),
                         static_cast<char>(state >> 29));
        }
        break;
    case 3:
        if (bytes.size() >= sizeof(uint64_t))
        {
            std::fill_n(bytes.begin() + (state % (bytes.size() - 7)),
                        sizeof(uint64_t),
                        static_cast<char>((state >> 41) | 0x80));
        }
        break;
    default:
        bytes.assign(state % 4097, static_cast<char>(state >> 11));
        break;
    }
}
#endif
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    ExerciseOne(data, size);
    return 0;
}

#ifndef PAGED_LIBFUZZER
int main(int argc, char **argv)
{
    uint64_t seed = 0xE10C57612ULL;
    size_t iterations = 10000;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
        {
            seed = std::strtoull(argv[++i], nullptr, 0);
        }
        else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
        {
            iterations =
                static_cast<size_t>(std::strtoull(argv[++i], nullptr, 0));
        }
        else
        {
            std::fprintf(
                stderr, "usage: %s [--seed N] [--iterations N]\n", argv[0]);
            return 2;
        }
    }

    std::vector<std::string> corpus = SeedCorpus();
    for (const std::string &bytes : corpus)
    {
        ExerciseOne(reinterpret_cast<const uint8_t *>(bytes.data()),
                    bytes.size());
    }
    for (size_t i = 0; i < iterations; ++i)
    {
        seed = Mix(seed + i);
        std::string bytes = corpus[seed % corpus.size()];
        unsigned mutations = 1 + static_cast<unsigned>((seed >> 8) % 8);
        for (unsigned mutation = 0; mutation < mutations; ++mutation)
        {
            Mutate(bytes, seed);
        }
        ExerciseOne(reinterpret_cast<const uint8_t *>(bytes.data()),
                    bytes.size());
    }
    std::printf("paged codec corpus: %zu seeds, %zu mutations passed\n",
                corpus.size(),
                iterations);
    return 0;
}
#endif
