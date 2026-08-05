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

// Paged-object constants pinned under metadata format version 1
// (docs/08-paged-objects.md §5 "Wet Cement"). Everything in this header is
// on-disk format or replay-affecting the moment the first paged object is
// written; none of it may change without a new format version.

#include <cstdint>
#include <string_view>

#include "butil/third_party/murmurhash3/murmurhash3.h"
#include "tx_service/include/page_key_codec.h"

namespace EloqKV
{
// Metadata-row format version (§5). Bump when the metadata layout changes; a
// reader must dispatch on this byte before interpreting the body.
inline constexpr uint8_t kPagedFormatVersion = 1;

// Field-hash algorithm registry (§4): the algorithm + seed pair is recorded
// per object under this id, so a future algorithm change is a new id, never a
// reinterpretation. Id 0 is MurmurHash3_x64_128 with the pinned seed below,
// truncated to its low 64 bits.
inline constexpr uint8_t kPagedHashAlgoMurmur3 = 0;
inline constexpr uint32_t kPagedFieldHashSeed = 0x454B5631;  // "EKV1"

/**
 * @brief The pinned production field hash (§4): stable across processes and
 * releases. It determines page assignment (replay determinism, §10) and
 * on-disk placement, which is why absl::Hash is forbidden here.
 * @return The low 64 bits of MurmurHash3_x64_128 under the pinned seed.
 */
inline uint64_t PagedFieldHash(std::string_view field)
{
    uint64_t out[2];
    butil::MurmurHash3_x64_128(
        field.data(), static_cast<int>(field.size()), kPagedFieldHashSeed, out);
    return out[0];
}

/**
 * @brief True iff a key (full engine key bytes, namespace prefix included)
 * begins with the reserved page-row magic and must be rejected at command
 * parsing and on the RESTORE/import paths (§5).
 */
inline bool IsReservedPagedKey(std::string_view key)
{
    return txservice::HasPageKeyMagic(key);
}
}  // namespace EloqKV
