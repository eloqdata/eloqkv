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

// The paged hash type (docs/08-paged-objects.md §4 "Ownership: two layers"):
// ONE class holding the hash's layout, routing, scan order, metadata codec,
// and commands. The page-management protocol — frames, pins, faults,
// eviction, install, flush — is NOT here: it is inherited from
// txservice::PagedTxObject, which contains the PageFrameTable and implements
// the engine-facing virtuals once for every paged type.
//
// This is the object the CcEntry payload holds for a paged hash: the
// metadata block. Its serialized form IS the metadata row (§5) —
// [type tag][ttl?][format version][page-manager section][type section];
// pages are separate store rows under derived keys and never travel through
// this object's Serialize.

#include <absl/container/flat_hash_map.h>
#include <glog/logging.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "eloq_algorithm.h"
#include "redis_command.h"
#include "redis_hash_object.h"
#include "redis_object.h"
#include "redis_paged_defs.h"
#include "redis_paged_hash_core.h"
#include "redis_string_match.h"
#include "redis_string_num.h"
#include "tx_service/include/paged_tx_object.h"

namespace EloqKV
{
// The store-TTL slack (docs/08 §9 interim scheme). Defined in
// src/redis_service.cpp; declared here so the flush path can add it to the
// metadata row's TTL attribute.
extern uint32_t GetPagedTtlSlackSeconds();
// The write ceiling (== MAX_OBJECT_SIZE) and the whole-object reply ceiling
// (§3). Both defined in src/redis_service.cpp with the flags they read.
extern uint64_t GetPagedMaxObjectSize();
extern uint64_t GetPagedObjectReplyBound();
// Conversion knobs (§11). The threshold is 0 (disabled) by default, which is
// what keeps the feature dark until enabled per cluster.
extern uint64_t GetPagedConvertThreshold();
extern uint32_t GetPagedPageSize();
/**
 * @brief The metadata block of a paged hash, as a TxObject.
 *
 * Two sizes must not be confused (§4, "a live rename hazard"):
 * SerializedLength() is the METADATA ROW size — what the engine sizes flush
 * buffers from — while LogicalBytes() is the whole-object logical size that
 * answers MEMORY USAGE, the conversion threshold, and MAX_OBJECT_SIZE.
 */
class RedisPagedHashObject : public RedisEloqObject,
                             public txservice::PagedTxObject
{
public:
    explicit RedisPagedHashObject(uint32_t page_size,
                                  FieldHashFn hash_fn = &PagedFieldHash)
        : hash_fn_(hash_fn)
    {
        frames_.InitFresh(page_size);
        PageId first = frames_.AllocatePageId();
        assert(first == 0);
        (void) first;
        uint8_t *buf = frames_.CreateDirtyPage(0);
        PageView pv(buf, page_size, 0);
        pv.Init(0);
        meta_.global_depth_ = 0;
        meta_.dir_.assign(1, 0);
        meta_.page_entry_counts_.try_emplace(0, 0);
    }

    /**
     * @brief Builds an empty, page-less shell for Deserialize to fill. The
     * page size is unknown until the metadata row supplies it.
     */
    RedisPagedHashObject() : hash_fn_(&PagedFieldHash)
    {
    }

    /**
     * @brief The same empty shell with an injected field hash — tests load
     * serialized objects built under adversarial hashes and must interpret
     * them with the same function.
     */
    explicit RedisPagedHashObject(FieldHashFn hash_fn) : hash_fn_(hash_fn)
    {
    }

    /**
     * @brief The frames copy (via PagedTxObject's PageFrameTable member)
     * SHARES page buffers — §7 copy-on-write — and drops the volatile
     * per-command state (pins, contexts, faults), which belongs to the block
     * whose slots counted it.
     */
    RedisPagedHashObject(const RedisPagedHashObject &rhs)
        : RedisEloqObject(rhs),
          txservice::PagedTxObject(rhs),
          meta_(rhs.meta_),
          hash_fn_(rhs.hash_fn_)
    {
    }

    RedisPagedHashObject(RedisPagedHashObject &&rhs) noexcept
        : RedisEloqObject(rhs),
          txservice::PagedTxObject(std::move(rhs)),
          meta_(std::move(rhs.meta_)),
          hash_fn_(rhs.hash_fn_)
    {
    }

    ~RedisPagedHashObject() override = default;
    RedisPagedHashObject &operator=(const RedisPagedHashObject &rhs) = delete;
    RedisPagedHashObject &operator=(RedisPagedHashObject &&rhs) = delete;

    txservice::TxRecord::Uptr Clone() const override
    {
        return std::make_unique<RedisPagedHashObject>(*this);
    }

    void Copy(const txservice::TxRecord &rhs) override
    {
        const auto &typed = static_cast<const RedisPagedHashObject &>(rhs);
        frames_ = typed.frames_;
        meta_ = typed.meta_;
        hash_fn_ = typed.hash_fn_;
    }

    /**
     * @brief Reports the BASE type, exactly as the TTL twins do
     * ([03-data-model.md](03-data-model.md) §3): a paged hash is a hash to
     * every client and to every type check, so `TYPE` says "hash",
     * CheckTypeMatch accepts it, and command code never branches on
     * paged-ness to decide *whether* it applies. Only serialization reveals
     * the representation, via the distinct PagedHash tag byte (§5).
     *
     * Getting this wrong is not subtle: returning PagedHash here would make
     * every hash command reject a converted object with WRONGTYPE.
     */
    RedisObjectType ObjectType() const override
    {
        return RedisObjectType::Hash;
    }

    /**
     * @brief The metadata row's byte size: type tag + metadata body. Computed
     * rather than cached — an exact recomputation cannot drift out of sync
     * with the directory, and a wrong value here would mis-size the flush
     * buffer. Caching is a later optimization if profiling asks for it.
     */
    size_t SerializedLength() const override
    {
        return 1 + MetaSerializedSize();
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override
    {
        // The metadata body codec appends to a std::string, so the vector
        // sink goes through one scratch copy. Metadata is ~0.01% of the
        // object (§4 sizing), so this is immaterial next to the page I/O the
        // same flush performs.
        std::string scratch;
        Serialize(scratch);
        buf.resize(offset + scratch.size());
        std::copy(scratch.begin(), scratch.end(), buf.begin() + offset);
        offset += scratch.size();
    }

    void Serialize(std::string &str) const override
    {
        str.append(1, static_cast<int8_t>(RedisObjectType::PagedHash));
        SerializeMeta(str);
    }

    /**
     * @brief Reads the metadata row. Yields a metadata-only object with every
     * page non-resident and the derived free list rebuilt (§5).
     *
     * The TxRecord interface supplies no buffer length, so THIS entry point
     * trusts its input — it serves the WAL/replay path, whose images the
     * node wrote itself. The STORE path does not come through here: it goes
     * through RedisEloqObject::DeserializeObject's bounded overload, which
     * calls DeserializeBounded with the row's real length and surfaces
     * failure as a null object (docs/08 §5) — a store row is never trusted.
     */
    void Deserialize(const char *buf, size_t &offset) override
    {
        bool ok = DeserializeBounded(buf, SIZE_MAX, offset);
        assert(ok && "malformed paged-hash metadata in a self-written image");
        (void) ok;
    }


    /**
     * @brief THE metadata-row parser (§5): every entry point funnels here.
     * `len` is the row's actual byte count, or SIZE_MAX when the caller
     * cannot supply one (the WAL path, whose images are self-written) —
     * SIZE_MAX disables the buffer-end checks, never any validation.
     * @return false on any framing or validation failure — including a wrong
     * type tag or a buffer too short to hold one. On failure this object's
     * state is unspecified and the caller must discard it.
     */
    bool DeserializeBounded(const char *buf, size_t len, size_t &offset)
    {
        if (offset >= len)
        {
            return false;
        }
        RedisObjectType obj_type =
            static_cast<RedisObjectType>(*(buf + offset));
        if (obj_type != RedisObjectType::PagedHash)
        {
            assert(len != SIZE_MAX && "wrong tag in a self-written image");
            return false;
        }
        offset += 1;
        return DeserializeSections(buf, len, offset);
    }

    /**
     * @brief Interface-completeness, NOT a live path today. SetEncodedBlob
     * is how the GENERIC-materialization store handlers (e.g. BigTable's
     * payload/unpack_info scheme) install fetched rows; the DSS backends
     * EloqKV runs on go through DeserializeObject instead, and §11's gating
     * keeps paged rows off non-DSS backends entirely. Implemented anyway
     * because the base default is assert(false) — a silent no-op in Release
     * — and a correct funnel into the one parser is strictly safer than
     * that trap if a future handler ever reaches here.
     */
    void SetEncodedBlob(const unsigned char *blob_ptr,
                        size_t blob_size) override
    {
        size_t offset = 0;
        bool ok = DeserializeBounded(
            reinterpret_cast<const char *>(blob_ptr), blob_size, offset);
        assert(ok && "malformed paged-hash blob");
        (void) ok;
    }

    /**
     * @brief Metadata bytes plus resident page bytes — the first Redis object
     * to report a real figure (the monolithic types inherit the TxRecord
     * default of 0 and track SerializedLength() instead, §4). Only this
     * shrinks under partial eviction; LogicalBytes() does not.
     */
    size_t MemUsage() const override
    {
        return sizeof(RedisPagedHashObject) + MetaSerializedSize() +
               ResidentBytes();
    }

    /**
     * @brief The whole-object logical size: what MEMORY USAGE, the conversion
     * threshold, and MAX_OBJECT_SIZE compare against (§4). Distinct from
     * SerializedLength(), which is only the metadata row.
     */
    uint64_t LogicalBytes() const
    {
        return meta_.logical_bytes_;
    }

    uint64_t FieldCount() const
    {
        return meta_.field_count_;
    }

    std::string ToString() const override
    {
        return "PagedHash(fields=" + std::to_string(FieldCount()) +
               ", pages=" + std::to_string(Meta().dir_.size()) +
               ", logical_bytes=" + std::to_string(LogicalBytes()) + ")";
    }

    // ---- per-command execution (docs/08 §6) ----
    //
    // These mirror RedisHashObject's Execute/Commit pair for the same
    // commands, with one addition: each Execute first makes sure the pages it
    // needs are resident, and if any is not it records the fault and reports
    // it. ExecuteOn then returns Yield having produced no reply and touched
    // nothing — the restart-must-be-side-effect-free rule (§6).
    //
    // Execute is const (the #509 contract: ExecuteOn never mutates); the
    // matching Commit* does the mutation, and asserts residency rather than
    // faulting, since CommitOn must not be able to stall (§6).

    /**
     * @brief HGET. Faults the one page `field` routes to (§4: routing is a
     * pure function of hash(field) and the directory, so this is one lookup).
     * @return false if a page must be fetched first; the reply is untouched.
     */
    bool Execute(HGetCommand &cmd) const
    {
        if (!EnsureFieldResident(cmd.field_.StringView()))
        {
            return false;
        }
        RedisHashResult &hash_result = cmd.result_;
        std::optional<std::string_view> value = Get(cmd.field_.StringView());
        if (!value.has_value())
        {
            hash_result.err_code_ = RD_NIL;
        }
        else
        {
            hash_result.err_code_ = RD_OK;
            // Copied, never a view into page bytes: §6 forbids any reference
            // into page memory surviving a yield, and eviction or a split can
            // move these bytes at any later step.
            hash_result.result_ = std::string(*value);
        }
        return true;
    }

    // ---- Command implementations -------------------------------------
    //
    // Every Execute(XCommand &) below returns the SAME bool: true when the
    // command ran to completion and its result is set; false when it touched
    // a non-resident page, recorded that page in the pending-fault set and did
    // NOTHING else. False is not failure — it is the Yield of §6, and the
    // caller turns it into ExecResult::Yield, fetches the recorded pages and
    // re-runs the command from scratch. A false return therefore leaves the
    // object unmodified and the result untouched; commands that cannot fault
    // (answered from metadata alone) always return true.

    /**
     * @brief HLEN. Answered from the metadata alone — no page is ever
     * touched, so this cannot yield (§3).
     */
    bool Execute(HLenCommand &cmd) const
    {
        RedisHashResult &hash_result = cmd.result_;
        hash_result.result_ = static_cast<int64_t>(FieldCount());
        hash_result.err_code_ = RD_OK;
        return true;
    }

    /**
     * @brief HEXISTS. One page, like HGET.
     */
    bool Execute(HExistsCommand &cmd) const
    {
        // Note HExistsCommand holds the FIELD in a member named key_, not
        // field_ — matching RedisHashObject::Execute(HExistsCommand&), which
        // looks up hash_map_.find(cmd.key_).
        if (!EnsureFieldResident(cmd.key_.StringView()))
        {
            return false;
        }
        RedisHashResult &hash_result = cmd.result_;
        // Existence is signalled by err_code_ ALONE, exactly as
        // RedisHashObject::Execute(HExistsCommand&) does: RD_OK when found,
        // and the result's default (not-found) code left untouched
        // otherwise. Setting result_ here — or forcing RD_OK
        // unconditionally — makes every missing field report as existing.
        if (Get(cmd.key_.StringView()).has_value())
        {
            hash_result.err_code_ = RD_OK;
        }
        return true;
    }

    /**
     * @brief HSET. Faults every page its fields route to — recorded in one
     * pass so the whole set is fetched in a single round rather than one
     * page per yield (§6 "compute the whole set, yield once").
     *
     * The reply counts fields that did not previously exist, which needs the
     * pages resident to determine; that is why a write faults its read set
     * too.
     */
    std::optional<bool> Execute(HSetCommand &cmd) const
    {
        bool all_resident = true;
        for (const auto &pair : cmd.field_value_pairs_)
        {
            if (!EnsureFieldResident(pair.first.StringView()))
            {
                all_resident = false;
            }
        }
        if (!all_resident)
        {
            return std::nullopt;
        }

        // Size limit is checked against the would-be post-image, as every
        // monolithic Execute does ([03-data-model.md](03-data-model.md) §9);
        // for a paged object the figure is the logical size, not the
        // metadata row (§4).
        // A record larger than one page cannot be stored at all (§4, §14),
        // and splitting cannot help. Refuse the command HERE, in Execute,
        // before it reaches the WAL — CommitOn on the replay and standby
        // paths runs without Execute, so a command that got past this point
        // would abort those nodes with no way to reject it.
        RedisHashResult &fit_result = cmd.result_;
        for (const auto &pair : cmd.field_value_pairs_)
        {
            if (!RecordFits(pair.first.StringView(),
                            pair.second.StringView(),
                            frames_.PageSize()))
            {
                fit_result.err_code_ = RD_ERR_PAGED_RECORD_TOO_BIG;
                return false;
            }
        }

        uint64_t delta = 0;
        int64_t added = 0;
        for (const auto &pair : cmd.field_value_pairs_)
        {
            std::string_view field = pair.first.StringView();
            std::optional<std::string_view> existing = Get(field);
            if (!existing.has_value())
            {
                ++added;
                delta += field.size() + pair.second.Length();
            }
            else
            {
                delta += pair.second.Length() - existing->size();
            }
        }
        RedisHashResult &hash_result = cmd.result_;
        if (LogicalBytes() + delta > GetPagedMaxObjectSize())
        {
            hash_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return false;
        }
        hash_result.result_ = added;
        hash_result.err_code_ = RD_OK;
        return true;
    }

    /**
     * @brief HDEL. Same one-round fault-set shape as HSET.
     */
    std::optional<CommandExecuteState> Execute(HDelCommand &cmd) const
    {
        bool all_resident = true;
        for (const EloqString &field : cmd.del_list_)
        {
            if (!EnsureFieldResident(field.StringView()))
            {
                all_resident = false;
            }
        }
        if (!all_resident)
        {
            return std::nullopt;
        }
        int64_t removed = 0;
        for (const EloqString &field : cmd.del_list_)
        {
            if (Get(field.StringView()).has_value())
            {
                ++removed;
            }
        }
        RedisHashResult &hash_result = cmd.result_;
        hash_result.result_ = removed;
        hash_result.err_code_ = RD_OK;
        if (removed == 0)
        {
            return CommandExecuteState::NoChange;
        }
        // An empty collection is an absent key, so the caller deletes it.
        return static_cast<uint64_t>(removed) == FieldCount()
                   ? CommandExecuteState::ModifiedToEmpty
                   : CommandExecuteState::Modified;
    }

    /**
     * @brief HSTRLEN. One page, like HGET.
     */
    bool Execute(HStrLenCommand &cmd) const
    {
        if (!EnsureFieldResident(cmd.field_.StringView()))
        {
            return false;
        }
        RedisHashResult &hash_result = cmd.result_;
        std::optional<std::string_view> value = Get(cmd.field_.StringView());
        if (value.has_value())
        {
            hash_result.result_ = static_cast<int64_t>(value->size());
            hash_result.err_code_ = RD_OK;
        }
        else
        {
            hash_result.err_code_ = RD_NIL;
        }
        return true;
    }

    /**
     * @brief HMGET. Faults every requested field's page in one pass, so the
     * whole set is fetched in a single round (§6).
     *
     * Builds a fresh local and ASSIGNS it into the result variant, exactly as
     * the monolithic version does — so an authoritative re-run after a yield
     * replaces the reply rather than doubling it. (§6 flags accumulate-then-
     * append as a hazard for this family; assigning sidesteps it, and doing
     * it the same way here keeps the two implementations comparable.)
     */
    bool Execute(HMGetCommand &cmd) const
    {
        bool all_resident = true;
        for (const EloqString &field : cmd.fields_)
        {
            if (!EnsureFieldResident(field.StringView()))
            {
                all_resident = false;
            }
        }
        if (!all_resident)
        {
            return false;
        }
        RedisHashResult &hash_result = cmd.result_;
        std::vector<std::optional<std::string>> elements;
        elements.reserve(cmd.fields_.size());
        for (const EloqString &field : cmd.fields_)
        {
            std::optional<std::string_view> value = Get(field.StringView());
            // Copied out, never a view into page bytes (§6).
            elements.emplace_back(value.has_value()
                                      ? std::optional<std::string>(*value)
                                      : std::nullopt);
        }
        hash_result.result_ = std::move(elements);
        hash_result.err_code_ = RD_OK;
        return true;
    }

    /**
     * @brief HSETNX. One page: the existence check and the write share it.
     */
    std::optional<bool> Execute(HSetNxCommand &cmd) const
    {
        if (!EnsureFieldResident(cmd.key_.StringView()))
        {
            return std::nullopt;
        }
        RedisHashResult &hash_result = cmd.result_;
        if (Get(cmd.key_.StringView()).has_value())
        {
            // RD_NIL, matching the monolithic path: OutputResult maps
            // err_code_ alone onto the wire — RD_OK is Redis's 1 ("inserted")
            // and RD_NIL its 0 ("field existed, nothing written"). Setting
            // RD_OK here told the client it won a conditional write that
            // never happened (a reported bug: the no-write decision was
            // correct, the reply was not).
            hash_result.err_code_ = RD_NIL;
            return false;  // exists: no write
        }
        if (!RecordFits(cmd.key_.StringView(),
                        cmd.value_.StringView(),
                        frames_.PageSize()))
        {
            hash_result.err_code_ = RD_ERR_PAGED_RECORD_TOO_BIG;
            return false;
        }
        uint64_t delta = cmd.key_.Length() + cmd.value_.Length();
        if (LogicalBytes() + delta > GetPagedMaxObjectSize())
        {
            hash_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return false;
        }
        hash_result.result_ = 1;
        hash_result.err_code_ = RD_OK;
        return true;
    }

    /**
     * @brief HINCRBY. One page; the increment must read the old value, so the
     * page is in the read set as well as the write set.
     */
    std::optional<bool> Execute(HIncrByCommand &cmd) const
    {
        // The value here is a short number, but the FIELD NAME is
        // client-supplied and can exceed a page on its own (§4, §14).
        if (!RecordFits(cmd.field_.StringView(), "", frames_.PageSize()))
        {
            cmd.result_.err_code_ = RD_ERR_PAGED_RECORD_TOO_BIG;
            return false;
        }
        if (!EnsureFieldResident(cmd.field_.StringView()))
        {
            return std::nullopt;
        }
        RedisHashResult &hash_result = cmd.result_;
        int64_t old_val = 0;
        std::optional<std::string_view> existing = Get(cmd.field_.StringView());
        if (existing.has_value())
        {
            if (!string2ll(existing->data(), existing->size(), old_val))
            {
                hash_result.err_code_ = RD_ERR_HASH_VAL_ERROR;
                return false;
            }
        }
        int64_t incr = cmd.score_;
        if ((incr < 0 && old_val <= 0 && incr <= (LLONG_MIN - old_val)) ||
            (incr > 0 && old_val >= 0 && incr >= (LLONG_MAX - old_val)))
        {
            hash_result.err_code_ = RD_ERR_INCR_OVERFLOW;
            return false;
        }
        // The RECORD the commit will write is field + the RENDERED result,
        // not field + "" (a review hole: a boundary-sized field passed the
        // empty-value check and the commit then exceeded the cap). Rendered
        // exactly as CommitHincrby renders it, checked here — pre-WAL, so
        // the apply paths never meet it.
        std::string rendered = std::to_string(old_val + incr);
        if (!RecordFits(cmd.field_.StringView(), rendered, frames_.PageSize()))
        {
            hash_result.err_code_ = RD_ERR_PAGED_RECORD_TOO_BIG;
            return false;
        }
        hash_result.err_code_ = RD_OK;
        hash_result.result_ = old_val + incr;
        return true;
    }

    /**
     * @brief HINCRBYFLOAT. One page, exactly like HINCRBY; the old value must
     * be read, so the page is in both the read and write set.
     */
    std::optional<bool> Execute(HIncrByFloatCommand &cmd) const
    {
        // The value here is a short number, but the FIELD NAME is
        // client-supplied and can exceed a page on its own (§4, §14).
        if (!RecordFits(cmd.field_.StringView(), "", frames_.PageSize()))
        {
            cmd.result_.err_code_ = RD_ERR_PAGED_RECORD_TOO_BIG;
            return false;
        }
        if (!EnsureFieldResident(cmd.field_.StringView()))
        {
            return std::nullopt;
        }
        RedisHashResult &hash_result = cmd.result_;
        long double old_val = 0;
        std::optional<std::string_view> existing = Get(cmd.field_.StringView());
        if (existing.has_value())
        {
            if (!string2ld(existing->data(), existing->size(), old_val))
            {
                // RD_ERR_FLOAT_VALUE, matching the monolithic path: a value
                // with spaces is rejected as a bad float, not as a generic
                // hash error. The client-visible message differs, which the
                // TCL suite asserts on.
                hash_result.err_code_ = RD_ERR_FLOAT_VALUE;
                return false;
            }
        }
        long double next = old_val + cmd.incr_;
        if (std::isnan(static_cast<double>(next)) ||
            std::isinf(static_cast<double>(next)))
        {
            hash_result.err_code_ = RD_ERR_INCR_NAN_OR_INFINITY;
            return false;
        }
        // Exact record check with the RENDERED result (review hole: the
        // empty-value check under-counted; ld2string's fixed notation can
        // render ~330 bytes for a double-range value). Same render as
        // CommitHIncrByFloat; checked pre-WAL.
        std::string rendered = ld2string(next);
        if (!RecordFits(cmd.field_.StringView(), rendered, frames_.PageSize()))
        {
            hash_result.err_code_ = RD_ERR_PAGED_RECORD_TOO_BIG;
            return false;
        }
        hash_result.result_ = std::move(rendered);
        hash_result.err_code_ = RD_OK;
        return true;
    }

    bool CommitHIncrByFloat(EloqString &field, long double incr)
    {
        if (!EnsureFieldResident(field.StringView()))
        {
            return false;
        }
        long double old_val = 0;
        std::optional<std::string_view> existing = Get(field.StringView());
        if (existing.has_value())
        {
            string2ld(existing->data(), existing->size(), old_val);
        }
        std::string next = ld2string(old_val + incr);
        Put(field.StringView(), next);
        return true;
    }

    /**
     * @brief Applies HSETNX / HINCRBY. Both are single-field writes whose
     * page the matching Execute already made resident (§6).
     */
    bool CommitHSetNx(EloqString &field, EloqString &value)
    {
        if (!EnsureFieldResident(field.StringView()))
        {
            return false;
        }
        Put(field.StringView(), value.StringView());
        return true;
    }

    bool CommitHincrby(EloqString &field, int64_t incr)
    {
        if (!EnsureFieldResident(field.StringView()))
        {
            return false;
        }
        int64_t old_val = 0;
        std::optional<std::string_view> existing = Get(field.StringView());
        if (existing.has_value())
        {
            string2ll(existing->data(), existing->size(), old_val);
        }
        std::string next = std::to_string(old_val + incr);
        Put(field.StringView(), next);
        return true;
    }

    /**
     * @brief HGETALL / HKEYS / HVALS — the whole-object family (§1 non-goal:
     * paging does not make these cheap, and like Redis they materialize).
     *
     * The reply bound is checked BEFORE any fault, from metadata alone, so an
     * oversized object fails fast without loading a single page (§3). Then
     * every live page is faulted in one round.
     *
     * `want` selects fields, values, or both, so one implementation serves
     * all three commands.
     */
    enum class WholeObjectPart
    {
        FieldsAndValues,
        FieldsOnly,
        ValuesOnly
    };

    /**
     * @brief Estimated bytes to MATERIALIZE a reply of `element_count`
     * elements drawn from this object (docs/08 §8).
     *
     * `LogicalBytes()` is field+value bytes only, which badly under-counts what
     * a reply actually costs: every element becomes a `std::string` (control
     * block plus allocator rounding) and carries RESP framing (`$<len>\r\n`
     * … `\r\n`). On small fields that overhead dominates the payload, so a
     * bound applied to logical bytes alone admits replies several times larger
     * than it intends.
     *
     * Deliberately O(1) — both inputs are metadata-resident — so the bound can
     * be checked before a single page is faulted.
     *
     * @param element_count How many reply elements will be built.
     * @return the estimated materialized size in bytes.
     */
    uint64_t EstimatedReplyBytes(uint64_t element_count) const
    {
        // Per element: std::string control block + small-allocation rounding
        // + "$<len>\r\n" and the trailing "\r\n".
        constexpr uint64_t kPerElementOverhead = 48;
        return LogicalBytes() + element_count * kPerElementOverhead;
    }

    bool ExecuteWholeObject(RedisHashResult &hash_result,
                            WholeObjectPart want) const
    {
        // Count what this variant actually emits: HGETALL is two elements per
        // field, HKEYS/HVALS one.
        uint64_t reply_elements =
            FieldCount() * (want == WholeObjectPart::FieldsAndValues ? 2 : 1);
        if (EstimatedReplyBytes(reply_elements) > GetPagedObjectReplyBound())
        {
            hash_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return true;
        }
        if (!EnsureAllResident())
        {
            return false;
        }
        std::vector<std::string> elements;
        elements.reserve(static_cast<size_t>(FieldCount()) *
                         (want == WholeObjectPart::FieldsAndValues ? 2 : 1));
        ForEachEntry(
            [&](std::string_view field, std::string_view value)
            {
                // Copied out: §6 forbids a view into page bytes surviving a
                // yield, and these outlive this call.
                if (want != WholeObjectPart::ValuesOnly)
                {
                    elements.emplace_back(field);
                }
                if (want != WholeObjectPart::FieldsOnly)
                {
                    elements.emplace_back(value);
                }
            });
        hash_result.result_ = std::move(elements);
        hash_result.err_code_ = RD_OK;
        return true;
    }

    bool Execute(HGetAllCommand &cmd) const
    {
        return ExecuteWholeObject(cmd.result_,
                                  WholeObjectPart::FieldsAndValues);
    }

    bool Execute(HKeysCommand &cmd) const
    {
        return ExecuteWholeObject(cmd.result_, WholeObjectPart::FieldsOnly);
    }

    bool Execute(HValsCommand &cmd) const
    {
        return ExecuteWholeObject(cmd.result_, WholeObjectPart::ValuesOnly);
    }

    /**
     * @brief HRANDFIELD. The §3 index-addressed shape: the sampled indices
     * are chosen first (from the metadata's field count alone), each is
     * mapped to its page by prefix-summing the per-page entry counts, and
     * only those pages are faulted — never the whole object.
     *
     * Selection reuses GenRandMap with the same arguments as the monolithic
     * version, so distinct/repeat semantics and output ordering match.
     */
    bool Execute(HRandFieldCommand &cmd) const
    {
        RedisHashResult &result = cmd.result_;
        result.err_code_ = RD_OK;

        uint64_t field_count = FieldCount();
        if (field_count == 0)
        {
            result.result_ = std::vector<std::string>();
            return true;
        }
        assert(field_count <= INT32_MAX);
        int32_t sz = static_cast<int32_t>(field_count);

        bool distinct = cmd.count_ >= 0;
        int64_t count = (cmd.count_ == INT64_MAX ? 1 : cmd.count_);
        count = (count >= 0 ? (count < sz ? count : sz) : -count);
        if (count == 0)
        {
            result.result_ = std::vector<std::string>();
            return true;
        }

        // A NEGATIVE count means "with repeats", so it is NOT clamped to the
        // field count: `HRANDFIELD k -100000000` asks for a reply far larger
        // than the object and is bounded by nothing else (§8). Charge it
        // against the same reply ceiling the whole-object commands use —
        // O(1), before any page is faulted.
        uint64_t reply_elements =
            static_cast<uint64_t>(count) * (cmd.with_values_ ? 2 : 1);
        if (EstimatedReplyBytes(reply_elements) > GetPagedObjectReplyBound())
        {
            result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return true;
        }

        std::multimap<int32_t, int64_t> picks;
        GenRandMap(sz, count, !distinct, picks);

        // Fault every sampled index's page in ONE round (§6), not one page
        // per yield.
        bool all_resident = true;
        for (const auto &[index, slot] : picks)
        {
            if (!EnsureIndexResident(static_cast<uint64_t>(index)))
            {
                all_resident = false;
            }
        }
        if (!all_resident)
        {
            return false;
        }

        std::vector<std::string> elements;
        elements.resize(cmd.with_values_ ? 2 * count : count);
        for (const auto &[index, out_slot] : picks)
        {
            auto entry = EntryAt(static_cast<uint64_t>(index));
            assert(entry.has_value());
            // Copied out, never a view into page bytes (§6).
            if (cmd.with_values_)
            {
                elements[out_slot * 2] = std::string(entry->first);
                elements[out_slot * 2 + 1] = std::string(entry->second);
            }
            else
            {
                elements[out_slot] = std::string(entry->first);
            }
        }
        result.result_ = std::move(elements);
        return true;
    }

    /**
     * @brief HSCAN — the §12 incremental cursor, and the one command paging
     * makes strictly better: the monolithic hash cannot implement a cursor at
     * all, because absl::flat_hash_map has no stable iteration order (see the
     * TODO at redis_hash_object.cpp:582). A stable page directory removes
     * that obstacle.
     *
     * The cursor is a plain hash32 lower bound — 0 starts the scan and 0
     * back means complete, Redis's own convention. See
     * PagedHashCore::ScanStep for why a hash bound, not a directory
     * position, is what survives splits and directory doubling on a
     * TOP-bit-routed directory (and why Redis's reverse-binary advance,
     * designed for low-bit bucket indexing, skips here).
     *
     * COUNT bounds WORK, not results — also Redis's semantics, and what
     * §12's memory-safety claim rests on: the call visits a bounded number
     * of entries (hence pages) and returns, however few of them MATCH lets
     * through. Counting matches instead would make a non-matching pattern
     * walk the entire object in one call, faulting every page of an object
     * that may exceed the shard budget.
     */
    bool Execute(HScanCommand &cmd) const
    {
        RedisHashResult &hash_result = cmd.result_;
        hash_result.err_code_ = RD_OK;

        // A previous reply can only have handed the client a 32-bit bound,
        // so anything outside that range is stale or fabricated: report
        // completion rather than scanning from a made-up position.
        if (cmd.cursor_ < 0 ||
            static_cast<uint64_t>(cmd.cursor_) > 0xFFFFFFFFULL)
        {
            hash_result.result_ = std::vector<std::string>{std::to_string(0)};
            return true;
        }
        uint32_t cursor = static_cast<uint32_t>(cmd.cursor_);
        uint64_t soft_limit = cmd.count_ == 0 ? 10 : cmd.count_;

        std::vector<std::string> out;
        out.emplace_back();  // slot 0 is the returned cursor, filled below
        uint32_t next_cursor = 0;
        bool resident = ScanStep(
            cursor,
            soft_limit,
            &next_cursor,
            [&](std::string_view field, std::string_view value)
            {
                if (cmd.match_ && !stringmatchlen(cmd.pattern_.Data(),
                                                  cmd.pattern_.Length(),
                                                  field.data(),
                                                  field.size(),
                                                  0))
                {
                    return;
                }
                // Copied out, never a view into page bytes (§6).
                out.emplace_back(field);
                if (!cmd.novalues_)
                {
                    out.emplace_back(value);
                }
            });
        if (!resident)
        {
            return false;  // fault recorded; yield
        }
        out[0] = std::to_string(next_cursor);
        hash_result.result_ = std::move(out);
        return true;
    }

    /**
     * @brief Applies an HSET.
     * @return true if applied. false means a page was missing: NOTHING was
     *         written, the missing pages are recorded as pending faults, and
     *         the caller must fetch them and retry (§10). On the ApplyCc path
     *         this cannot happen — Execute made the write set resident — but
     *         replay, standby apply and migration run CommitOn with no
     *         Execute at all (#509), against an object that is metadata-only
     *         straight out of Deserialize.
     */
    bool CommitHset(std::vector<std::pair<EloqString, EloqString>> &elements)
    {
        // DISCOVER, then mutate. Every missing page is recorded in one pass
        // before anything is written, so a "not ready" return leaves the
        // object untouched and the retry sees identical input, and so the
        // fetch set issued is complete rather than one page at a time. Note
        // the deliberate non-short-circuiting accumulate.
        bool ready = true;
        for (auto &[field, value] : elements)
        {
            ready &= EnsureFieldResident(field.StringView());
        }
        if (!ready)
        {
            return false;
        }
        // No ts here by design: CommitOn receives none (one of its call
        // sites applies an *uncommitted* effect). Pages are only marked
        // dirty; StampWrites assigns the commit ts where the payload is
        // installed as committed, which is the first moment anything can
        // observe it — the checkpoint never scans an uncommitted payload
        // (§4).
        for (auto &[field, value] : elements)
        {
            Put(field.StringView(), value.StringView());
        }
        return true;
    }

    /**
     * @brief Applies an HDEL.
     * @param now_empty set only when this returns true: the hash has no
     *        fields left, so the caller deletes the key (Redis semantics — an
     *        empty collection is an absent key).
     * @return true if applied; false means a page was missing and nothing was
     *         deleted (see CommitHset).
     */
    bool CommitHdel(std::vector<EloqString> &fields, bool &now_empty)
    {
        bool ready = true;
        for (EloqString &field : fields)
        {
            ready &= EnsureFieldResident(field.StringView());
        }
        if (!ready)
        {
            return false;
        }
        for (EloqString &field : fields)
        {
            Del(field.StringView());
        }
        now_empty = FieldCount() == 0;
        return true;
    }

    /**
     * @brief Builds the paged representation of a monolithic hash (§11
     * conversion). The caller has already decided the threshold is crossed.
     *
     * Conversion is one-way and deliberately early, so this always runs on a
     * small object (low-single-digit MB) — which is what guarantees the
     * resulting flush fits one atomic batch and makes the "convert a 10 GB
     * hash" case nonexistent. Every page it produces is dirty, so the next
     * checkpoint writes the metadata row and all pages as one indivisible
     * record (§9); the store therefore transitions monolithic -> paged in a
     * single commit, with no staged protocol and no orphan window.
     *
     * Deterministic **per command stream**, which is what §10 requires: the
     * same field sequence always produces a byte-identical layout, so primary,
     * standby and replay agree — they all apply one key's commands in log
     * order. It is NOT order-independent, and does not need to be: page ids
     * are allocated in the order splits occur, so a different insertion order
     * yields the same logical content under a different id assignment.
     */
    static std::unique_ptr<RedisPagedHashObject> FromFields(
        const std::vector<std::pair<std::string_view, std::string_view>>
            &fields,
        uint32_t page_size,
        FieldHashFn hash_fn = &PagedFieldHash)
    {
        auto paged = std::make_unique<RedisPagedHashObject>(page_size, hash_fn);
        // No SetWriteTs: conversion runs inside CommitOn, which has no
        // commit ts. Every page it produces is dirty and takes its ts from
        // StampWrites at the payload-install site (§4).
        for (const auto &[field, value] : fields)
        {
            paged->Put(field, value);
        }
        return paged;
    }  // GCOVR_EXCL_LINE: unreachable dtor code (copy elision)

    /**
     * @brief Does one field+value fit an empty page of `page_size`?
     *
     * A record larger than a page would need the out-of-line large-value
     * path (§4), which v1 does not implement — splitting cannot help, since
     * the record does not fit even alone. Every path that could put such a
     * record into a paged object must consult this first: the write paths
     * refuse the command, and conversion declines to convert (§14).
     */
    /**
     * @brief The inline-record cap: `max(page_size / 8, 4 KB)`, further
     * bounded by what physically fits one page (docs/08 §4).
     *
     * The `page_size / 8` term keeps inline bucket capacity >= 8 at
     * production page sizes: at capacity ~1 the extendible directory
     * degenerates — the required depth is the birthday bound over all hash
     * pairs, ~2*log2(N) bits, i.e. a Θ(N²)-entry directory (measured: tens
     * of MB of directory for thousands of records). Records above the cap
     * belong in §4's out-of-line large-value runs; until those land (§14),
     * writes above the cap are refused outright when conversion is enabled.
     *
     * The 4 KB floor is a deliberate testing affordance: harnesses set small
     * pages (4 KB) for convenience, where a divided cap (512 B) would break
     * every stock-behavior suite. Below 32 KB pages the floor makes the cap
     * degenerate to page capacity — one record per page is ALLOWED there,
     * accepting the degenerate directory in exchange for testability; the
     * capacity guarantee is only claimed for page sizes >= 32 KB (§4).
     */
    static uint32_t InlineRecordCap(uint32_t page_size)
    {
        constexpr uint32_t kCapFloor = 4096;
        uint32_t cap = std::max(page_size / 8, kCapFloor);
        uint32_t page_capacity = page_size > PageView::kHeaderSize
                                     ? page_size - PageView::kHeaderSize
                                     : 0;
        return std::min(cap, page_capacity);
    }

    static bool RecordFits(std::string_view field,
                           std::string_view value,
                           uint32_t page_size)
    {
        return PageView::RecordSize(field, value) + PageView::kSlotSize <=
               InlineRecordCap(page_size);
    }

    /**
     * @brief Is every field of a monolithic hash small enough to page at
     * `page_size`? Conversion is all-or-nothing, so ONE oversized record
     * makes the whole object ineligible and it stays monolithic (§14).
     *
     * A pure function of the object's contents and the page size, so it is
     * as deterministic per command stream as the threshold check beside it
     * (§11): primary, standby and replay reach the same verdict.
     */
    static bool AllRecordsFit(
        const std::vector<std::pair<std::string_view, std::string_view>>
            &fields,
        uint32_t page_size)
    {
        for (const auto &[field, value] : fields)
        {
            if (!RecordFits(field, value, page_size))
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief True if a hash of this logical size should convert (§11). False
     * when the threshold is 0, which disables conversion entirely.
     */
    static bool ShouldConvert(uint64_t logical_bytes)
    {
        uint64_t threshold = GetPagedConvertThreshold();
        return threshold != 0 && logical_bytes >= threshold;
    }

    /**
     * @brief Swaps to the TTL twin, per the class-twin convention
     * ([03-data-model.md](03-data-model.md) §3): EXPIRE and friends change the
     * object's CLASS rather than setting a flag. Moves the core across, so the
     * directory, resident pages and per-txn state transfer without copying.
     *
     * Overriding this is not optional — TxRecord::AddTTL asserts false by
     * default, so EXPIRE on a paged hash would abort the process.
     */
    // Defined out-of-line at the end of this header: the TTL twin is declared
    // below, so the body cannot be inline here.
    txservice::TxRecord::Uptr AddTTL(uint64_t ttl) override;

    // ---- txservice::PagedTxObject: the representation query (§6) --------
    //
    // The pin/fault/shed/install/flush protocol is NOT here: PagedTxObject
    // implements it once over its contained PageFrameTable. This class
    // supplies only the type hooks (below, protected) and its own layout.

    txservice::PagedTxObject *AsPaged() override
    {
        return this;
    }

    const txservice::PagedTxObject *AsPaged() const override
    {
        return this;
    }

    // ---- hash layout: routing, point ops, iteration (§4) ----------------

    const PagedHashMetadata &Meta() const
    {
        return meta_;
    }

    PagedHashMetadata &MutableMeta()
    {
        return meta_;
    }

    /**
     * @brief Sets the provisional ts stamped on subsequent mutations: one
     * CommitOn is one commit_ts for all of its writes (§4).
     */
    void SetWriteTs(uint64_t write_ts)
    {
        frames_.SetWriteTs(write_ts);
    }

    size_t ResidentBytes() const
    {
        return frames_.ResidentBytes();
    }

    /**
     * @brief Routing (§4): top global_depth bits of the 64-bit field hash.
     */
    size_t DirIndex(uint64_t hash64) const
    {
        return meta_.global_depth_ == 0
                   ? 0
                   : static_cast<size_t>(hash64 >> (64 - meta_.global_depth_));
    }

    PageId RouteField(std::string_view field, uint64_t *hash_out) const
    {
        uint64_t h = hash_fn_(field);
        if (hash_out != nullptr)
        {
            *hash_out = h;
        }
        return meta_.dir_[DirIndex(h)];
    }

    std::optional<std::string_view> Get(std::string_view field) const
    {
        uint64_t h = 0;
        PageId pid = RouteField(field, &h);
        PageView pv = View(pid);
        size_t i = pv.Find(static_cast<uint32_t>(h >> 32), field);
        if (i == PageView::kNpos)
        {
            return std::nullopt;
        }
        return pv.ValueAt(i);
    }

    /**
     * @brief Inserts or updates one field. Keeps field_count_ and
     * logical_bytes_ exact on both insert and update.
     * @return true if the field is new (HSET reply semantics).
     */
    bool Put(std::string_view field, std::string_view value)
    {
        // A record that cannot fit an empty page needs the out-of-line
        // large-value path (§4), which is not implemented yet. Every
        // admission guard runs at Execute on the node that ACCEPTED the
        // write, so reaching here means this record was admitted under a
        // DIFFERENT node's configuration — replica threshold/page-size
        // skew, which v1 forbids (§11 deployment invariant). This is the
        // apply path: there is no Execute to reject with (#509), splitting
        // can never place it, and a Debug-only assert would leave Release
        // doubling the directory until memory ran out. Die loudly instead,
        // the same policy replay corruption already follows.
        if (PageView::RecordSize(field, value) + PageView::kSlotSize >
            frames_.PageSize() -
                PageView::kHeaderSize)  // GCOVR_EXCL_START — invariant
                                        // self-check failure arm: every
                                        // admission guard prevents this on
                                        // the accepting node (passing tests
                                        // prove it), and executing it aborts
                                        // the process. It exists to catch
                                        // replica-config skew (§11).
        {
            LOG(FATAL) << "paged hash record (field " << field.size()
                       << " B, value " << value.size()
                       << " B) cannot fit an empty " << frames_.PageSize()
                       << " B page; admitted under skewed replica config "
                          "(docs/08 §11).";
        }  // GCOVR_EXCL_STOP
        for (;;)
        {
            uint64_t h = 0;
            PageId pid = RouteField(field, &h);
            PageView pv = MutableView(pid);
            uint64_t old_value_len = 0;
            PageView::WriteResult r = pv.Write(
                static_cast<uint32_t>(h >> 32), field, value, &old_value_len);
            if (r == PageView::WriteResult::NeedSplit)
            {
                SplitPage(pid);
                continue;
            }
            SetCount(pid, pv.EntryCount());
            frames_.TouchPage(pid);
            if (r == PageView::WriteResult::Inserted)
            {
                ++meta_.field_count_;
                meta_.logical_bytes_ += field.size() + value.size();
                return true;
            }
            meta_.logical_bytes_ += value.size();
            meta_.logical_bytes_ -= old_value_len;
            return false;
        }
    }

    bool Del(std::string_view field)
    {
        uint64_t h = 0;
        PageId pid = RouteField(field, &h);
        PageView pv = MutableView(pid);
        size_t i = pv.Find(static_cast<uint32_t>(h >> 32), field);
        if (i == PageView::kNpos)
        {
            return false;
        }
        uint64_t bytes = field.size() + pv.ValueAt(i).size();
        pv.EraseAt(i);
        SetCount(pid, pv.EntryCount());
        frames_.TouchPage(pid);
        --meta_.field_count_;
        meta_.logical_bytes_ -= bytes;
        return true;
    }

    /**
     * @brief Routes `field` and reports whether its page is resident,
     * recording the page id as a pending fault when it is not — the
     * per-command fault-set computation of §3/§6. Const, mutating only the
     * frame table's volatile pending-fault set (#509: ExecuteOn never
     * mutates).
     * @return true if the field's page is resident.
     */
    bool EnsureFieldResident(std::string_view field) const
    {
        PageId pid = RouteField(field, nullptr);
        if (frames_.IsResident(pid))
        {
            return true;
        }
        frames_.RecordPendingFault(pid);
        return false;
    }

    /**
     * @brief Fault-set computation for whole-object commands (HGETALL and
     * friends): records every live page that is not resident. The caller
     * should check the reply bound against LogicalBytes() *first* (§3).
     * @return true if every live page is already resident.
     */
    bool EnsureAllResident() const
    {
        bool all = true;
        frames_.ForEachLivePageId(
            [&](PageId id)
            {
                if (!frames_.IsResident(id))
                {
                    frames_.RecordPendingFault(id);
                    all = false;
                }
            });
        return all;
    }

    /**
     * @brief Invokes fn(field, value) for every entry, in directory order
     * then intra-page slot order — deterministic, and the ascending-hash
     * order HSCAN walks (§12). Every page must be resident;
     * EnsureAllResident is the matching precondition.
     */
    template <typename Fn>
    void ForEachEntry(Fn &&fn) const
    {
        for (size_t i = 0; i < meta_.dir_.size(); ++i)
        {
            if (i > 0 && meta_.dir_[i] == meta_.dir_[i - 1])
            {
                continue;
            }
            PageView pv = View(meta_.dir_[i]);
            for (size_t slot = 0; slot < pv.EntryCount(); ++slot)
            {
                fn(pv.KeyAt(slot), pv.ValueAt(slot));
            }
        }
    }

    /**
     * @brief The page holding the `index`-th entry in ForEachEntry order,
     * found by prefix-summing the metadata's per-page entry counts (§4).
     * This is why the counts live in the metadata rather than in page
     * headers: index-based selection has to decide *which* page to fault, so
     * reading the count from the page would be circular. Faults nothing.
     * @param first_index_of_page If non-null, receives the index of that
     * page's first entry, so the caller can convert to an intra-page slot.
     * @return kInvalidPageId if `index` is past the last entry.
     */
    PageId PageForIndex(uint64_t index, uint64_t *first_index_of_page) const
    {
        uint64_t seen = 0;
        for (size_t i = 0; i < meta_.dir_.size(); ++i)
        {
            if (i > 0 && meta_.dir_[i] == meta_.dir_[i - 1])
            {
                continue;
            }
            PageId id = meta_.dir_[i];
            auto cit = meta_.page_entry_counts_.find(id);
            assert(cit != meta_.page_entry_counts_.end());
            uint64_t count = cit->second;
            if (index < seen + count)
            {
                if (first_index_of_page != nullptr)
                {
                    *first_index_of_page = seen;
                }
                return id;
            }
            seen += count;
        }
        return kInvalidPageId;
    }

    /**
     * @brief Records a fault for the page holding `index`, if not resident.
     * @return true if that page is already resident.
     */
    bool EnsureIndexResident(uint64_t index) const
    {
        PageId id = PageForIndex(index, nullptr);
        if (id == kInvalidPageId || frames_.IsResident(id))
        {
            return true;
        }
        frames_.RecordPendingFault(id);
        return false;
    }

    /**
     * @brief The `index`-th entry in ForEachEntry order. Its page must be
     * resident (EnsureIndexResident is the matching precondition).
     */
    std::optional<std::pair<std::string_view, std::string_view>> EntryAt(
        uint64_t index) const
    {
        uint64_t first = 0;
        PageId id = PageForIndex(index, &first);
        if (id == kInvalidPageId)
        {
            return std::nullopt;
        }
        PageView pv = View(id);
        size_t slot = static_cast<size_t>(index - first);
        assert(slot < pv.EntryCount());
        return std::make_pair(pv.KeyAt(slot), pv.ValueAt(slot));
    }

    /**
     * @brief One HSCAN step over a single directory bucket (§12): emits
     * entries whose hash32 is >= from_hash32, never splitting an equal-hash
     * run across calls — the run is emitted to its end even past the cap,
     * which is also what keeps the resume hash unambiguous (slots are
     * sorted, so a mid-bucket resume value is never 0; 0 always means
     * "bucket exhausted").
     * @param next_hash32 Receives the hash to resume from within this
     * bucket, or 0 when the bucket is exhausted.
     * @return false if the bucket's page is not resident; the fault is
     * recorded and nothing is emitted.
     */
    template <typename Fn>
    bool ScanBucket(size_t dir_entry,
                    uint32_t from_hash32,
                    uint64_t soft_limit,
                    uint32_t *next_hash32,
                    Fn &&emit) const
    {
        assert(dir_entry < meta_.dir_.size());
        PageId id = meta_.dir_[dir_entry];
        if (!frames_.IsResident(id))
        {
            frames_.RecordPendingFault(id);
            return false;
        }
        PageView pv = View(id);
        *next_hash32 = 0;
        size_t i = pv.LowerBound(from_hash32);
        uint64_t emitted = 0;
        uint32_t last_hash = 0;
        assert(soft_limit > 0);
        while (i < pv.EntryCount())
        {
            uint32_t h = pv.Hash32At(i);
            if (emitted >= soft_limit && h != last_hash)
            {
                *next_hash32 = h;
                return true;
            }
            emit(pv.KeyAt(i), pv.ValueAt(i));
            ++emitted;
            last_hash = h;
            ++i;
        }
        return true;
    }

    /**
     * @brief One HSCAN call (§12): scans forward from `cursor` in ascending
     * hash32 order. The cursor is a hash32 lower bound — what survives
     * splits and directory doubling on a top-bit-routed directory (a
     * directory entry IS a hash-prefix range; doubling refines ranges
     * without reordering them). Redis's reverse-binary advance solves the
     * OPPOSITE layout (low-bit indexing) and both revisits and skips here.
     * `budget` bounds WORK, not results. A page shared by several
     * consecutive directory entries is emitted once.
     * @param next_cursor Receives the resume bound, or 0 when complete.
     * @return false if a needed page is not resident; the fault is recorded.
     */
    template <typename Fn>
    bool ScanStep(uint32_t cursor,
                  uint64_t budget,
                  uint32_t *next_cursor,
                  Fn &&emit) const
    {
        assert(budget > 0);
        uint64_t scanned = 0;
        uint64_t bound = cursor;
        while (bound < (uint64_t{1} << 32))
        {
            uint8_t depth = meta_.global_depth_;
            size_t entry =
                depth == 0 ? 0 : static_cast<size_t>(bound >> (32 - depth));
            uint32_t in_bucket_next = 0;
            bool resident =
                ScanBucket(entry,
                           static_cast<uint32_t>(bound),
                           budget - scanned,
                           &in_bucket_next,
                           [&](std::string_view f, std::string_view v)
                           {
                               ++scanned;
                               emit(f, v);
                           });
            if (!resident)
            {
                return false;
            }
            if (in_bucket_next != 0)
            {
                *next_cursor = in_bucket_next;
                return true;
            }
            // Page exhausted. Jump the bound past every consecutive entry
            // sharing this page, so a shared page is not re-emitted once per
            // entry pointing at it.
            PageId id = meta_.dir_[entry];
            size_t e = entry + 1;
            while (e < meta_.dir_.size() && meta_.dir_[e] == id)
            {
                ++e;
            }
            bound = depth == 0 ? (uint64_t{1} << 32)
                               : (static_cast<uint64_t>(e) << (32 - depth));
            if (scanned >= budget && bound < (uint64_t{1} << 32))
            {
                *next_cursor = static_cast<uint32_t>(bound);
                return true;
            }
        }
        *next_cursor = 0;
        return true;
    }

    /**
     * @brief Serializes the full metadata row + every RESIDENT page, sorted
     * by id — the layout-determinism test hook (§16 diffs these
     * byte-for-byte).
     */
    void SerializeAll(
        std::string &meta_out,
        std::vector<std::pair<PageId, std::string>> &pages_out) const
    {
        Serialize(meta_out);
        pages_out.clear();
        std::vector<PageId> ids;
        frames_.ForEachLivePageId(
            [&](PageId id)
            {
                if (frames_.IsResident(id))
                {
                    ids.push_back(id);
                }
            });
        for (PageId id : ids)
        {
            const txservice::PageSlot *slot = frames_.SlotOf(id);
            pages_out.emplace_back(
                id,
                std::string(reinterpret_cast<const char *>(slot->buf_.get()),
                            frames_.PageSize()));
        }
    }

    /**
     * @brief Test hook: verifies the structural invariants (§4, §16) —
     * directory shape, count consistency, ascending slots, and that every
     * entry routes to the page holding it. Tolerates PARTIAL residency:
     * in-page checks apply only to resident pages (a non-resident page is
     * the normal state of a freshly loaded or partially evicted object).
     */
    // GCOVR_EXCL_START: the failure arms of this checker are unreachable
    // while the invariants hold — which passing tests prove. They exist to
    // catch future regressions, not to be covered. (The pass-through arms
    // are exercised: every unit test asserts CheckInvariants() == true.)
    bool CheckInvariants() const
    {
        if (!frames_.CheckLruInvariants())
        {
            return false;
        }
        if (meta_.dir_.size() != (size_t{1} << meta_.global_depth_))
        {
            return false;
        }
        uint64_t fields = 0;
        for (size_t i = 0; i < meta_.dir_.size(); ++i)
        {
            PageId id = meta_.dir_[i];
            auto cit = meta_.page_entry_counts_.find(id);
            if (cit == meta_.page_entry_counts_.end())
            {
                return false;
            }
            bool first_occurrence =
                (i == 0 || meta_.dir_[i] != meta_.dir_[i - 1]);
            if (first_occurrence)
            {
                fields += cit->second;
            }

            const txservice::PageSlot *slot = frames_.SlotOf(id);
            if (slot == nullptr || slot->buf_ == nullptr)
            {
                continue;  // not resident: nothing in-page to verify
            }
            PageView pv(slot->buf_.get(), frames_.PageSize(), cit->second);
            if (pv.LocalDepth() > meta_.global_depth_)
            {
                return false;
            }
            if (first_occurrence)
            {
                // Hashes ascending; every entry routes here.
                for (size_t s = 0; s < pv.EntryCount(); ++s)
                {
                    if (s > 0 && pv.Hash32At(s) < pv.Hash32At(s - 1))
                    {
                        return false;
                    }
                    uint64_t h = hash_fn_(pv.KeyAt(s));
                    if (meta_.dir_[DirIndex(h)] != id)
                    {
                        return false;
                    }
                }
            }
        }
        return fields == meta_.field_count_ &&
               frames_.FreeList().CheckCanonical();
    }
    // GCOVR_EXCL_STOP

protected:
    // ---- PagedTxObject type hooks ---------------------------------------

    /**
     * @brief Validates a fetched page image for THIS object (§13).
     *
     * Three layers, cheapest first: the page's own framing
     * (PageView::ValidateImage — layout version, slot array, offsets, record
     * varints, ascending hashes) checked against the entry count THIS
     * object's metadata records for the page; the page's local depth against
     * the directory; and routing identity — every entry must route to
     * page_id under this directory, which catches a structurally valid page
     * belonging to some OTHER id being served here. Pages carry no id of
     * their own (§5 keys them externally), so routing IS the identity check.
     */
    bool ValidatePageImage(uint32_t page_id,
                           std::string_view bytes) const override
    {
        if (bytes.size() != frames_.PageSize())
        {
            return false;
        }
        auto cnt_it = meta_.page_entry_counts_.find(page_id);
        if (cnt_it == meta_.page_entry_counts_.end())
        {
            return false;  // not a page this object's directory knows
        }
        const uint8_t *data = reinterpret_cast<const uint8_t *>(bytes.data());
        uint32_t entry_count = cnt_it->second;
        if (!PageView::ValidateImage(data, frames_.PageSize(), entry_count))
        {
            return false;
        }
        PageView pv(
            const_cast<uint8_t *>(data), frames_.PageSize(), entry_count);
        // Local depth cannot exceed the directory's.
        if (pv.LocalDepth() > meta_.global_depth_)
        {
            return false;
        }
        // Routing identity: routing takes the TOP global_depth bits and the
        // slots are sorted by the top 32 hash bits, so every entry's
        // directory slot must point back at this page.
        uint8_t depth = meta_.global_depth_;
        for (uint32_t i = 0; i < entry_count; ++i)
        {
            size_t idx =
                depth == 0
                    ? 0
                    : static_cast<size_t>(pv.Hash32At(i) >> (32 - depth));
            if (idx >= meta_.dir_.size() || meta_.dir_[idx] != page_id)
            {
                return false;
            }
        }
        return true;
    }

    void SerializeMetadataRow(std::string &out) const override
    {
        Serialize(out);
    }

    txservice::PageRowKind PageKind() const override
    {
        return txservice::PageRowKind::HashPage;
    }

    /**
     * @brief The metadata row's store-TTL attribute (§9 interim scheme):
     * logical deadline plus slack, or 0 for "no TTL". The base object has no
     * deadline; the TTL twin overrides.
     */
    uint64_t MetadataRowTtl() const override
    {
        return 0;
    }

public:
    /**
     * @brief The metadata row BODY shared by both twins: format version,
     * then the page-manager section (engine codec), then the type section
     * (§5). The caller has already written its envelope (tag [+ ttl]).
     */
    void SerializeMeta(std::string &str) const
    {
        str.push_back(static_cast<char>(kPagedFormatVersion));
        frames_.SerializeMeta(str);
        meta_.Serialize(str);
    }

    /**
     * @brief Exact byte size SerializeMeta() appends.
     */
    size_t MetaSerializedSize() const
    {
        return 1 + frames_.SerializedSize() + meta_.SerializedSize();
    }

    /**
     * @brief Parses [version][page-manager section][type section] and
     * rebuilds the derived free list from the type-enumerated live set (§5).
     * Shared by both twins, after each has read its own envelope.
     * @return false on malformed input.
     */
    bool DeserializeSections(const char *buf, size_t len, size_t &offset)
    {
        if (len - offset < 1 ||
            static_cast<uint8_t>(buf[offset]) != kPagedFormatVersion)
        {
            return false;
        }
        offset += 1;
        if (!frames_.DeserializeMeta(buf, len, offset))
        {
            return false;
        }
        if (!meta_.Deserialize(buf, len, offset))
        {
            return false;
        }
        // CROSS-SECTION consistency, checkable only now that both sections
        // are in: every live page id the TYPE section names must lie below
        // the high-water the PAGE-MANAGER section declares. A row failing
        // this is corrupt, and acting on it would index the free-list bitmap
        // out of bounds.
        if (!frames_.RebuildFreeRanges(
                [this](auto &&mark)
                {
                    for (PageId id : meta_.dir_)
                    {
                        mark(id);
                    }
                    for (const LargeRun &run : meta_.large_runs_)
                    {
                        for (PageId id : run.page_ids_)
                        {
                            mark(id);
                        }
                    }
                }))
        {
            return false;
        }
        // The per-page entry counts must add up to the object's field count:
        // every field lives in exactly one page, so a mismatch means the two
        // halves of the row disagree about how much data exists.
        uint64_t counted = 0;
        for (const auto &[id, n] : meta_.page_entry_counts_)
        {
            (void) id;
            counted += n;
        }
        if (counted != meta_.field_count_)
        {
            return false;
        }
        return true;
    }

protected:

    // ---- layout internals -----------------------------------------------

    /**
     * @brief Read view over a resident page. Every page access funnels
     * through here or MutableView — the frame table's LRU touch points (§8).
     */
    PageView View(PageId id) const
    {
        auto cit = meta_.page_entry_counts_.find(id);
        assert(cit != meta_.page_entry_counts_.end());
        return PageView(
            frames_.BufForRead(id), frames_.PageSize(), cit->second);
    }

    /**
     * @brief Write view: copy-on-write via the frame table (§7), then a view
     * over a buffer this object exclusively owns.
     */
    PageView MutableView(PageId id)
    {
        auto cit = meta_.page_entry_counts_.find(id);
        assert(cit != meta_.page_entry_counts_.end());
        return PageView(
            frames_.BufForWrite(id), frames_.PageSize(), cit->second);
    }

    void SetCount(PageId id, uint32_t count)
    {
        assert(count <= kMaxEntriesPerPage);
        auto it = meta_.page_entry_counts_.find(id);
        assert(it != meta_.page_entry_counts_.end());
        it->second = static_cast<uint16_t>(count);
    }

    /**
     * @brief Splits page `pid` on its next routing bit; doubles the
     * directory when local depth would exceed global depth (§4).
     * Deterministic in (directory, page contents), as §10 replay requires.
     */
    void SplitPage(PageId pid)
    {
        PageView low = MutableView(pid);
        uint8_t ld = low.LocalDepth();
        assert(ld < kMaxDepth);
        if (ld == meta_.global_depth_)
        {
            DoubleDirectory();
        }
        PageId high_id = frames_.AllocatePageId();
        uint8_t *high_buf = frames_.CreateDirtyPage(high_id);
        PageView high(high_buf, frames_.PageSize(), 0);
        high.Init(0);
        low.SplitInto(high, ld);
        meta_.page_entry_counts_.try_emplace(high_id, 0);
        SetCount(pid, low.EntryCount());
        SetCount(high_id, high.EntryCount());
        // Both halves changed content; the new page was already stamped
        // dirty at creation.
        frames_.TouchPage(pid);
        // Repoint the upper half of pid's directory-sharing group at the new
        // page. The group is the aligned block of 2^(gd-new_ld) * 2 entries
        // that shared pid; its upper half now routes to high_id.
        uint8_t new_ld = low.LocalDepth();  // ld + 1
        size_t group = size_t{1} << (meta_.global_depth_ - new_ld + 1);
        size_t start = 0;
        while (meta_.dir_[start] != pid)
        {
            ++start;
        }
        assert(start % group == 0);
        for (size_t i = start + group / 2; i < start + group; ++i)
        {
            assert(meta_.dir_[i] == pid);
            meta_.dir_[i] = high_id;
        }
    }

    void DoubleDirectory()
    {
        assert(meta_.global_depth_ < kMaxDepth);
        std::vector<PageId> next(meta_.dir_.size() * 2);
        for (size_t i = 0; i < next.size(); ++i)
        {
            next[i] = meta_.dir_[i >> 1];
        }
        meta_.dir_ = std::move(next);
        ++meta_.global_depth_;
    }

    // The type's "header" that interprets pages (§4): routing directory,
    // per-page entry counts, large-value runs. The page manager (frames_) is
    // inherited from PagedTxObject; no fetch collection lives here at all —
    // outstanding fetches and wake records are ENTRY-scoped (FetchHub, §7),
    // while frame state and pin contexts are PAYLOAD-scoped inside frames_
    // and die with this block at a §7 swap.
    PagedHashMetadata meta_;
    FieldHashFn hash_fn_;
};

/**
 * @brief The TTL twin (§4). Per the class-twin convention it reports the BASE
 * ObjectType() so type checks and command code never see TTL-ness; only
 * serialization and HasTTL() reveal it ([03-data-model.md](03-data-model.md)
 * §3).
 *
 * Note the TTL lives here, in the object, and is serialized into the metadata
 * row — the store-row TTL *attribute* is a separate enforcement-only
 * annotation the flush path derives (deadline + slack on the metadata row,
 * none on page rows; §9).
 */
class RedisPagedHashTTLObject : public RedisPagedHashObject
{
public:
    RedisPagedHashTTLObject() : RedisPagedHashObject(), ttl_(UINT64_MAX)
    {
    }

    RedisPagedHashTTLObject(const RedisPagedHashTTLObject &other)
        : RedisPagedHashObject(other), ttl_(other.ttl_)
    {
    }

    RedisPagedHashTTLObject(RedisPagedHashObject &&other, uint64_t ttl)
        : RedisPagedHashObject(std::move(other)), ttl_(ttl)
    {
    }

    txservice::TxRecord::Uptr Clone() const override
    {
        return std::make_unique<RedisPagedHashTTLObject>(*this);
    }

    void SetTTL(uint64_t ttl) override
    {
        ttl_ = ttl;
    }

    uint64_t GetTTL() const override
    {
        return ttl_;
    }

    bool HasTTL() const override
    {
        return true;
    }

    RedisObjectType ObjectType() const override
    {
        // The base type on purpose, and the same base the non-TTL paged
        // object reports: neither TTL-ness nor paged-ness is visible to type
        // checks or command code.
        return RedisObjectType::Hash;
    }

    size_t SerializedLength() const override
    {
        return sizeof(uint64_t) + RedisPagedHashObject::SerializedLength();
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override
    {
        std::string scratch;
        Serialize(scratch);
        buf.resize(offset + scratch.size());
        std::copy(scratch.begin(), scratch.end(), buf.begin() + offset);
        offset += scratch.size();
    }

    void Serialize(std::string &str) const override
    {
        str.append(1, static_cast<int8_t>(RedisObjectType::TTLPagedHash));
        const char *ttl_ptr = reinterpret_cast<const char *>(&ttl_);
        str.append(ttl_ptr, sizeof(uint64_t));
        SerializeMeta(str);
    }

    void Deserialize(const char *buf, size_t &offset) override
    {
        bool ok = DeserializeBounded(buf, SIZE_MAX, offset);
        assert(ok &&
               "malformed paged-hash TTL metadata in a self-written "
               "image");
        (void) ok;
    }


    /**
     * @brief The TTL twin's row parser (ttl field, then the shared
     * sections). Deliberately SHADOWS the base method: each class parses
     * its own row shape, and callers hold the concrete type. A TTL row fed
     * to the base parser (or vice versa) fails the tag check — a safe
     * refusal, never a misparse.
     */
    bool DeserializeBounded(const char *buf, size_t len, size_t &offset)
    {
        if (offset >= len)
        {
            return false;
        }
        RedisObjectType obj_type =
            static_cast<RedisObjectType>(*(buf + offset));
        if (obj_type != RedisObjectType::TTLPagedHash)
        {
            assert(len != SIZE_MAX && "wrong tag in a self-written image");
            return false;
        }
        offset += 1;
        if (len != SIZE_MAX && offset + sizeof(uint64_t) > len)
        {
            return false;
        }
        std::memcpy(&ttl_, buf + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        return DeserializeSections(buf, len, offset);
    }

    /**
     * @brief Interface-completeness, NOT a live path today. SetEncodedBlob
     * is how the GENERIC-materialization store handlers (e.g. BigTable's
     * payload/unpack_info scheme) install fetched rows; the DSS backends
     * EloqKV runs on go through DeserializeObject instead, and §11's gating
     * keeps paged rows off non-DSS backends entirely. Implemented anyway
     * because the base default is assert(false) — a silent no-op in Release
     * — and a correct funnel into the one parser is strictly safer than
     * that trap if a future handler ever reaches here.
     */
    void SetEncodedBlob(const unsigned char *blob_ptr,
                        size_t blob_size) override
    {
        size_t offset = 0;
        bool ok = DeserializeBounded(
            reinterpret_cast<const char *>(blob_ptr), blob_size, offset);
        assert(ok && "malformed paged-hash TTL blob");
        (void) ok;
    }

    size_t MemUsage() const override
    {
        return sizeof(RedisPagedHashTTLObject) + MetaSerializedSize() +
               ResidentBytes();
    }

    /**
     * @brief Swaps back to the non-TTL class (PERSIST, or a command that
     * clears the deadline), mirroring RedisHashTTLObject::RemoveTTL.
     */
    txservice::TxRecord::Uptr RemoveTTL() override
    {
        return std::make_unique<RedisPagedHashObject>(std::move(*this));
    }

protected:
    /**
     * @brief The §9 interim TTL scheme: the metadata row's store-TTL
     * attribute is the logical deadline plus slack, so the row outlives an
     * un-checkpointed TTL extension across a crash. Page rows get no
     * attribute at all — the flush path hardcodes 0 for them, since their
     * attributes cannot track a TTL reset.
     */
    uint64_t MetadataRowTtl() const override
    {
        if (ttl_ == UINT64_MAX)
        {
            return 0;
        }
        return ttl_ +
               static_cast<uint64_t>(GetPagedTtlSlackSeconds()) * 1000ULL;
    }

private:

    uint64_t ttl_{UINT64_MAX};
};
/**
 * @brief Defined here rather than in the class body because the TTL twin is
 * declared after the base (the monolithic hash solves the same ordering by
 * putting AddTTL in its .cpp).
 */
inline txservice::TxRecord::Uptr RedisPagedHashObject::AddTTL(uint64_t ttl)
{
    return std::make_unique<RedisPagedHashTTLObject>(std::move(*this), ttl);
}
/**
 * @brief THE monolithic-to-paged conversion policy (docs/08 §11), applied
 * after any mutation that can grow a hash and on every import path.
 *
 * Centralized on purpose. The invariant is "a hash converts when its
 * post-image crosses the threshold", which is a property of the OBJECT, not
 * of the command that touched it — so it cannot live inside one command's
 * CommitOn. Tied to `HSET` alone it produced hashes of identical size in
 * different representations depending on how they were grown, and left
 * `HSETNX`/`HINCRBY`/`HINCRBYFLOAT`/`RESTORE`-grown objects permanently
 * monolithic, which is exactly the scaling problem paging exists to remove.
 *
 * Three conditions, all required: the threshold is enabled and crossed
 * (`ShouldConvert`); every record fits a page (`AllRecordsFit` — v1 has no
 * out-of-line large values, §14); and the source is a monolithic hash. The
 * TTL is carried across: converting through `FromFields` alone yields a
 * non-TTL twin, so a hash with a deadline would silently become permanent.
 *
 * Deterministic per command stream — it reads only the post-image and the
 * page size — so primary, standby and replay reach the same verdict (§11).
 *
 * @param obj_ptr The just-mutated payload. Ownership is unaffected when no
 * conversion happens.
 * @return `obj_ptr` unchanged, or a newly allocated paged twin the caller
 * installs in its place (the object-swap channel AddTTL/RemoveTTL use).
 */
inline txservice::TxObject *MaybeConvertHashToPaged(
    txservice::TxObject *obj_ptr)
{
    if (obj_ptr == nullptr || obj_ptr->AsPaged() != nullptr)
    {
        return obj_ptr;  // gone, or already paged
    }
    // The import path hands us objects of EVERY type, so the hash check is
    // load-bearing, not defensive: casting a list to RedisHashObject would
    // read another type's members. Both hash twins report the base type.
    auto &eloq_obj = static_cast<RedisEloqObject &>(*obj_ptr);
    if (eloq_obj.ObjectType() != RedisObjectType::Hash)
    {
        return obj_ptr;
    }
    auto &hash_obj = static_cast<RedisHashObject &>(*obj_ptr);
    if (!RedisPagedHashObject::ShouldConvert(hash_obj.SerializedLength()))
    {
        return obj_ptr;
    }
    auto fields = hash_obj.FieldsView();
    if (!RedisPagedHashObject::AllRecordsFit(fields, GetPagedPageSize()))
    {
        return obj_ptr;  // ineligible: stays monolithic (§14)
    }
    auto paged = RedisPagedHashObject::FromFields(fields, GetPagedPageSize());
    if (hash_obj.HasTTL())
    {
        return static_cast<txservice::TxObject *>(
            paged->AddTTL(hash_obj.GetTTL()).release());
    }
    return static_cast<txservice::TxObject *>(paged.release());
}

/**
 * @brief Ownership-taking form, for a call site that OWNS its input.
 *
 * The two kinds of caller differ in who frees the source. A mutator's
 * `CommitOn` is handed the engine-owned payload, and the engine destroys what
 * it replaces when a different pointer comes back — so those sites pass a raw
 * pointer. The import path (`RestoreCommand::CommitOn`) instead BUILDS its
 * object locally and hands ownership out through its return value; if the
 * policy replaces that object, nothing else has ever held it, so releasing it
 * to the raw-pointer form orphans it. Passing the `unique_ptr` here keeps the
 * source owned until the verdict is known.
 *
 * @param owned The freshly built object. Destroyed here iff it is converted.
 * @return the object the caller must return: `owned` released unchanged, or
 * the paged twin.
 */
inline txservice::TxObject *MaybeConvertHashToPaged(
    std::unique_ptr<txservice::TxRecord> owned)
{
    auto *raw = static_cast<txservice::TxObject *>(owned.get());
    auto *converted = MaybeConvertHashToPaged(raw);
    if (converted == raw)
    {
        owned.release();  // unchanged: ownership passes to the caller
    }
    return converted;  // otherwise `owned` destroys the source right here
}
}  // namespace EloqKV
