#include "namespace/storage.h"

#include <glog/logging.h>

#include "b255.h"
#include "eloqkv_key.h"
#include "namespace/gc.h"
#include "namespace/prefix.h"
#include "redis_command.h"
#include "redis_service.h"
#include "tx_execution.h"
#include "tx_request.h"
#include "tx_util.h"

namespace EloqKV
{

NamespaceStorage::NamespaceStorage(RedisServiceImpl *server)
    : server_(server), gc_(std::make_unique<NamespaceGc>(server))
{
}

NamespaceStorage::~NamespaceStorage() = default;

NamespaceToken NamespaceStorage::GetToken(std::string_view ns)
{
    TransactionExecution *txm =
        server_->NewTxm(IsolationLevel::RepeatableRead, CcProtocol::Locking);
    if (txm == nullptr)
        return NamespaceToken();

    EloqKey db_key =
        EloqKey::Raw(std::string(kNamespacePrefix) + std::string(ns));

    GetCommand cmd;
    ObjectCommandTxRequest tx_req(server_->NamespaceTableName(),
                                  &db_key,
                                  &cmd,
                                  /*auto_commit=*/false,
                                  /*always_redirect=*/true,
                                  txm);
    bool success = server_->ExecuteNamespaceTxRequest(txm, &tx_req);

    if (!success)
    {
        txservice::AbortTx(txm);
        return NamespaceToken();
    }
    auto [commit_success, _] = txservice::CommitTx(txm);
    if (commit_success && cmd.result_.err_code_ == RD_OK)
    {
        return NamespaceToken(cmd.result_.str_);
    }
    return NamespaceToken();
}

std::string NamespaceStorage::ByToken(const NamespaceToken &token,
                                      std::string &ns_id,
                                      uint64_t &epoch)
{
    ns_id = "";
    epoch = 1;

    if (!token.valid)
    {
        return "";
    }

    if (token.ToString() == requirepass && !requirepass.empty())
    {
        return "";
    }

    TransactionExecution *txm =
        server_->NewTxm(IsolationLevel::RepeatableRead, CcProtocol::Locking);
    if (txm == nullptr)
        return "";

    std::unique_ptr<EloqKey> i_key;
    std::unique_ptr<GetCommand> cmd_i;
    std::unique_ptr<ObjectCommandTxRequest> tx_req_i;

    std::unique_ptr<EloqKey> e_key;
    std::unique_ptr<GetCommand> cmd_e;
    std::unique_ptr<ObjectCommandTxRequest> tx_req_e;

    EloqKey db_key =
        EloqKey::Raw(std::string(kTokenPrefix) + std::string(token.RawView()));

    GetCommand cmd;
    ObjectCommandTxRequest tx_req(server_->NamespaceTableName(),
                                  &db_key,
                                  &cmd,
                                  /*auto_commit=*/false,
                                  /*always_redirect=*/true,
                                  txm);
    bool success = server_->ExecuteNamespaceTxRequest(txm, &tx_req);

    std::string ns_name;
    if (success && cmd.result_.err_code_ == RD_OK)
    {
        std::string encoded_id = cmd.result_.str_;
        ns_id = encoded_id;

        i_key = std::make_unique<EloqKey>(
            EloqKey::Raw(std::string(kIdToNamePrefix) + encoded_id));

        cmd_i = std::make_unique<GetCommand>();
        tx_req_i = std::make_unique<ObjectCommandTxRequest>(
            server_->NamespaceTableName(),
            i_key.get(),
            cmd_i.get(),
            /*auto_commit=*/false,
            /*always_redirect=*/true,
            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, tx_req_i.get());
        if (success && cmd_i->result_.err_code_ == RD_OK)
        {
            ns_name = cmd_i->result_.str_;
        }

        e_key = std::make_unique<EloqKey>(
            EloqKey::Raw(std::string(kEpochPrefix) + encoded_id));
        cmd_e = std::make_unique<GetCommand>();
        tx_req_e = std::make_unique<ObjectCommandTxRequest>(
            server_->NamespaceTableName(),
            e_key.get(),
            cmd_e.get(),
            /*auto_commit=*/false,
            /*always_redirect=*/true,
            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, tx_req_e.get());
        if (success && cmd_e->result_.err_code_ == RD_OK)
        {
            try
            {
                epoch = std::stoull(cmd_e->result_.str_);
            }
            catch (...)
            {
                epoch = 1;
            }
        }
    }

    txservice::CommitTx(txm);
    return ns_name;
}

bool NamespaceStorage::Add(std::string_view ns, const NamespaceToken &token)
{
    if (!token.valid)
    {
        return false;
    }
    if (token.ToString() == requirepass && !requirepass.empty())
    {
        return false;
    }
    TransactionExecution *txm =
        server_->NewTxm(IsolationLevel::RepeatableRead, CcProtocol::Locking);
    if (txm == nullptr)
        return false;

    // Check if ns already exists
    EloqKey check_ns_key =
        EloqKey::Raw(std::string(kNamespacePrefix) + std::string(ns));
    EloqKey check_token_key =
        EloqKey::Raw(std::string(kTokenPrefix) + std::string(token.RawView()));

    GetCommand cmd_ns;
    ObjectCommandTxRequest tx_req_ns(server_->NamespaceTableName(),
                                     &check_ns_key,
                                     &cmd_ns,
                                     /*auto_commit=*/false,
                                     /*always_redirect=*/true,
                                     txm);
    bool success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_ns);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }
    if (cmd_ns.result_.err_code_ == RD_OK)
    {
        txservice::CommitTx(txm);
        return cmd_ns.result_.str_ ==
               token.RawView();  // Already exists with same token
    }

    // Check if token already exists
    GetCommand cmd_token;
    ObjectCommandTxRequest tx_req_token(server_->NamespaceTableName(),
                                        &check_token_key,
                                        &cmd_token,
                                        /*auto_commit=*/false,
                                        /*always_redirect=*/true,
                                        txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_token);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }
    if (cmd_token.result_.err_code_ == RD_OK)
    {
        txservice::AbortTx(txm);
        return false;
    }

    // Get next ID
    EloqKey next_id_key = EloqKey::Raw(kNextIdKey);

    GetCommand cmd_id;
    ObjectCommandTxRequest tx_req_id(server_->NamespaceTableName(),
                                     &next_id_key,
                                     &cmd_id,
                                     /*auto_commit=*/false,
                                     /*always_redirect=*/true,
                                     txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_id);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    uint64_t next_id = 1;
    if (cmd_id.result_.err_code_ == RD_OK)
    {
        try
        {
            next_id = std::stoull(cmd_id.result_.str_);
        }
        catch (...)
        {
            next_id = 1;
        }
    }

    uint64_t id = next_id;
    std::string new_next_id_val = std::to_string(next_id + 1);
    std::string encoded_id = b255e(id);

    SetCommand cmd_set_id(new_next_id_val);
    ObjectCommandTxRequest tx_req_set_id(server_->NamespaceTableName(),
                                         &next_id_key,
                                         &cmd_set_id,
                                         /*auto_commit=*/false,
                                         /*always_redirect=*/true,
                                         txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_set_id);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    // Write n:<ns> = token
    std::string token_raw(token.RawView());
    SetCommand cmd_set_ns(token_raw);
    ObjectCommandTxRequest tx_req_set_ns(server_->NamespaceTableName(),
                                         &check_ns_key,
                                         &cmd_set_ns,
                                         /*auto_commit=*/false,
                                         /*always_redirect=*/true,
                                         txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_set_ns);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    // Write t:<token> = encoded_id
    SetCommand cmd_set_token(encoded_id);
    ObjectCommandTxRequest tx_req_set_token(server_->NamespaceTableName(),
                                            &check_token_key,
                                            &cmd_set_token,
                                            /*auto_commit=*/false,
                                            /*always_redirect=*/true,
                                            txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_set_token);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    // Write i:<encoded_id> = ns
    EloqKey i_key = EloqKey::Raw(std::string(kIdToNamePrefix) + encoded_id);

    SetCommand cmd_set_i(ns);
    ObjectCommandTxRequest tx_req_set_i(server_->NamespaceTableName(),
                                        &i_key,
                                        &cmd_set_i,
                                        /*auto_commit=*/false,
                                        /*always_redirect=*/true,
                                        txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_set_i);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    // Write e:<encoded_id> = "1"
    EloqKey e_key = EloqKey::Raw(std::string(kEpochPrefix) + encoded_id);

    SetCommand cmd_set_e("1");
    ObjectCommandTxRequest tx_req_set_e(server_->NamespaceTableName(),
                                        &e_key,
                                        &cmd_set_e,
                                        /*auto_commit=*/false,
                                        /*always_redirect=*/true,
                                        txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_set_e);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    auto [commit_success, commit_err] = txservice::CommitTx(txm);
    return commit_success;
}

bool NamespaceStorage::Set(std::string_view ns, const NamespaceToken &token)
{
    if (!token.valid)
    {
        return false;
    }
    if (token.ToString() == requirepass && !requirepass.empty())
    {
        return false;
    }
    TransactionExecution *txm =
        server_->NewTxm(IsolationLevel::RepeatableRead, CcProtocol::Locking);
    if (txm == nullptr)
        return false;

    std::unique_ptr<EloqKey> owner_i_key;
    std::unique_ptr<GetCommand> cmd_owner;
    std::unique_ptr<ObjectCommandTxRequest> tx_req_owner;

    std::unique_ptr<EloqKey> old_token_key;
    std::unique_ptr<DelCommand> cmd_del_old;
    std::unique_ptr<ObjectCommandTxRequest> tx_req_del_old;

    std::unique_ptr<EloqKey> next_id_key;
    std::unique_ptr<SetCommand> cmd_set_id;
    std::unique_ptr<ObjectCommandTxRequest> tx_req_set_id;

    std::string token_raw;

    // Check if token already exists and is occupied by another namespace
    EloqKey check_token_key =
        EloqKey::Raw(std::string(kTokenPrefix) + std::string(token.RawView()));

    GetCommand cmd_token;
    ObjectCommandTxRequest tx_req_token(server_->NamespaceTableName(),
                                        &check_token_key,
                                        &cmd_token,
                                        /*auto_commit=*/false,
                                        /*always_redirect=*/true,
                                        txm);
    bool success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_token);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }
    if (cmd_token.result_.err_code_ == RD_OK)
    {
        std::string existing_encoded_id = cmd_token.result_.str_;
        owner_i_key = std::make_unique<EloqKey>(
            EloqKey::Raw(std::string(kIdToNamePrefix) + existing_encoded_id));

        cmd_owner = std::make_unique<GetCommand>();
        tx_req_owner = std::make_unique<ObjectCommandTxRequest>(
            server_->NamespaceTableName(),
            owner_i_key.get(),
            cmd_owner.get(),
            /*auto_commit=*/false,
            /*always_redirect=*/true,
            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, tx_req_owner.get());
        if (success && cmd_owner->result_.err_code_ == RD_OK)
        {
            if (cmd_owner->result_.str_ != ns)
            {
                txservice::AbortTx(txm);
                return false;
            }
        }
        else
        {
            txservice::AbortTx(txm);
            return false;
        }
    }

    // Get old token and ID
    EloqKey check_ns_key =
        EloqKey::Raw(std::string(kNamespacePrefix) + std::string(ns));

    GetCommand cmd_ns;
    ObjectCommandTxRequest tx_req_ns(server_->NamespaceTableName(),
                                     &check_ns_key,
                                     &cmd_ns,
                                     /*auto_commit=*/false,
                                     /*always_redirect=*/true,
                                     txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_ns);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    std::string encoded_id;
    if (cmd_ns.result_.err_code_ == RD_OK)
    {
        std::string old_token = cmd_ns.result_.str_;
        if (old_token == token.RawView())
        {
            txservice::CommitTx(txm);
            return true;
        }

        old_token_key = std::make_unique<EloqKey>(
            EloqKey::Raw(std::string(kTokenPrefix) + old_token));

        GetCommand cmd_old_t;
        ObjectCommandTxRequest tx_req_old_t(server_->NamespaceTableName(),
                                            old_token_key.get(),
                                            &cmd_old_t,
                                            /*auto_commit=*/false,
                                            /*always_redirect=*/true,
                                            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_old_t);
        if (success && cmd_old_t.result_.err_code_ == RD_OK)
        {
            encoded_id = cmd_old_t.result_.str_;
        }

        cmd_del_old = std::make_unique<DelCommand>();
        tx_req_del_old = std::make_unique<ObjectCommandTxRequest>(
            server_->NamespaceTableName(),
            old_token_key.get(),
            cmd_del_old.get(),
            /*auto_commit=*/false,
            /*always_redirect=*/true,
            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, tx_req_del_old.get());
        if (!success)
        {
            txservice::AbortTx(txm);
            return false;
        }
    }

    if (encoded_id.empty())
    {
        if (cmd_ns.result_.err_code_ == RD_OK)
        {
            txservice::AbortTx(txm);
            return false;
        }
        next_id_key = std::make_unique<EloqKey>(EloqKey::Raw(kNextIdKey));

        GetCommand cmd_id;
        ObjectCommandTxRequest tx_req_id(server_->NamespaceTableName(),
                                         next_id_key.get(),
                                         &cmd_id,
                                         /*auto_commit=*/false,
                                         /*always_redirect=*/true,
                                         txm);
        success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_id);
        if (!success)
        {
            txservice::AbortTx(txm);
            return false;
        }

        uint64_t next_id = 1;
        if (cmd_id.result_.err_code_ == RD_OK)
        {
            try
            {
                next_id = std::stoull(cmd_id.result_.str_);
            }
            catch (...)
            {
                next_id = 1;
            }
        }

        uint64_t id = next_id;
        std::string new_next_id_val = std::to_string(next_id + 1);
        encoded_id = b255e(id);

        cmd_set_id = std::make_unique<SetCommand>(new_next_id_val);
        tx_req_set_id = std::make_unique<ObjectCommandTxRequest>(
            server_->NamespaceTableName(),
            next_id_key.get(),
            cmd_set_id.get(),
            /*auto_commit=*/false,
            /*always_redirect=*/true,
            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, tx_req_set_id.get());
        if (!success)
        {
            txservice::AbortTx(txm);
            return false;
        }
    }

    token_raw = std::string(token.RawView());
    SetCommand cmd_set_ns(token_raw);
    ObjectCommandTxRequest tx_req_set_ns(server_->NamespaceTableName(),
                                         &check_ns_key,
                                         &cmd_set_ns,
                                         /*auto_commit=*/false,
                                         /*always_redirect=*/true,
                                         txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_set_ns);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    SetCommand cmd_set_token(encoded_id);
    ObjectCommandTxRequest tx_req_set_token(server_->NamespaceTableName(),
                                            &check_token_key,
                                            &cmd_set_token,
                                            /*auto_commit=*/false,
                                            /*always_redirect=*/true,
                                            txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_set_token);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    EloqKey i_key = EloqKey::Raw(std::string(kIdToNamePrefix) + encoded_id);

    SetCommand cmd_set_i(ns);
    ObjectCommandTxRequest tx_req_set_i(server_->NamespaceTableName(),
                                        &i_key,
                                        &cmd_set_i,
                                        /*auto_commit=*/false,
                                        /*always_redirect=*/true,
                                        txm);
    success = server_->ExecuteNamespaceTxRequest(txm, &tx_req_set_i);
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    auto [commit_success, commit_err] = txservice::CommitTx(txm);
    return commit_success;
}

bool NamespaceStorage::Del(std::string_view ns)
{
    TransactionExecution *txm =
        server_->NewTxm(IsolationLevel::RepeatableRead, CcProtocol::Locking);
    if (txm == nullptr)
        return false;

    // Define all keys, commands, and request objects at function scope using
    // unique_ptr to prevent stack lifetime bugs
    std::unique_ptr<EloqKey> check_ns_key = std::make_unique<EloqKey>(
        EloqKey::Raw(std::string(kNamespacePrefix) + std::string(ns)));
    std::unique_ptr<EloqKey> check_token_key;
    std::unique_ptr<EloqKey> gc_key;
    std::unique_ptr<EloqKey> i_key;
    std::unique_ptr<EloqKey> e_key;
    std::unique_ptr<EloqKey> e_get_key;

    std::unique_ptr<GetCommand> cmd_ns = std::make_unique<GetCommand>();
    std::unique_ptr<ObjectCommandTxRequest> tx_req_ns =
        std::make_unique<ObjectCommandTxRequest>(server_->NamespaceTableName(),
                                                 check_ns_key.get(),
                                                 cmd_ns.get(),
                                                 /*auto_commit=*/false,
                                                 /*always_redirect=*/true,
                                                 txm);

    bool success = server_->ExecuteNamespaceTxRequest(txm, tx_req_ns.get());
    if (!success || cmd_ns->result_.err_code_ != RD_OK)
    {
        txservice::AbortTx(txm);
        return false;
    }

    std::string token = cmd_ns->result_.str_;
    std::string encoded_id;

    check_token_key = std::make_unique<EloqKey>(
        EloqKey::Raw(std::string(kTokenPrefix) + token));

    std::unique_ptr<GetCommand> cmd_t = std::make_unique<GetCommand>();
    std::unique_ptr<ObjectCommandTxRequest> tx_req_t =
        std::make_unique<ObjectCommandTxRequest>(server_->NamespaceTableName(),
                                                 check_token_key.get(),
                                                 cmd_t.get(),
                                                 /*auto_commit=*/false,
                                                 /*always_redirect=*/true,
                                                 txm);
    success = server_->ExecuteNamespaceTxRequest(txm, tx_req_t.get());
    if (success && cmd_t->result_.err_code_ == RD_OK)
    {
        encoded_id = cmd_t->result_.str_;
    }

    uint64_t epoch = 1;
    std::unique_ptr<GetCommand> cmd_e;
    std::unique_ptr<ObjectCommandTxRequest> tx_req_e;
    std::unique_ptr<SetCommand> set_gc_cmd;
    std::unique_ptr<ObjectCommandTxRequest> set_gc_req;
    std::unique_ptr<DelCommand> cmd_del_i;
    std::unique_ptr<ObjectCommandTxRequest> tx_req_del_i;
    std::unique_ptr<DelCommand> cmd_del_e;
    std::unique_ptr<ObjectCommandTxRequest> tx_req_del_e;

    if (!encoded_id.empty())
    {
        e_get_key = std::make_unique<EloqKey>(
            EloqKey::Raw(std::string(kEpochPrefix) + encoded_id));
        cmd_e = std::make_unique<GetCommand>();
        tx_req_e = std::make_unique<ObjectCommandTxRequest>(
            server_->NamespaceTableName(),
            e_get_key.get(),
            cmd_e.get(),
            /*auto_commit=*/false,
            /*always_redirect=*/true,
            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, tx_req_e.get());
        if (success && cmd_e->result_.err_code_ == RD_OK)
        {
            try
            {
                epoch = std::stoull(cmd_e->result_.str_);
            }
            catch (...)
            {
                epoch = 1;
            }
        }

        // Register GC record for async deletion
        gc_key = std::make_unique<EloqKey>(EloqKey::Raw(
            std::string(kGcPrefix) + encoded_id + ":" + std::to_string(epoch)));
        set_gc_cmd = std::make_unique<SetCommand>("1");
        set_gc_req = std::make_unique<ObjectCommandTxRequest>(
            server_->NamespaceTableName(),
            gc_key.get(),
            set_gc_cmd.get(),
            /*auto_commit=*/false,
            /*always_redirect=*/true,
            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, set_gc_req.get());
        if (!success)
        {
            txservice::AbortTx(txm);
            return false;
        }
    }

    std::unique_ptr<DelCommand> cmd_del_ns = std::make_unique<DelCommand>();
    std::unique_ptr<ObjectCommandTxRequest> tx_req_del_ns =
        std::make_unique<ObjectCommandTxRequest>(server_->NamespaceTableName(),
                                                 check_ns_key.get(),
                                                 cmd_del_ns.get(),
                                                 /*auto_commit=*/false,
                                                 /*always_redirect=*/true,
                                                 txm);
    success = server_->ExecuteNamespaceTxRequest(txm, tx_req_del_ns.get());
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    std::unique_ptr<DelCommand> cmd_del_token = std::make_unique<DelCommand>();
    std::unique_ptr<ObjectCommandTxRequest> tx_req_del_token =
        std::make_unique<ObjectCommandTxRequest>(server_->NamespaceTableName(),
                                                 check_token_key.get(),
                                                 cmd_del_token.get(),
                                                 /*auto_commit=*/false,
                                                 /*always_redirect=*/true,
                                                 txm);
    success = server_->ExecuteNamespaceTxRequest(txm, tx_req_del_token.get());
    if (!success)
    {
        txservice::AbortTx(txm);
        return false;
    }

    if (!encoded_id.empty())
    {
        i_key = std::make_unique<EloqKey>(
            EloqKey::Raw(std::string(kIdToNamePrefix) + encoded_id));
        cmd_del_i = std::make_unique<DelCommand>();
        tx_req_del_i = std::make_unique<ObjectCommandTxRequest>(
            server_->NamespaceTableName(),
            i_key.get(),
            cmd_del_i.get(),
            /*auto_commit=*/false,
            /*always_redirect=*/true,
            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, tx_req_del_i.get());
        if (!success)
        {
            txservice::AbortTx(txm);
            return false;
        }

        e_key = std::make_unique<EloqKey>(
            EloqKey::Raw(std::string(kEpochPrefix) + encoded_id));
        cmd_del_e = std::make_unique<DelCommand>();
        tx_req_del_e = std::make_unique<ObjectCommandTxRequest>(
            server_->NamespaceTableName(),
            e_key.get(),
            cmd_del_e.get(),
            /*auto_commit=*/false,
            /*always_redirect=*/true,
            txm);
        success = server_->ExecuteNamespaceTxRequest(txm, tx_req_del_e.get());
        if (!success)
        {
            txservice::AbortTx(txm);
            return false;
        }
    }

    auto [commit_success, commit_err] = txservice::CommitTx(txm);
    return commit_success;
}

std::map<NamespaceToken, std::string> NamespaceStorage::List()
{
    std::map<NamespaceToken, std::string> result;
    TransactionExecution *txm =
        server_->NewTxm(IsolationLevel::RepeatableRead, CcProtocol::Locking);
    if (txm == nullptr)
        return result;

    std::vector<std::unique_ptr<EloqKey>> get_keys;
    std::vector<std::unique_ptr<GetCommand>> get_cmds;
    std::vector<std::unique_ptr<ObjectCommandTxRequest>> get_reqs;

    EloqKey start_key = EloqKey::Raw(kNamespacePrefix);
    EloqKey end_key = EloqKey::Raw(kNamespaceScanEnd);

    TxKey start_tx_key(&start_key);
    TxKey end_tx_key(&end_key);

    txservice::BucketScanSavePoint save_point;

    const TableName &table_name = *(server_->NamespaceTableName());
    CatalogKey catalog_key(table_name);
    TxKey cat_tx_key(&catalog_key);
    CatalogRecord catalog_rec;
    ReadTxRequest read_req(&txservice::catalog_ccm_name,
                           0,
                           &cat_tx_key,
                           &catalog_rec,
                           false,
                           false,
                           true,
                           0,
                           false,
                           false,
                           false,
                           nullptr,
                           nullptr,
                           txm);
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        txservice::AbortTx(txm);
        return result;
    }
    uint64_t schema_version = catalog_rec.SchemaTs();

    ScanOpenTxRequest scan_open(server_->NamespaceTableName(),
                                schema_version,
                                ScanIndexType::Primary,
                                &start_tx_key,
                                true,
                                &end_tx_key,
                                false,
                                ScanDirection::Forward,
                                false,
                                false,
                                false,
                                false,
                                true,
                                false,
                                true,
                                false,
                                nullptr,
                                nullptr,
                                txm,
                                -1,
                                "",
                                &save_point);

    bool success = server_->ExecuteNamespaceTxRequest(txm, &scan_open);
    if (!success)
    {
        txservice::AbortTx(txm);
        return result;
    }

    uint64_t scan_alias = scan_open.Result();
    if (scan_alias == UINT64_MAX)
    {
        txservice::AbortTx(txm);
        return result;
    }

    size_t current_index = 0;
    size_t plan_size = save_point.PlanSize();
    txservice::BucketScanPlan plan = save_point.PickPlan(current_index);
    std::vector<txservice::ScanBatchTuple> scan_batch;

    while (current_index < plan_size)
    {
        scan_batch.clear();
        ScanBatchTxRequest scan_batch_req(scan_alias,
                                          *server_->NamespaceTableName(),
                                          &scan_batch,
                                          nullptr,
                                          nullptr,
                                          txm,
                                          -1,
                                          "",
                                          &plan);

        success = server_->ExecuteNamespaceTxRequest(txm, &scan_batch_req);
        if (!success)
        {
            txservice::AbortTx(txm);
            return {};
        }

        for (const auto &tuple : scan_batch)
        {
            if (tuple.status_ != txservice::RecordStatus::Normal)
            {
                continue;
            }
            std::string full_key = tuple.key_.ToString();
            if (full_key.size() > kNamespacePrefix.size() &&
                full_key.substr(0, kNamespacePrefix.size()) == kNamespacePrefix)
            {
                std::string ns_name = full_key.substr(kNamespacePrefix.size());
                auto key_obj =
                    std::make_unique<EloqKey>(EloqKey::Raw(full_key));

                auto get_cmd = std::make_unique<GetCommand>();
                auto get_req = std::make_unique<ObjectCommandTxRequest>(
                    server_->NamespaceTableName(),
                    key_obj.get(),
                    get_cmd.get(),
                    /*auto_commit=*/false,
                    /*always_redirect=*/true,
                    txm);

                if (server_->ExecuteNamespaceTxRequest(txm, get_req.get()) &&
                    get_cmd->result_.err_code_ == RD_OK)
                {
                    NamespaceToken token(get_cmd->result_.str_);
                    result[token] = ns_name;
                }

                get_keys.push_back(std::move(key_obj));
                get_cmds.push_back(std::move(get_cmd));
                get_reqs.push_back(std::move(get_req));
            }
        }

        if (scan_batch_req.Result())
        {
            current_index++;
            if (current_index < plan_size)
            {
                plan = save_point.PickPlan(current_index);
            }
        }
    }

    txservice::CommitTx(txm);
    return result;
}

void NamespaceStorage::StartGCDaemon()
{
    if (gc_)
    {
        gc_->Start();
    }
}

void NamespaceStorage::StopGCDaemon()
{
    if (gc_)
    {
        gc_->Stop();
    }
}

}  // namespace EloqKV
