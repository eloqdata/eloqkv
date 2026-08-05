/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under either of the following two licenses:
 *    1. GNU Affero General Public License, version 3, as published by the Free
 *    Software Foundation; or
 *    2. GNU General Public License as published by the Free Software
 *    Foundation; version 2 of the License.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License or GNU General Public License for more
 *    details.
 */

// Production-engine coverage for docs/08-paged-objects-test-plan.md §4.5.
// Unlike the FetchHub-only tests, this fixture keeps a real PageFetch owned by
// a real CcEntry and completes it through PageFetch::Execute ->
// ObjectCcMap::BackFillPage. A narrow friend-only Sharder accessor seeds the
// local term without opening RPC listeners.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/cc_shard.h"
#include "cc/object_cc_map.h"
#include "cc/page_fetch.h"
#include "eloqkv_catalog_factory.h"
#include "eloqkv_key.h"
#include "redis_paged_hash_object.h"

namespace txservice
{
class SharderTestAccess
{
public:
    static void BindLocalTerm(Sharder &sharder,
                              LocalCcShards *local_shards,
                              NodeGroupId ng_id,
                              int64_t term)
    {
        sharder.local_shards_ = local_shards;
        sharder.native_ng_ = ng_id;
        sharder.cc_nodes_init_.store(true, std::memory_order_release);
        sharder.leader_term_cache_[ng_id].store(term,
                                                std::memory_order_release);
        sharder.candidate_leader_term_cache_[ng_id].store(
            -1, std::memory_order_release);
        sharder.standby_node_term_cache_.store(-1, std::memory_order_release);
        sharder.candidate_standby_node_term_cache_.store(
            -1, std::memory_order_release);
    }

    static void SetLeaderTerm(Sharder &sharder, NodeGroupId ng_id, int64_t term)
    {
        sharder.leader_term_cache_[ng_id].store(term,
                                                std::memory_order_release);
    }
};
}  // namespace txservice

namespace
{
using namespace EloqKV;
using namespace txservice;

#undef CHECK
#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            std::fprintf(stderr,                                               \
                         "CHECK failed at %s:%d: %s\n",                        \
                         __FILE__,                                             \
                         __LINE__,                                             \
                         #cond);                                               \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

constexpr uint32_t kPageSize = 512;
constexpr int64_t kTerm = 7;
RedisCatalogFactory catalog_factory;

using RedisCcEntry = CcEntry<EloqKey, RedisEloqObject, false, false>;

struct StubCcRequest : public CcRequestBase
{
    explicit StubCcRequest(TxNumber txn)
    {
        tx_number_ = txn;
    }

    bool Execute(CcShard &ccs) override
    {
        (void) ccs;
        CHECK(false && "stub request is never executed");
        return false;
    }
};

class RedisCcMap : public ObjectCcMap<EloqKey, RedisEloqObject>
{
public:
    using ObjectCcMap<EloqKey, RedisEloqObject>::ObjectCcMap;
    using TemplateCcMap<EloqKey, RedisEloqObject, false, false>::
        OnCommittedUpdate;
    using TemplateCcMap<EloqKey, RedisEloqObject, false, false>::OnFlushed;
};

struct EngineFixture
{
    std::unordered_map<uint32_t, std::vector<NodeConfig>> ng_configs{
        {0, {NodeConfig(0, "127.0.0.1", 8600)}}};
    std::map<std::string, uint32_t> tx_config{
        {"node_memory_limit_mb", 1000},
        {"enable_key_cache", 0},
        {"reltime_sampling", 0},
        {"range_split_worker_num", 1},
        {"range_slice_memory_limit_percent", 20},
        {"core_num", 1},
        {"realtime_sampling", 0},
        {"checkpointer_interval", 10},
        {"checkpointer_delay_seconds", 0},
        {"checkpointer_min_ckpt_request_interval", 5},
        {"enable_shard_heap_defragment", 0},
        {"node_log_limit_mb", 1000},
        {"collect_active_tx_ts_interval_seconds", 2},
        {"rep_group_cnt", 1},
    };
    CatalogFactory *catalog_factories[5] = {
        &catalog_factory,
        &catalog_factory,
        &catalog_factory,
        &catalog_factory,
        &catalog_factory,
    };
    LocalCcShards local_shards;
    CcShard shard;
    TableName table_name{std::string("paged_backfill"),
                         TableType::Primary,
                         TableEngine::EloqSql};
    RedisTableSchema table_schema{table_name, "paged_backfill", 1};
    RedisCcMap cc_map{&shard, 0, table_name, 1, &table_schema, true};
    std::string raft_path;

    EngineFixture()
        : local_shards(0,
                       0,
                       tx_config,
                       catalog_factories,
                       nullptr,
                       &ng_configs,
                       2,
                       nullptr,
                       nullptr,
                       true),
          shard(0,
                1,
                1000,
                false,
                0,
                local_shards,
                catalog_factories,
                nullptr,
                &ng_configs,
                2)
    {
        // CcShard production threads bind before Init; FastMetaDataMutex uses
        // the binding as its per-core index.
        local_shards.BindThreadToFastMetaDataShard(0);
        shard.Init();
        SharderTestAccess::BindLocalTerm(
            Sharder::Instance(), &local_shards, 0, kTerm);
    }

    ~EngineFixture()
    {
        local_shards.Terminate();
    }
};

struct ColdObject
{
    std::unique_ptr<RedisPagedHashObject> object_;
    std::string metadata_;
    std::vector<std::pair<PageId, std::string>> pages_;
};

ColdObject BuildColdObject()
{
    RedisPagedHashObject resident(kPageSize);
    for (uint32_t i = 0; i < 80; ++i)
    {
        resident.Put("field-" + std::to_string(i),
                     std::string(24, static_cast<char>('a' + i % 26)));
    }

    ColdObject cold;
    resident.SerializeAll(cold.metadata_, cold.pages_);
    CHECK(cold.pages_.size() > 1);

    cold.object_ = std::make_unique<RedisPagedHashObject>();
    size_t offset = 0;
    cold.object_->Deserialize(cold.metadata_.data(), offset);
    CHECK(offset == cold.metadata_.size());
    CHECK(cold.object_->ResidentPageCount() == 0);
    return cold;
}

enum class CompletionShape
{
    Valid,
    TransportError,
    Missing,
    WrongSize,
    Corrupt,
    DeletedDuringFlight,
    TermChanged,
};

void RunCompletion(EngineFixture &fixture,
                   uint32_t sequence,
                   CompletionShape shape)
{
    ColdObject cold = BuildColdObject();
    const PageId page_id = cold.pages_.front().first;
    const std::string page_image = cold.pages_.front().second;
    const TxNumber txn = 1000 + sequence;
    EloqKey key = EloqKey::Raw("backfill:" + std::to_string(sequence));

    bool emplaced = false;
    auto it = fixture.cc_map.FindEmplace(key, &emplaced, false, false);
    CHECK(emplaced);
    auto *cce = static_cast<RedisCcEntry *>(it->second);
    auto *ccp = it.GetPage();
    bool was_dirty = cce->IsDirty();
    cce->SetCommitTsPayloadStatus(100,
                                  shape == CompletionShape::DeletedDuringFlight
                                      ? RecordStatus::Deleted
                                      : RecordStatus::Normal);
    cce->SetCkptTs(100);
    fixture.cc_map.OnFlushed(cce, was_dirty);
    fixture.cc_map.OnCommittedUpdate(cce, was_dirty);
    ccp->last_dirty_commit_ts_ =
        std::max(cce->CommitTs(), ccp->last_dirty_commit_ts_);
    cce->payload_.cur_payload_ = std::move(cold.object_);
    auto *paged = cce->payload_.cur_payload_->AsPaged();
    CHECK(paged != nullptr);
    CHECK(paged->IsPageLive(page_id));
    CHECK(!paged->IsPageResident(page_id));
    paged->EnsureTxFaultContext(txn);
    CHECK(paged->ReserveFaultBuffers({page_id}));
    CHECK(paged->ReservedCount() == 1);

    // Execute releases the issuance pin and attempts to recycle the lock. The
    // second pin keeps the hub alive long enough to inspect the result.
    cce->GetOrCreateKeyLock(&fixture.shard, &fixture.cc_map, ccp);
    KeyGapLockAndExtraData *lke = cce->GetKeyGapLockAndExtraData();
    lke->AddPin();
    lke->AddPin();
    FetchHub &hub = lke->GetOrCreateFetchHub();
    hub.NoteAwaited(txn);

    std::string encoded_page_key;
    EncodePageKey(
        encoded_page_key, key.KVSerialize(), PageRowKind::HashPage, page_id);
    PageKey page_key(std::move(encoded_page_key), key.Hash());
    auto fetch = std::make_unique<PageFetch>(&fixture.table_name,
                                             &fixture.table_schema,
                                             page_key.CloneTxKey(),
                                             page_id,
                                             cce,
                                             fixture.shard,
                                             0,
                                             kTerm,
                                             0);
    fetch->waiter_txns_.push_back(txn);
    fetch->rec_ts_ = 77;
    fetch->rec_status_ = RecordStatus::Normal;
    fetch->rec_str_ = page_image;
    if (shape == CompletionShape::Missing)
    {
        fetch->rec_status_ = RecordStatus::Deleted;
        fetch->rec_str_.clear();
    }
    else if (shape == CompletionShape::WrongSize)
    {
        fetch->rec_str_.pop_back();
    }
    else if (shape == CompletionShape::Corrupt)
    {
        std::fill(fetch->rec_str_.begin(), fetch->rec_str_.end(), '\xA5');
    }
    else if (shape == CompletionShape::TransportError)
    {
        fetch->error_code_ = static_cast<int>(CcErrorCode::DATA_STORE_ERR);
    }

    if (shape == CompletionShape::TermChanged)
    {
        SharderTestAccess::SetLeaderTerm(Sharder::Instance(), 0, kTerm + 1);
    }

    PageFetch *completion = fetch.get();
    CHECK(hub.live_.emplace(page_id, std::move(fetch)).second);
    CHECK(!completion->Execute(fixture.shard));
    SharderTestAccess::SetLeaderTerm(Sharder::Instance(), 0, kTerm);
    CHECK(hub.live_.empty());

    const bool should_install = shape == CompletionShape::Valid;
    const bool should_error = shape == CompletionShape::TransportError ||
                              shape == CompletionShape::Missing ||
                              shape == CompletionShape::WrongSize ||
                              shape == CompletionShape::Corrupt ||
                              shape == CompletionShape::TermChanged;
    CHECK(paged->IsPageResident(page_id) == should_install);
    CHECK(paged->ReservedCount() == 0);
    CHECK(hub.ConsumeError(txn) == should_error);

    // Successful fetches pin until the transaction re-runs/finishes. Errors
    // and benign deletion do not pin, but ReleaseTxPins is idempotent.
    paged->ReleaseTxPins(txn);
    lke->ReleasePin();
    CHECK(cce->RecycleKeyLock(fixture.shard));
    CHECK(cce->GetKeyGapLockAndExtraData() == nullptr);
}

void RunCommittedDirtyCompletion(EngineFixture &fixture,
                                 uint32_t sequence,
                                 bool reserve_on_dirty)
{
    ColdObject cold = BuildColdObject();
    const PageId page_id = cold.pages_.front().first;
    const std::string page_image = cold.pages_.front().second;
    const TxNumber committed_txn = 3000 + 2 * sequence;
    const TxNumber dirty_txn = committed_txn + 1;
    EloqKey key = EloqKey::Raw("backfill:dual:" + std::to_string(sequence));

    auto dirty_object = std::make_unique<RedisPagedHashObject>(*cold.object_);
    bool emplaced = false;
    auto it = fixture.cc_map.FindEmplace(key, &emplaced, false, false);
    CHECK(emplaced);
    auto *cce = static_cast<RedisCcEntry *>(it->second);
    auto *ccp = it.GetPage();
    bool was_dirty = cce->IsDirty();
    cce->SetCommitTsPayloadStatus(100, RecordStatus::Normal);
    cce->SetCkptTs(100);
    fixture.cc_map.OnFlushed(cce, was_dirty);
    fixture.cc_map.OnCommittedUpdate(cce, was_dirty);
    ccp->last_dirty_commit_ts_ =
        std::max(cce->CommitTs(), ccp->last_dirty_commit_ts_);
    cce->payload_.cur_payload_ = std::move(cold.object_);

    cce->GetOrCreateKeyLock(&fixture.shard, &fixture.cc_map, ccp);
    KeyGapLockAndExtraData *lke = cce->GetKeyGapLockAndExtraData();
    StubCcRequest writer(dirty_txn);
    CHECK(lke->KeyLock()->AcquireWriteLock(&writer, CcProtocol::Locking));
    lke->SetDirtyPayload(std::move(dirty_object));
    lke->SetDirtyPayloadStatus(RecordStatus::Normal);
    auto *committed =
        static_cast<RedisPagedHashObject *>(cce->payload_.cur_payload_.get());
    auto *dirty = static_cast<RedisPagedHashObject *>(lke->PeekDirtyPayload());
    CHECK(committed != nullptr && dirty != nullptr);
    CHECK(!committed->IsPageResident(page_id));
    CHECK(!dirty->IsPageResident(page_id));

    committed->EnsureTxFaultContext(committed_txn);
    dirty->EnsureTxFaultContext(dirty_txn);
    PagedTxObject *reservation_owner = reserve_on_dirty ? dirty : committed;
    CHECK(reservation_owner->ReserveFaultBuffers({page_id}));
    CHECK(reservation_owner->ReservedCount() == 1);

    // One issuance pin is consumed by Execute; one inspection pin keeps the
    // entry's hub and dirty payload alive for the post-completion assertions.
    lke->AddPin();
    lke->AddPin();
    FetchHub &hub = lke->GetOrCreateFetchHub();
    hub.NoteAwaited(committed_txn);
    hub.NoteAwaited(dirty_txn);

    std::string encoded_page_key;
    EncodePageKey(
        encoded_page_key, key.KVSerialize(), PageRowKind::HashPage, page_id);
    PageKey page_key(std::move(encoded_page_key), key.Hash());
    auto fetch = std::make_unique<PageFetch>(&fixture.table_name,
                                             &fixture.table_schema,
                                             page_key.CloneTxKey(),
                                             page_id,
                                             cce,
                                             fixture.shard,
                                             0,
                                             kTerm,
                                             0);
    fetch->waiter_txns_ = {committed_txn, dirty_txn};
    fetch->rec_ts_ = 77;
    fetch->rec_status_ = RecordStatus::Normal;
    fetch->rec_str_ = page_image;
    PageFetch *completion = fetch.get();
    CHECK(hub.live_.emplace(page_id, std::move(fetch)).second);
    CHECK(!completion->Execute(fixture.shard));

    CHECK(hub.live_.empty());
    CHECK(!hub.ConsumeError(committed_txn));
    CHECK(!hub.ConsumeError(dirty_txn));
    CHECK(committed->IsPageResident(page_id));
    CHECK(dirty->IsPageResident(page_id));
    CHECK(committed->ReservedCount() == 0);
    CHECK(dirty->ReservedCount() == 0);

    // Both payloads receive the one canonical fetched buffer. A write to the
    // dirty object must COW it rather than rewrite committed state.
    std::string field;
    for (uint32_t i = 0; i < 80; ++i)
    {
        std::string candidate = "field-" + std::to_string(i);
        if (committed->RouteField(candidate, nullptr) == page_id)
        {
            field = std::move(candidate);
            break;
        }
    }
    CHECK(!field.empty());
    std::optional<std::string_view> original = committed->Get(field);
    CHECK(original.has_value());
    std::string original_copy(*original);
    CHECK(dirty->Put(field, "dirty-only") == false);
    CHECK(committed->Get(field).value() == original_copy);
    CHECK(dirty->Get(field).value() == "dirty-only");

    committed->ReleaseTxPins(committed_txn);
    dirty->ReleaseTxPins(dirty_txn);
    lke->SetDirtyPayload(nullptr);
    lke->SetDirtyPayloadStatus(RecordStatus::NonExistent);
    CHECK(lke->KeyLock()->ReleaseWriteLock(dirty_txn, nullptr));
    lke->ReleasePin();
    CHECK(cce->RecycleKeyLock(fixture.shard));
    CHECK(cce->GetKeyGapLockAndExtraData() == nullptr);
}

void RunOrphanExecute(EngineFixture &fixture)
{
    ColdObject cold = BuildColdObject();
    const PageId page_id = cold.pages_.front().first;
    constexpr TxNumber txn = 2000;
    EloqKey key = EloqKey::Raw("backfill:orphan");

    bool emplaced = false;
    auto it = fixture.cc_map.FindEmplace(key, &emplaced, false, false);
    CHECK(emplaced);
    auto *cce = static_cast<RedisCcEntry *>(it->second);
    auto *ccp = it.GetPage();
    bool was_dirty = cce->IsDirty();
    cce->SetCommitTsPayloadStatus(100, RecordStatus::Normal);
    cce->SetCkptTs(100);
    fixture.cc_map.OnFlushed(cce, was_dirty);
    fixture.cc_map.OnCommittedUpdate(cce, was_dirty);
    ccp->last_dirty_commit_ts_ =
        std::max(cce->CommitTs(), ccp->last_dirty_commit_ts_);
    cce->payload_.cur_payload_ = std::move(cold.object_);

    cce->GetOrCreateKeyLock(&fixture.shard, &fixture.cc_map, ccp);
    KeyGapLockAndExtraData *lke = cce->GetKeyGapLockAndExtraData();
    // One issuance pin is released by Execute; one inspection pin prevents
    // RecycleKeyLock from destroying the hub before assertions run.
    lke->AddPin();
    lke->AddPin();
    FetchHub &hub = lke->GetOrCreateFetchHub();
    hub.NoteAwaited(txn);

    std::string encoded_page_key;
    EncodePageKey(
        encoded_page_key, key.KVSerialize(), PageRowKind::HashPage, page_id);
    PageKey page_key(std::move(encoded_page_key), key.Hash());
    auto fetch = std::make_unique<PageFetch>(&fixture.table_name,
                                             &fixture.table_schema,
                                             page_key.CloneTxKey(),
                                             page_id,
                                             cce,
                                             fixture.shard,
                                             0,
                                             kTerm,
                                             0);
    fetch->waiter_txns_.push_back(txn);
    fetch->orphaned_ = true;
    PageFetch *completion = fetch.get();
    hub.orphans_.push_back(std::move(fetch));

    CHECK(!completion->Execute(fixture.shard));
    CHECK(hub.Empty());
    CHECK(!hub.ConsumeError(txn));
    lke->ReleasePin();
    CHECK(cce->RecycleKeyLock(fixture.shard));
    CHECK(cce->GetKeyGapLockAndExtraData() == nullptr);
}

void RunMetadataBackfill(EngineFixture &fixture)
{
    ColdObject cold = BuildColdObject();
    auto emplace_entry = [&](std::string_view suffix)
    {
        EloqKey key =
            EloqKey::Raw(std::string("backfill:metadata:").append(suffix));
        bool emplaced = false;
        auto it = fixture.cc_map.FindEmplace(key, &emplaced, false, false);
        CHECK(emplaced);
        auto *cce = static_cast<RedisCcEntry *>(it->second);
        cce->GetOrCreateKeyLock(&fixture.shard, &fixture.cc_map, it.GetPage());
        cce->GetKeyGapLockAndExtraData()->AddPin();
        return cce;
    };

    RedisCcEntry *valid = emplace_entry("valid");
    bool corrupt = false;
    CHECK(fixture.cc_map.BackFill(
        valid, 50, RecordStatus::Normal, cold.metadata_, &corrupt));
    CHECK(!corrupt);
    CHECK(valid->CommitTs() == 50);
    CHECK(valid->PayloadStatus() == RecordStatus::Normal);
    CHECK(valid->payload_.cur_payload_ != nullptr);
    CHECK(valid->payload_.cur_payload_->AsPaged() != nullptr);
    CHECK(!valid->payload_.cur_payload_->AsPaged()->IsFullyResident());
    CHECK(valid->GetKeyGapLockAndExtraData() == nullptr);

    RedisCcEntry *empty = emplace_entry("empty");
    const uint64_t empty_before_ts = empty->CommitTs();
    const RecordStatus empty_before_status = empty->PayloadStatus();
    corrupt = false;
    CHECK(fixture.cc_map.BackFill(
        empty, 51, RecordStatus::Normal, std::string(), &corrupt));
    CHECK(corrupt);
    CHECK(empty->CommitTs() == empty_before_ts);
    CHECK(empty->PayloadStatus() == empty_before_status);
    CHECK(empty->payload_.cur_payload_ == nullptr);
    CHECK(empty->GetKeyGapLockAndExtraData() == nullptr);

    RedisCcEntry *truncated = emplace_entry("truncated");
    const uint64_t truncated_before_ts = truncated->CommitTs();
    const RecordStatus truncated_before_status = truncated->PayloadStatus();
    corrupt = false;
    std::string short_metadata = cold.metadata_.substr(
        0, std::max<size_t>(1, cold.metadata_.size() / 2));
    CHECK(fixture.cc_map.BackFill(
        truncated, 52, RecordStatus::Normal, short_metadata, &corrupt));
    CHECK(corrupt);
    CHECK(truncated->CommitTs() == truncated_before_ts);
    CHECK(truncated->PayloadStatus() == truncated_before_status);
    CHECK(truncated->payload_.cur_payload_ == nullptr);
    CHECK(truncated->GetKeyGapLockAndExtraData() == nullptr);
}
}  // namespace

int main()
{
    EngineFixture fixture;
    std::fprintf(stderr, "backfill case: valid\n");
    RunCompletion(fixture, 1, CompletionShape::Valid);
    std::fprintf(stderr, "backfill case: transport-error\n");
    RunCompletion(fixture, 2, CompletionShape::TransportError);
    std::fprintf(stderr, "backfill case: missing\n");
    RunCompletion(fixture, 3, CompletionShape::Missing);
    std::fprintf(stderr, "backfill case: wrong-size\n");
    RunCompletion(fixture, 4, CompletionShape::WrongSize);
    std::fprintf(stderr, "backfill case: corrupt\n");
    RunCompletion(fixture, 5, CompletionShape::Corrupt);
    std::fprintf(stderr, "backfill case: deleted\n");
    RunCompletion(fixture, 6, CompletionShape::DeletedDuringFlight);
    std::fprintf(stderr, "backfill case: term-changed\n");
    RunCompletion(fixture, 7, CompletionShape::TermChanged);
    std::fprintf(stderr, "backfill case: dual committed reservation\n");
    RunCommittedDirtyCompletion(fixture, 8, false);
    std::fprintf(stderr, "backfill case: dual dirty reservation\n");
    RunCommittedDirtyCompletion(fixture, 9, true);
    std::fprintf(stderr, "backfill case: orphan\n");
    RunOrphanExecute(fixture);
    std::fprintf(stderr, "backfill case: metadata\n");
    RunMetadataBackfill(fixture);
    std::printf("paged engine backfill: ok\n");
    return 0;
}
