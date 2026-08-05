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

// The paged hash's PAGE FORMAT and TYPE METADATA (docs/08-paged-objects.md
// §4, §5): the sorted slotted page (PageView) and the metadata row's type
// section (PagedHashMetadata). The page-management vocabulary and machinery
// live engine-side in tx_service/include/page_frame_table.h; the type class
// that composes both is RedisPagedHashObject (redis_paged_hash_object.h).
//
// On-disk format notes (wet cement, §5):
// - All multi-byte integers in the page header, slot arrays, and metadata
//   sections are LITTLE-ENDIAN, written through the shared helpers
//   (txservice::paged_detail).
// - The type section serialized here follows the envelope ([type tag][ttl?]
//   [format version], written by the object) and the page-manager section
//   (written by PageFrameTable) in the metadata row (§5).
// - page_entry_counts_ serializes in DIRECTORY ORDER (first occurrence of
//   each page id), never map iteration order: replay determinism tests diff
//   serialized layouts byte-for-byte.
// - Pending-delete ranges (page-manager section) persist without their
//   freed_ts_ (§5): after a reload no flush is in flight, so every reloaded
//   range is drainable by the first checkpoint that writes its Deletes.
//
// Not implemented in v1 (§14):
// - Large-value RUN management (§4): the page-level descriptor encoding IS
//   implemented and pinned — the record length varint's low bit is the
//   inline/out-of-line indicator, (len << 1) | is_large, with total_length
//   in the length field and the 4-byte first_page_id as the body — but
//   allocation and rewrite are absent; Put asserts values fit a page.
// - Opportunistic merges (§6): discretionary by design, absent here.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tx_service/include/page_frame_table.h"

namespace EloqKV
{
// The page-management vocabulary is engine-defined (docs/08 §4 "Ownership:
// two layers"): frames, pins, faults, and the id lifecycle live in
// txservice::PageFrameTable; this header holds only what a page MEANS for a
// hash — the page format (PageView) and the type metadata (§5's type
// section).
using PageId = txservice::PageId;
inline constexpr PageId kInvalidPageId = txservice::kInvalidPageId;
using txservice::FreeRanges;
using txservice::PageBuf;
using txservice::PageIdRange;
using txservice::PageSlot;
using txservice::PendingDelete;
using txservice::PendingDeletes;
namespace paged_detail = txservice::paged_detail;

// Forced-split valve for the uint16 per-page entry count (§4).
inline constexpr uint32_t kMaxEntriesPerPage = 65535;

// Directory depth bound: routing and intra-page order share the top 32 hash
// bits (§4), so splits are well-defined only while local depth < 32. The §4
// sizing table puts real workloads at depth ~17; reaching 32 is a hash-flood
// pathology, guarded by assert.
inline constexpr uint8_t kMaxDepth = 32;

// The injected field hash: must be the pinned production hash
// (redis_paged_defs.h) everywhere outside tests. Tests inject adversarial
// hashes to force splits and equal-hash runs.
using FieldHashFn = uint64_t (*)(std::string_view);

/**
 * @brief One out-of-line large value's page run (§4). Persisted in the
 * metadata; not yet produced by this core (see the header note).
 */
struct LargeRun
{
    std::vector<PageId> page_ids_;
    uint64_t total_length_{0};
};

/**
 * @brief Non-owning typed view over one page buffer — the §4 sorted slotted
 * layout, mutated in place. The buffer bytes ARE the on-disk row.
 *
 * Layout:
 * @verbatim
 *   [header: version u8 | flags u8 | local_depth u8 | reserved u8 |
 *            free_offset u32 | dead_bytes u32]                      (12 B)
 *   [hash32[entry_count] ascending][offset[entry_count]]     (slot arrays)
 *   ... free space ...
 *   [data records, growing DOWN from the page end:
 *        varint klen | key | varint vlen | value]
 * @endverbatim
 *
 * entry_count is NOT in the page (§4): it lives in the metadata and is
 * passed in. free_offset is the lowest byte of the used data region.
 */
class PageView
{
public:
    static constexpr size_t kHeaderSize = 12;
    static constexpr uint8_t kPageLayoutVersion = 1;
    static constexpr size_t kSlotSize = 8;  // hash32 + offset
    static constexpr size_t kNpos = SIZE_MAX;

    PageView(uint8_t *data, uint32_t page_size, uint32_t entry_count)
        : data_(data), page_size_(page_size), entry_count_(entry_count)
    {
        assert(page_size_ > kHeaderSize + kSlotSize);
    }

    /**
     * @brief Formats an empty page in place. Zeroes the WHOLE buffer, not
     * just the header: page bytes are the on-disk row (§4), so uninitialized
     * free space would leak heap residue into the store and break the §16
     * byte-identical layout determinism that replay tests diff on.
     */
    void Init(uint8_t local_depth)
    {
        std::memset(data_, 0, page_size_);
        data_[0] = kPageLayoutVersion;
        data_[2] = local_depth;
        paged_detail::StoreU32(data_ + 4, page_size_);  // free_offset
        paged_detail::StoreU32(data_ + 8, 0);           // dead_bytes
        entry_count_ = 0;
    }

    uint8_t LayoutVersion() const
    {
        return data_[0];
    }

    uint8_t LocalDepth() const
    {
        return data_[2];
    }

    void SetLocalDepth(uint8_t d)
    {
        data_[2] = d;
    }

    uint32_t FreeOffset() const
    {
        return paged_detail::LoadU32(data_ + 4);
    }

    uint32_t DeadBytes() const
    {
        return paged_detail::LoadU32(data_ + 8);
    }

    uint32_t EntryCount() const
    {
        return entry_count_;
    }

    uint32_t Hash32At(size_t i) const
    {
        return paged_detail::LoadU32(data_ + kHeaderSize + 4 * i);
    }

    std::string_view KeyAt(size_t i) const
    {
        return ParseRecordAt(i).key_;
    }

    /**
     * @brief The inline value bytes of slot i. Must not be called on an
     * out-of-line record — check IsLargeAt first.
     */
    std::string_view ValueAt(size_t i) const
    {
        ParsedRecord rec = ParseRecordAt(i);
        assert(!rec.is_large_);
        return std::string_view(reinterpret_cast<const char *>(rec.body_),
                                static_cast<size_t>(rec.len_));
    }

    /**
     * @brief True iff slot i holds an out-of-line descriptor — the low bit
     * of the record's length varint (§4).
     */
    bool IsLargeAt(size_t i) const
    {
        return ParseRecordAt(i).is_large_;
    }

    /**
     * @brief The out-of-line value's total length (the descriptor's length
     * field). Slot i must be large.
     */
    uint64_t LargeTotalLengthAt(size_t i) const
    {
        ParsedRecord rec = ParseRecordAt(i);
        assert(rec.is_large_);
        return rec.len_;
    }

    /**
     * @brief The out-of-line run's first page id (the descriptor body).
     * Slot i must be large.
     */
    PageId LargeFirstPageIdAt(size_t i) const
    {
        ParsedRecord rec = ParseRecordAt(i);
        assert(rec.is_large_);
        return paged_detail::LoadU32(rec.body_);
    }

    /**
     * @brief Binary search over the dense hash32 array, then key compare
     * within the (almost always single-entry) equal-hash run.
     * @return The slot index, or kNpos.
     */
    size_t Find(uint32_t hash32, std::string_view key) const
    {
        size_t lo = LowerBound(hash32);
        for (size_t i = lo; i < entry_count_ && Hash32At(i) == hash32; ++i)
        {
            if (KeyAt(i) == key)
            {
                return i;
            }
        }
        return kNpos;
    }

    /**
     * @brief First slot index whose hash32 >= h (the HSCAN §12 resume
     * primitive).
     */
    size_t LowerBound(uint32_t h) const
    {
        size_t lo = 0;
        size_t hi = entry_count_;
        while (lo < hi)
        {
            size_t mid = (lo + hi) / 2;
            if (Hash32At(mid) < h)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        return lo;
    }

    /**
     * @brief Footprint of an inline record. The length varint carries
     * (vlen << 1), tag bit clear (§4).
     */
    /**
     * @brief Structural validation of a page image just read from the store
     * (docs/08 §13). Every later accessor — ParseRecordAt, Find, the scan —
     * trusts the header, the slot array, and the record framing, so a
     * corrupt or truncated image must be rejected HERE rather than produce
     * out-of-bounds reads or silently missing fields.
     *
     * Checks, in order of what they protect: the layout version this build
     * understands; the slot array fitting below free_offset; free_offset and
     * dead_bytes inside the page; every slot offset landing in the record
     * area; every record's two varints parsing within the page and its body
     * fitting; and the slot hashes being non-decreasing, which the binary
     * search in Find and the ordered scan both depend on.
     *
     * `entry_count` is supplied by the caller because it lives in the
     * metadata row, not the page header (§4) — validating the image against
     * it is exactly the metadata-vs-page cross-check.
     *
     * @return true if the image is self-consistent at `entry_count` entries.
     */
    static bool ValidateImage(const uint8_t *data,
                              uint32_t page_size,
                              uint32_t entry_count)
    {
        if (data == nullptr || page_size <= kHeaderSize + kSlotSize)
        {
            return false;
        }
        if (data[0] != kPageLayoutVersion)
        {
            return false;
        }
        if (data[2] > 32)  // local depth; routing bits come from a u32 hash
        {
            return false;
        }
        // The slot array must fit below the record area.
        uint64_t slots_end = static_cast<uint64_t>(kHeaderSize) +
                             static_cast<uint64_t>(entry_count) * kSlotSize;
        if (slots_end > page_size)
        {
            return false;
        }
        uint32_t free_offset = paged_detail::LoadU32(data + 4);
        uint32_t dead_bytes = paged_detail::LoadU32(data + 8);
        if (free_offset < slots_end || free_offset > page_size ||
            dead_bytes > page_size)
        {
            return false;
        }
        const uint8_t *end = data + page_size;
        uint32_t prev_hash = 0;
        for (uint32_t i = 0; i < entry_count; ++i)
        {
            uint32_t hash = paged_detail::LoadU32(data + kHeaderSize + 4 * i);
            uint32_t off = paged_detail::LoadU32(data + kHeaderSize +
                                                 4 * entry_count + 4 * i);
            if (i > 0 && hash < prev_hash)
            {
                return false;  // Find's binary search assumes ascending
            }
            prev_hash = hash;
            if (off < free_offset || off >= page_size)
            {
                return false;
            }
            // Mirror ParseRecordAt, but bounded and without its asserts.
            const uint8_t *start = data + off;
            uint64_t klen = 0;
            const uint8_t *p = paged_detail::ReadVarint(start, end, klen);
            if (p == nullptr || klen > static_cast<uint64_t>(end - p))
            {
                return false;
            }
            p += klen;
            uint64_t tagged = 0;
            p = paged_detail::ReadVarint(p, end, tagged);
            if (p == nullptr)
            {
                return false;
            }
            bool is_large = (tagged & 1) != 0;
            uint64_t body = is_large ? 4u : (tagged >> 1);
            if (body > static_cast<uint64_t>(end - p))
            {
                return false;
            }
        }
        return true;
    }

    static size_t RecordSize(std::string_view key, std::string_view value)
    {
        return paged_detail::VarintSize(key.size()) + key.size() +
               paged_detail::VarintSize(value.size() << 1) + value.size();
    }

    /**
     * @brief Footprint of an out-of-line descriptor record: the length
     * varint carries (total_length << 1) | 1 and the body is the 4-byte
     * first page id (§4).
     */
    static size_t LargeRecordSize(std::string_view key, uint64_t total_length)
    {
        return paged_detail::VarintSize(key.size()) + key.size() +
               paged_detail::VarintSize((total_length << 1) | 1) + 4;
    }

    /**
     * @brief Free bytes between the slot arrays and the data region.
     */
    size_t FreeSpace() const
    {
        size_t slots_end = kHeaderSize + kSlotSize * entry_count_;
        size_t free_off = FreeOffset();
        assert(free_off >= slots_end);
        return free_off - slots_end;
    }

    enum class WriteResult
    {
        Inserted,  // new field
        Updated,   // existing field's value replaced
        NeedSplit  // no room even after compaction, or the count valve fired
    };

    /**
     * @brief Inserts or updates an inline <key, value>. May compact in
     * place. Never splits — NeedSplit tells the caller (the directory) to
     * split and retry, which keeps every structural change a pure function
     * of (directory, page contents) for §10 determinism.
     * @param old_value_len On Updated, receives the replaced value's logical
     * length — the inline length, or total_length if the old record was
     * out-of-line (exact logical-bytes maintenance either way).
     */
    WriteResult Write(uint32_t hash32,
                      std::string_view key,
                      std::string_view value,
                      uint64_t *old_value_len = nullptr)
    {
        return WriteRecordCommon(
            hash32,
            key,
            RecordSize(key, value),
            [&](uint32_t rec_off)
            { WriteInlineRecordBytes(rec_off, key, value); },
            old_value_len);
    }

    /**
     * @brief Inserts or updates an out-of-line descriptor record for `key`
     * (§4): length field (total_length << 1) | 1, body first_page_id. Same
     * contract as Write otherwise.
     */
    WriteResult WriteLargeRef(uint32_t hash32,
                              std::string_view key,
                              PageId first_page_id,
                              uint64_t total_length,
                              uint64_t *old_value_len = nullptr)
    {
        return WriteRecordCommon(
            hash32,
            key,
            LargeRecordSize(key, total_length),
            [&](uint32_t rec_off) {
                WriteLargeRecordBytes(
                    rec_off, key, first_page_id, total_length);
            },
            old_value_len);
    }

    /**
     * @brief Erases the slot at index i. Record space is reclaimed lazily
     * via dead_bytes.
     */
    void EraseAt(size_t i)
    {
        assert(i < entry_count_);
        SetDeadBytes(DeadBytes() + static_cast<uint32_t>(RecordSizeAt(i)));
        // Shift the hash array left over i, then the offset array (whose
        // base also moves down one slot).
        uint8_t *hashes = data_ + kHeaderSize;
        uint8_t *offsets = hashes + 4 * entry_count_;
        std::memmove(
            hashes + 4 * i, hashes + 4 * (i + 1), 4 * (entry_count_ - i - 1));
        // Offsets: drop entry i and slide the whole array down 4 bytes.
        std::memmove(hashes + 4 * (entry_count_ - 1), offsets, 4 * i);
        std::memmove(hashes + 4 * (entry_count_ - 1) + 4 * i,
                     offsets + 4 * (i + 1),
                     4 * (entry_count_ - i - 1));
        --entry_count_;
    }

    /**
     * @brief Rewrites the data region tightly (slot order preserved),
     * zeroing dead_bytes. Uses a page_size_ scratch buffer.
     */
    void Compact()
    {
        std::unique_ptr<uint8_t[]> scratch(new uint8_t[page_size_]);
        uint32_t write_off = page_size_;
        std::vector<uint32_t> new_offsets(entry_count_);
        for (size_t i = 0; i < entry_count_; ++i)
        {
            size_t rec_size = RecordSizeAt(i);
            write_off -= static_cast<uint32_t>(rec_size);
            std::memcpy(
                scratch.get() + write_off, data_ + OffsetAt(i), rec_size);
            new_offsets[i] = write_off;
        }
        std::memcpy(data_ + write_off,
                    scratch.get() + write_off,
                    page_size_ - write_off);
        for (size_t i = 0; i < entry_count_; ++i)
        {
            SetOffsetAt(i, new_offsets[i]);
        }
        paged_detail::StoreU32(data_ + 4, write_off);  // free_offset
        SetDeadBytes(0);
    }

    /**
     * @brief Splits this page's contents on split_bit (0-based from the MSB
     * of hash32): entries with the bit set move to `high`, which must be a
     * freshly Init()ed page of the same size. Because slots sort by hash32
     * and all entries share the top split_bit bits, the partition is a
     * single point (§4).
     * @return The number of entries moved to `high`.
     */
    uint32_t SplitInto(PageView &high, uint8_t split_bit)
    {
        assert(split_bit < 32);
        assert(high.entry_count_ == 0);
        uint32_t bit_mask = 1u << (31 - split_bit);
        // Partition point: first slot with the split bit set.
        size_t lo = 0;
        size_t hi = entry_count_;
        while (lo < hi)
        {
            size_t mid = (lo + hi) / 2;
            if ((Hash32At(mid) & bit_mask) == 0)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        uint32_t moved = static_cast<uint32_t>(entry_count_ - lo);
        for (size_t i = lo; i < entry_count_; ++i)
        {
            // Raw record copy: inline and out-of-line records move
            // identically, with no re-encoding to get subtly wrong.
            size_t rec_size = RecordSizeAt(i);
            high.InsertSlotRaw(Hash32At(i), data_ + OffsetAt(i), rec_size);
            SetDeadBytes(DeadBytes() + static_cast<uint32_t>(rec_size));
        }
        // The offset array's base is a function of entry_count_, so keeping
        // the low run's offsets readable after the count shrinks requires
        // relocating them to the new base (old base H+4n → new base H+4lo).
        uint8_t *hashes = data_ + kHeaderSize;
        std::memmove(hashes + 4 * lo, hashes + 4 * entry_count_, 4 * lo);
        entry_count_ = static_cast<uint32_t>(lo);
        SetLocalDepth(static_cast<uint8_t>(LocalDepth() + 1));
        high.SetLocalDepth(LocalDepth());
        // The low half keeps its dead high-run bytes until its next
        // compaction; compact eagerly when the split shed most of the page.
        if (DeadBytes() > page_size_ / 2)
        {
            Compact();
        }
        return moved;
    }

    size_t RecordSizeAt(size_t i) const
    {
        return ParseRecordAt(i).size_;
    }

private:
    /**
     * @brief One decoded record. body_ points into the page; views are valid
     * only until the page is next mutated.
     */
    struct ParsedRecord
    {
        std::string_view key_;
        uint64_t len_{0};  // inline value length, or total_length when large
        bool is_large_{false};
        const uint8_t *body_{nullptr};  // len_ bytes, or 4-byte first page id
        size_t size_{0};                // full record footprint
    };

    ParsedRecord ParseRecordAt(size_t i) const
    {
        const uint8_t *start = data_ + OffsetAt(i);
        const uint8_t *end = data_ + page_size_;
        ParsedRecord out;
        uint64_t klen = 0;
        const uint8_t *p = paged_detail::ReadVarint(start, end, klen);
        assert(p != nullptr);
        out.key_ = std::string_view(reinterpret_cast<const char *>(p),
                                    static_cast<size_t>(klen));
        p += klen;
        uint64_t tagged = 0;
        p = paged_detail::ReadVarint(p, end, tagged);
        assert(p != nullptr);
        out.is_large_ = (tagged & 1) != 0;
        out.len_ = tagged >> 1;
        out.body_ = p;
        size_t body_size = out.is_large_ ? 4 : static_cast<size_t>(out.len_);
        out.size_ = static_cast<size_t>(p - start) + body_size;
        return out;
    }

    /**
     * @brief The shared insert-or-update machinery behind Write and
     * WriteLargeRef: find, space-check (compacting when reclaimable), emit
     * the new record bytes, and repoint or slot-insert.
     */
    template <typename EmitFn>
    WriteResult WriteRecordCommon(uint32_t hash32,
                                  std::string_view key,
                                  size_t rec_size,
                                  EmitFn &&emit,
                                  uint64_t *old_value_len)
    {
        size_t existing = Find(hash32, key);
        if (existing != kNpos)
        {
            ParsedRecord old = ParseRecordAt(existing);
            if (old_value_len != nullptr)
            {
                *old_value_len = old.len_;
            }
            if (FreeSpace() < rec_size)
            {
                // The old record survives compaction (its slot still points
                // at it), so it cannot be reclaimed to make room here.
                if (FreeSpace() + DeadBytes() < rec_size)
                {
                    return WriteResult::NeedSplit;
                }
                Compact();
            }
            uint32_t rec_off = FreeOffset() - static_cast<uint32_t>(rec_size);
            emit(rec_off);
            paged_detail::StoreU32(data_ + 4, rec_off);
            SetOffsetAt(existing, rec_off);
            SetDeadBytes(DeadBytes() + static_cast<uint32_t>(old.size_));
            return WriteResult::Updated;
        }
        if (entry_count_ >= kMaxEntriesPerPage)
        {
            return WriteResult::NeedSplit;  // §4 count valve
        }
        if (FreeSpace() < rec_size + kSlotSize)
        {
            if (FreeSpace() + DeadBytes() < rec_size + kSlotSize)
            {
                return WriteResult::NeedSplit;
            }
            Compact();
        }
        uint32_t rec_off = FreeOffset() - static_cast<uint32_t>(rec_size);
        emit(rec_off);
        paged_detail::StoreU32(data_ + 4, rec_off);
        SlotInsert(hash32, rec_off);
        return WriteResult::Inserted;
    }
    uint32_t OffsetAt(size_t i) const
    {
        return paged_detail::LoadU32(data_ + kHeaderSize + 4 * entry_count_ +
                                     4 * i);
    }

    void SetOffsetAt(size_t i, uint32_t off)
    {
        paged_detail::StoreU32(data_ + kHeaderSize + 4 * entry_count_ + 4 * i,
                               off);
    }

    void SetDeadBytes(uint32_t v)
    {
        paged_detail::StoreU32(data_ + 8, v);
    }

    /**
     * @brief Inserts the sorted slot pair for a record already written at
     * rec_off. Space must have been checked by the caller. Move order
     * matters — highest destination first, since both arrays shift upward:
     *   1. offsets [pos..n) move up 8 (one slot for the new hash, one for
     *      the new offset gap at index pos);
     *   2. offsets [0..pos) move up 4 (the new hash only);
     *   3. hashes  [pos..n) move up 4.
     */
    void SlotInsert(uint32_t hash32, uint32_t rec_off)
    {
        size_t pos = UpperBound(hash32);
        uint8_t *hashes = data_ + kHeaderSize;
        uint8_t *offsets = hashes + 4 * entry_count_;
        std::memmove(
            offsets + 4 * pos + 8, offsets + 4 * pos, 4 * (entry_count_ - pos));
        std::memmove(offsets + 4, offsets, 4 * pos);
        std::memmove(
            hashes + 4 * pos + 4, hashes + 4 * pos, 4 * (entry_count_ - pos));
        paged_detail::StoreU32(hashes + 4 * pos, hash32);
        ++entry_count_;
        SetOffsetAt(pos, rec_off);
    }

    /**
     * @brief Copies pre-encoded record bytes into the data region and
     * slot-inserts them — the split path's mover, format-agnostic by
     * construction.
     */
    void InsertSlotRaw(uint32_t hash32, const uint8_t *rec, size_t rec_size)
    {
        uint32_t rec_off = FreeOffset() - static_cast<uint32_t>(rec_size);
        std::memcpy(data_ + rec_off, rec, rec_size);
        paged_detail::StoreU32(data_ + 4, rec_off);
        SlotInsert(hash32, rec_off);
    }

    void WriteInlineRecordBytes(uint32_t off,
                                std::string_view key,
                                std::string_view value)
    {
        uint8_t *p = data_ + off;
        p = paged_detail::WriteVarint(p, key.size());
        std::memcpy(p, key.data(), key.size());
        p += key.size();
        p = paged_detail::WriteVarint(p, value.size() << 1);  // tag bit 0
        std::memcpy(p, value.data(), value.size());
    }

    void WriteLargeRecordBytes(uint32_t off,
                               std::string_view key,
                               PageId first_page_id,
                               uint64_t total_length)
    {
        uint8_t *p = data_ + off;
        p = paged_detail::WriteVarint(p, key.size());
        std::memcpy(p, key.data(), key.size());
        p += key.size();
        p = paged_detail::WriteVarint(p, (total_length << 1) | 1);
        paged_detail::StoreU32(p, first_page_id);
    }

    /**
     * @brief First slot index whose hash32 > h (insertion point keeping
     * ascending order stable for equal hashes: new entries append after
     * their run).
     */
    size_t UpperBound(uint32_t h) const
    {
        size_t lo = 0;
        size_t hi = entry_count_;
        while (lo < hi)
        {
            size_t mid = (lo + hi) / 2;
            if (Hash32At(mid) <= h)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        return lo;
    }

    uint8_t *data_;
    uint32_t page_size_;
    uint32_t entry_count_;
};

/**
 * @brief The metadata row's TYPE SECTION (§5): the hash's persisted
 * knowledge — routing directory, per-page entry counts, large-value runs —
 * plus its codec. The row is [envelope][page-manager section][this]; the
 * envelope (type tag, ttl, format version) is written by the object wrapper
 * and the page-manager section (page size, id high-water, pending deletes)
 * by txservice::PageFrameTable's own codec.
 */
struct PagedHashMetadata
{
    uint8_t hash_algo_id_{0};
    uint8_t global_depth_{0};
    uint64_t field_count_{0};
    uint64_t logical_bytes_{0};
    std::vector<PageId> dir_;
    std::unordered_map<PageId, uint16_t> page_entry_counts_;
    std::vector<LargeRun> large_runs_;

    /**
     * @brief Invokes fn once per live hash page, in directory order. A page
     * with local depth d is shared by 2^(gd-d) CONSECUTIVE dir entries, so
     * "first occurrence" is testable against the previous entry alone — no
     * set needed, and the order is deterministic.
     */
    template <typename Fn>
    void ForEachDirFirstOccurrence(Fn &&fn) const
    {
        for (size_t i = 0; i < dir_.size(); ++i)
        {
            if (i == 0 || dir_[i] != dir_[i - 1])
            {
                fn(dir_[i]);
            }
        }
    }

    /**
     * @brief Exact byte size Serialize() will append. Kept adjacent to
     * Serialize so the two stay in step; the engine sizes flush buffers from
     * this, so an over- or under-estimate is a corruption bug, not a
     * performance one.
     */
    size_t SerializedSize() const
    {
        // hash_algo + global_depth + field_count + logical_bytes.
        size_t n = 1 + 1 + 8 + 8;
        n += 4 * dir_.size();
        size_t live_pages = 0;
        ForEachDirFirstOccurrence([&](PageId) { ++live_pages; });
        n += 2 * live_pages;
        n += 4;  // large-run count
        for (const LargeRun &run : large_runs_)
        {
            n += 4 + 4 * run.page_ids_.size() + 8;
        }
        return n;
    }

    /**
     * @brief Appends the serialized type section to out; §5 layout, entry
     * counts in directory order (first occurrence).
     */
    void Serialize(std::string &out) const
    {
        using namespace paged_detail;
        uint8_t tmp[8];
        out.push_back(static_cast<char>(hash_algo_id_));
        out.push_back(static_cast<char>(global_depth_));
        StoreU64(tmp, field_count_);
        out.append(reinterpret_cast<char *>(tmp), 8);
        StoreU64(tmp, logical_bytes_);
        out.append(reinterpret_cast<char *>(tmp), 8);
        for (PageId id : dir_)
        {
            StoreU32(tmp, id);
            out.append(reinterpret_cast<char *>(tmp), 4);
        }
        // Entry counts in dir-first-occurrence order (canonical bytes).
        ForEachDirFirstOccurrence(
            [&](PageId id)
            {
                auto it = page_entry_counts_.find(id);
                assert(it != page_entry_counts_.end());
                StoreU16(tmp, it->second);
                out.append(reinterpret_cast<char *>(tmp), 2);
            });
        StoreU32(tmp, static_cast<uint32_t>(large_runs_.size()));
        out.append(reinterpret_cast<char *>(tmp), 4);
        for (const LargeRun &run : large_runs_)
        {
            StoreU32(tmp, static_cast<uint32_t>(run.page_ids_.size()));
            out.append(reinterpret_cast<char *>(tmp), 4);
            for (PageId id : run.page_ids_)
            {
                StoreU32(tmp, id);
                out.append(reinterpret_cast<char *>(tmp), 4);
            }
            StoreU64(tmp, run.total_length_);
            out.append(reinterpret_cast<char *>(tmp), 8);
        }
    }

    /**
     * @brief Parses the type section from [buf+offset, buf+len); advances
     * offset. The format-version byte is validated by the OBJECT against the
     * envelope before either section parses.
     * @return false on malformed input.
     */
    bool Deserialize(const char *buf, size_t len, size_t &offset)
    {
        using namespace paged_detail;
        const uint8_t *base = reinterpret_cast<const uint8_t *>(buf);
        size_t need = 1 + 1 + 8 + 8;
        if (len - offset < need)
        {
            return false;
        }
        const uint8_t *p = base + offset;
        hash_algo_id_ = p[0];
        global_depth_ = p[1];
        field_count_ = LoadU64(p + 2);
        logical_bytes_ = LoadU64(p + 10);
        offset += need;
        if (global_depth_ > kMaxDepth)
        {
            return false;
        }
        size_t dir_size = size_t{1} << global_depth_;
        if ((len - offset) / 4 < dir_size)
        {
            return false;
        }
        dir_.resize(dir_size);
        for (size_t i = 0; i < dir_size; ++i)
        {
            dir_[i] = LoadU32(base + offset);
            offset += 4;
        }
        page_entry_counts_.clear();
        bool ok = true;
        ForEachDirFirstOccurrence(
            [&](PageId id)
            {
                if (!ok || len - offset < 2)
                {
                    ok = false;
                    return;
                }
                page_entry_counts_.try_emplace(id, LoadU16(base + offset));
                offset += 2;
            });
        if (!ok || len - offset < 4)
        {
            return false;
        }
        uint32_t run_count = LoadU32(base + offset);
        offset += 4;
        large_runs_.clear();
        // Bound the count by what the remaining bytes could possibly encode
        // BEFORE reserving: a corrupt run_count would otherwise reserve
        // gigabytes and throw bad_alloc instead of reporting a malformed row.
        // The smallest possible run is 4 (id count) + 8 (total length).
        if (static_cast<uint64_t>(run_count) > (len - offset) / 12)
        {
            return false;
        }
        large_runs_.reserve(run_count);
        for (uint32_t r = 0; r < run_count; ++r)
        {
            if (len - offset < 4)
            {
                return false;
            }
            uint32_t id_count = LoadU32(base + offset);
            offset += 4;
            if ((len - offset) / 4 < id_count)
            {
                return false;
            }
            LargeRun run;
            run.page_ids_.resize(id_count);
            for (uint32_t i = 0; i < id_count; ++i)
            {
                run.page_ids_[i] = LoadU32(base + offset);
                offset += 4;
            }
            if (len - offset < 8)
            {
                return false;
            }
            run.total_length_ = LoadU64(base + offset);
            offset += 8;
            large_runs_.push_back(std::move(run));
        }
        return true;
    }
};
}  // namespace EloqKV
