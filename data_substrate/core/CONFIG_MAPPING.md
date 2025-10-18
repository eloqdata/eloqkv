# Configuration Parameter Mapping: eloqsql, eloqkv, and Data Substrate

## Overview

This document provides a comprehensive mapping of all configuration parameters across the three systems:
- **eloqsql**: MySQL system variables (MYSQL_SYSVAR)
- **eloqkv**: gflags (DEFINE_*)
- **Data Substrate**: gflags (DEFINE_*)

## Configuration Categories

### 1. Core Data Substrate Parameters

| Parameter | eloqsql (MYSQL_SYSVAR) | eloqkv (DEFINE_*) | Data Substrate (DEFINE_*) | Description |
|-----------|------------------------|-------------------|---------------------------|-------------|
| **Data Path** | `eloq_data_path` | `eloq_data_path` | `eloq_data_path` | Path for cc_ng and tx_log |
| **Core Number** | `eloq_core_num` | `core_number` | `core_num` | Number of cores for transaction service |
| **Checkpointer Interval** | `eloq_checkpointer_interval_sec` | `checkpoint_interval` | `checkpointer_interval` | Interval time(seconds) of checkpoint |
| **Node Memory Limit** | `eloq_node_memory_limit_mb` | `node_memory_limit_mb` | `node_memory_limit_mb` | txservice node_memory_limit_mb |
| **Checkpointer Delay** | `eloq_checkpointer_delay_sec` | - | `checkpointer_delay_seconds` | Checkpointer delay in seconds |
| **Active TX TS Interval** | `eloq_collect_active_tx_ts_interval_sec` | - | `collect_active_tx_ts_interval_seconds` | Collect active tx timestamp interval |
| **Max Standby Lag** | - | `max_standby_lag` | `max_standby_lag` | Maximum standby lag |
| **Kickout Data for Test** | `eloq_kickout_data_for_test` | `kickout_data_for_test` | `kickout_data_for_test` | Clean data for test |
| **Use Key Cache** | `eloq_use_key_cache` | - | `enable_key_cache` | Enable key cache |
| **Enable Heap Defragment** | `eloq_enable_heap_defragment` | `enable_heap_defragment` | `enable_shard_heap_defragment` | Enable heap defragmentation |
| **Enable data WAL Log** | `enable_wal` | Enable WAL |
| **Enbale Data Store** | `enable_data_store` | Enable data storage |

### 2. Network and Cluster Configuration

| Parameter | eloqsql (MYSQL_SYSVAR) | eloqkv (DEFINE_*) | Data Substrate (DEFINE_*) | Description |
|-----------|------------------------|-------------------|---------------------------|-------------|
| **Local IP** | `eloq_local_ip` | `ip` | - | Local IP address |
| **IP List** | `eloq_ip_list` | `ip_port_list` | - | Server cluster ip port list |
| **Standby IP List** | `eloq_standby_ip_list` | `standby_ip_port_list` | - | Standby nodes ip:port list |
| **Voter IP List** | `eloq_voter_ip_list` | `voter_ip_port_list` | - | Voter nodes ip:port list |
| **Host Manager IP** | `eloq_hm_ip` | `hm_ip` | - | Host manager ip address |
| **Host Manager Port** | - | `hm_port` | - | Host manager port |
| **Host Manager Bin** | `eloq_hm_bin_path` | `hm_bin` | - | Host manager binary path |
| **Cluster Config File** | `eloq_cluster_config_file` | `cluster_config_file` | - | Path for cluster config file |
| **Node Group Replica Num** | `eloq_node_group_replica_num` | `tx_nodegroup_replica_num` | - | Replica number of one txservice node group |

### 3. Log Service Configuration

| Parameter | eloqsql (MYSQL_SYSVAR) | eloqkv (DEFINE_*) | Data Substrate (DEFINE_*) | Description |
|-----------|------------------------|-------------------|---------------------------|-------------|
| **TxLog Service List** | `eloq_txlog_service_list` | `txlog_service_list` | `txlog_service_list` | Log group servers configuration |
| **TxLog Group Replica Num** | `eloq_txlog_group_replica_num` | `txlog_group_replica_num` | `txlog_group_replica_num` | Replica number of one log group |
| **TxLog RocksDB Storage Path** | `eloq_txlog_rocksdb_storage_path` | `txlog_rocksdb_storage_path` | - | Storage path for txlog rocksdb state |
| **Log Service Data Path** | - | `log_service_data_path` | `log_service_data_path` | Log service data path |
| **RocksDB Scan Threads** | `eloq_logserver_rocksdb_scan_thread_num` | `txlog_rocksdb_scan_threads` | `rocksdb_scan_threads` | RocksDB scan threads |
| **Log Server Snapshot Interval** | `eloq_logserver_snapshot_interval` | `logserver_snapshot_interval` | `logserver_snapshot_interval` | Log server snapshot interval |
| **Enable TxLog Request Checkpoint** | `eloq_enable_txlog_request_checkpoint` | `enable_txlog_request_checkpoint` | `enable_txlog_request_checkpoint` | Enable txlog request checkpoint |
| **Notify Checkpointer Threshold** | `eloq_notify_checkpointer_threshold_size` | `notify_checkpointer_threshold_size` | `notify_checkpointer_threshold_size` | Notify checkpointer threshold size |
| **Check Replay Log Size Interval** | `eloq_check_replay_log_size_interval_sec` | `check_replay_log_size_interval_sec` | - | Check replay log size interval |
| **TxLog RocksDB Max Write Buffer Number** | `eloq_txlog_rocksdb_max_write_buffer_number` | `txlog_rocksdb_max_write_buffer_number` | `txlog_rocksdb_max_write_buffer_number` | RocksDB max write buffer number for txlog |
| **TxLog RocksDB Max Background Jobs** | `eloq_txlog_rocksdb_max_background_jobs` | `txlog_rocksdb_max_background_jobs` | `txlog_rocksdb_max_background_jobs` | RocksDB max background jobs for txlog |
| **TxLog RocksDB Target File Size Base** | `eloq_txlog_rocksdb_target_file_size_base` | `txlog_rocksdb_target_file_size_base` | `txlog_rocksdb_target_file_size_base` | RocksDB target file size base for txlog |
| **TxLog RocksDB SST Files Size Limit** | `eloq_txlog_rocksdb_sst_files_size_limit` | `txlog_rocksdb_sst_files_size_limit` | - | RocksDB SST files size limit for txlog |
| **TxLog RocksDB Cloud Region** | `eloq_txlog_rocksdb_cloud_region` | `txlog_rocksdb_cloud_region` | `txlog_rocksdb_cloud_region` | Cloud service region for txlog |
| **TxLog RocksDB Cloud Bucket Name** | `eloq_txlog_rocksdb_cloud_bucket_name` | `txlog_rocksdb_cloud_bucket_name` | `txlog_rocksdb_cloud_bucket_name` | Cloud storage bucket name for txlog |
| **TxLog RocksDB Cloud Bucket Prefix** | `eloq_txlog_rocksdb_cloud_bucket_prefix` | `txlog_rocksdb_cloud_bucket_prefix` | `txlog_rocksdb_cloud_bucket_prefix` | Cloud storage bucket prefix for txlog |
| **TxLog RocksDB Cloud Object Path** | `eloq_txlog_rocksdb_cloud_object_path` | `txlog_rocksdb_cloud_object_path` | `txlog_rocksdb_cloud_object_path` | Cloud object path for txlog |
| **TxLog RocksDB Cloud Endpoint URL** | `eloq_txlog_rocksdb_cloud_endpoint_url` | `txlog_rocksdb_cloud_endpoint_url` | `txlog_rocksdb_cloud_endpoint_url` | Cloud storage endpoint URL for txlog |
| **TxLog RocksDB Cloud SST File Cache Size** | `eloq_txlog_rocksdb_cloud_sst_file_cache_size` | `txlog_rocksdb_cloud_sst_file_cache_size` | `txlog_rocksdb_cloud_sst_file_cache_size` | Cloud SST file cache size for txlog |
| **TxLog RocksDB Cloud Ready Timeout** | `eloq_txlog_rocksdb_cloud_ready_timeout` | `txlog_rocksdb_cloud_ready_timeout` | `txlog_rocksdb_cloud_ready_timeout` | Cloud ready timeout for txlog |
| **TxLog RocksDB Cloud File Deletion Delay** | `eloq_txlog_rocksdb_cloud_file_deletion_delay` | `txlog_rocksdb_cloud_file_deletion_delay` | `txlog_rocksdb_cloud_file_deletion_delay` | Cloud file deletion delay for txlog |

### 4. Storage Configuration

| Parameter | eloqsql (MYSQL_SYSVAR) | eloqkv (DEFINE_*) | Data Substrate (DEFINE_*) | Description |
|-----------|------------------------|-------------------|---------------------------|-------------|
| **Enable Cache Replacement** | - | `enable_cache_replacement` | `enable_cache_replacement` | Enable cache replacement |
| **RocksDB Storage Path** | - | - | `rocksdb_storage_path` | RocksDB storage path |

#### 4.1. DynamoDB Configuration

| Parameter | eloqsql (MYSQL_SYSVAR) | eloqkv (DEFINE_*) | Data Substrate (DEFINE_*) | Description |
|-----------|------------------------|-------------------|---------------------------|-------------|
| **DynamoDB Endpoint** | `eloq_dynamodb_endpoint` | `dynamodb_endpoint` | `dynamodb_endpoint` | DynamoDB endpoint |
| **DynamoDB Region** | `eloq_dynamodb_region` | `dynamodb_region` | `dynamodb_region` | DynamoDB region |
| **DynamoDB Keyspace** | - | `dynamodb_keyspace` | `dynamodb_keyspace` | DynamoDB keyspace |
| **AWS Access Key ID** | `eloq_aws_access_key_id` | `aws_access_key_id` | `aws_access_key_id` | AWS access key ID |
| **AWS Secret Key** | `eloq_aws_secret_key` | `aws_secret_key` | `aws_secret_key` | AWS secret key |

#### 4.2. BigTable Configuration (eloqsql only)

| Parameter | eloqsql (MYSQL_SYSVAR) | Description |
|-----------|------------------------|-------------|
| **BigTable Project ID** | `eloq_bigtable_project_id` | BigTable project ID |
| **BigTable Instance ID** | `eloq_bigtable_instance_id` | BigTable instance ID |

#### 4.3. Data Store Service (ELOQDS)
| Parameter | eloqsql (MYSQL_SYSVAR) | eloqkv (DEFINE_*) | Description |
|-----------|------------------------|-------------------|-------------|
| **DSS Config File Path** | `eloq_dss_config_file_path` | - | DSS config file path |
| **DSS Peer Node** | `eloq_dss_peer_node` | `eloq_dss_peer_node` | DSS peer node |
| **DSS Branch Name** | `eloq_dss_branch_name` | `eloq_dss_branch_name` | DSS branch name |
| **DSS RocksDB Cloud Config** | Multiple `eloq_dss_rocksdb_cloud_*` | - | DSS RocksDB cloud configuration |

#### 4.4. EloqStore (DATA_STORE_TYPE_ELOQDSS_ELOQSTORE)
| Parameter | eloqsql (MYSQL_SYSVAR) | Description |
|-----------|------------------------|-------------|
| **EloqStore Worker Num** | `eloq_eloqstore_worker_num` | EloqStore worker number |
| **EloqStore Data Path List** | `eloq_eloqstore_data_path_list` | EloqStore data path list |
| **EloqStore Open Files Limit** | `eloq_eloqstore_open_files_limit` | EloqStore open files limit |
| **EloqStore Cloud Store Path** | `eloq_eloqstore_cloud_store_path` | EloqStore cloud store path |
| **EloqStore GC Threads** | `eloq_eloqstore_gc_threads` | EloqStore GC threads |
| **EloqStore Cloud Worker Count** | `eloq_eloqstore_cloud_worker_count` | EloqStore cloud worker count |
| **EloqStore Data Page Restart Interval** | `eloq_eloqstore_data_page_restart_interval` | EloqStore data page restart interval |
| **EloqStore Index Page Restart Interval** | `eloq_eloqstore_index_page_restart_interval` | EloqStore index page restart interval |
| **EloqStore Init Page Count** | `eloq_eloqstore_init_page_count` | EloqStore init page count |
| **EloqStore Skip Verify Checksum** | `eloq_eloqstore_skip_verify_checksum` | EloqStore skip verify checksum |
| **EloqStore Index Buffer Pool Size** | `eloq_eloqstore_index_buffer_pool_size` | EloqStore index buffer pool size |
| **EloqStore Manifest Limit** | `eloq_eloqstore_manifest_limit` | EloqStore manifest limit |
| **EloqStore IO Queue Size** | `eloq_eloqstore_io_queue_size` | EloqStore IO queue size |
| **EloqStore Max Inflight Write** | `eloq_eloqstore_max_inflight_write` | EloqStore max inflight write |
| **EloqStore Max Write Batch Pages** | `eloq_eloqstore_max_write_batch_pages` | EloqStore max write batch pages |
| **EloqStore Buf Ring Size** | `eloq_eloqstore_buf_ring_size` | EloqStore buf ring size |
| **EloqStore Coroutine Stack Size** | `eloq_eloqstore_coroutine_stack_size` | EloqStore coroutine stack size |
| **EloqStore Num Retained Archives** | `eloq_eloqstore_num_retained_archives` | EloqStore num retained archives |
| **EloqStore Archive Interval** | `eloq_eloqstore_archive_interval_secs` | EloqStore archive interval |
| **EloqStore Max Archive Tasks** | `eloq_eloqstore_max_archive_tasks` | EloqStore max archive tasks |
| **EloqStore File Amplify Factor** | `eloq_eloqstore_file_amplify_factor` | EloqStore file amplify factor |
| **EloqStore Local Space Limit** | `eloq_eloqstore_local_space_limit` | EloqStore local space limit |
| **EloqStore Reserve Space Ratio** | `eloq_eloqstore_reserve_space_ratio` | EloqStore reserve space ratio |
| **EloqStore Data Page Size** | `eloq_eloqstore_data_page_size` | EloqStore data page size |
| **EloqStore Pages Per File Shift** | `eloq_eloqstore_pages_per_file_shift` | EloqStore pages per file shift |
| **EloqStore Overflow Pointers** | `eloq_eloqstore_overflow_pointers` | EloqStore overflow pointers |
| **EloqStore Data Append Mode** | `eloq_e

### 5. Metrics Configuration

| Parameter | eloqsql (MYSQL_SYSVAR) | eloqkv (DEFINE_*) | Data Substrate (DEFINE_*) | Description |
|-----------|------------------------|-------------------|---------------------------|-------------|
| **Enable Metrics** | `eloq_enable_metrics` | - | `enable_metrics` | Enable metrics |
| **Metrics Port** | `eloq_metrics_port` | - | `metrics_port` | Metrics port |
| **Enable TX Metrics** | `eloq_enable_tx_metrics` | - | `enable_tx_metrics` | Enable transaction metrics |
| **Enable Log Service Metrics** | `eloq_enable_log_service_metrics` | - | `enable_log_service_metrics` | Enable log service metrics |
| **Enable Cache Hit Rate** | `eloq_enable_cache_hit_rate` | - | `enable_cache_hit_rate` | Enable cache hit rate metrics |
| **Enable KV Metrics** | `eloq_enable_kv_metrics` | - | `enable_kv_metrics` | Enable KV store metrics |
| **Enable Remote Request Metrics** | `eloq_enable_remote_request_metrics` | - | `enable_remote_request_metrics` | Enable remote request metrics |
| **Collect TX Duration Round** | `eloq_collect_tx_duration_round` | - | `collect_tx_duration_round` | Collect transaction duration round |
| **Busy Round Threshold** | `eloq_busy_round_threshold` | - | `busy_round_threshold` | Busy round threshold |
| **Collect Memory Usage Round** | `eloq_collect_memory_usage_round` | - | - | Collect memory usage round |
| **Enable Busy Round Metrics** | `eloq_enable_busy_round_metrics` | - | - | Enable busy round metrics |
| **Enable Memory Usage** | `eloq_enable_memory_usage` | - | - | Enable memory usage metrics |
| **Realtime Sampling** | `eloq_realtime_sampling` | - | `realtime_sampling` | Enable realtime sampling |

### 6. Engine-Specific Configuration

#### eloqsql-specific (MySQL system variables)
| Parameter | Description |
|-----------|-------------|
| `eloq_kv_storage` | KV storage type |
| `eloq_insert_semantic` | Insert semantic (insert/upsert) |
| `eloq_auto_increment` | Auto increment configuration |
| `eloq_invalidate_cache_once` | Invalidate cache once |
| `eloq_ddl_skip_kv` | Skip KV for DDL operations |
| `eloq_skip_redo_log` | Skip redo log |
| `eloq_scan_skip_kv` | Skip KV for scan operations |
| `eloq_random_scan_sort` | Random scan sort |
| `eloq_report_debug_info` | Report debug info |
| `eloq_deadlock_interval_sec` | Deadlock check interval |
| `eloq_signal_monitor` | Signal monitor |
| `eloq_partition_type` | Partition type |
| `eloq_range_split_worker_num` | Range split worker number |

#### eloqkv-specific (gflags)
| Parameter | Description |
|-----------|-------------|
| `auto_redirect` | Auto redirect request to remote node |
| `cc_notify` | Notify txrequest sender when cc request finishes |
| `enable_io_uring` | Enable io_uring as IO engine |
| `raft_log_async_fsync` | Raft log fsync performed asynchronously |
| `bind_all` | Listen on all interfaces |
| `port` | Redis port |
| `bootstrap` | Init system tables and exit |
| `maxclients` | Maximum clients |
| `slow_log_threshold` | Slow log threshold |
| `slow_log_max_length` | Slow log max length |
| `enable_redis_stats` | Enable redis statistics |
| `enable_cmd_sort` | Enable command sort in Multi-Exec |
| `isolation_level` | Isolation level of simple commands |
| `protocol` | Concurrency control protocol |
| `txn_isolation_level` | Isolation level of MULTI/EXEC |
| `txn_protocol` | Concurrency control protocol of MULTI/EXEC |
| `snapshot_sync_worker_num` | Snapshot sync worker num |
| `retry_on_occ_error` | Retry transaction on OCC error |
| `fork_host_manager` | Fork host manager process |

### Total Configuration Parameters:
- **eloqsql**: ~150+ parameters (including compile-time macro parameters)
- **eloqkv**: ~75+ parameters (including compile-time macro parameters)  
- **Data Substrate**: ~40+ parameters (core unified parameters)

### Key Observations:
1. **Naming Inconsistencies**: Different naming conventions across systems (e.g., `core_num` vs `core_number`)
2. **Missing Parameters**: Some parameters exist in one system but not others
3. **Compile-time Macros**: Many parameters are wrapped in compile-time macros for conditional compilation
4. **Engine-Specific**: Each system has unique parameters for its specific functionality
5. **Unified Approach**: Data Substrate provides a unified configuration system that can support all three engines

### Recommendations:
1. **Standardize Naming**: Use consistent parameter names across all systems
2. **Complete Coverage**: Ensure all necessary parameters are available in Data Substrate
3. **Documentation**: Maintain this mapping as systems evolve
4. **Validation**: Implement parameter validation and type checking
5. **Migration Path**: Provide clear migration path from existing systems to Data Substrate
