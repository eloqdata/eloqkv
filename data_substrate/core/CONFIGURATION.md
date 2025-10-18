# Data Substrate Configuration System

## Overview

The Data Substrate supports configuration through both INI configuration files and command line flags (gflags). Command line flags take precedence over configuration file values, allowing for flexible deployment and testing scenarios.

## Configuration Sources (Priority Order)

1. **Command Line Flags** (highest priority)
2. **Configuration File** (medium priority)  
3. **Default Values** (lowest priority)

## Configuration File Format

The configuration file uses INI format with the following sections:

### [data_substrate]
Core data substrate configuration parameters.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `eloq_data_path` | string | "eloq_data" | Path for cc_ng and tx_log |
| `core_num` | int | 8 | Number of cores for transaction service |
| `checkpointer_interval` | int | 10 | Interval time(seconds) of checkpoint |
| `node_memory_limit_mb` | int | 8192 | txservice node_memory_limit_mb |
| `checkpointer_delay_seconds` | int | 5 | Checkpointer delay in seconds |
| `collect_active_tx_ts_interval_seconds` | int | 2 | Collect active tx timestamp interval |
| `realtime_sampling` | bool | true | Enable realtime sampling |
| `txlog_service_list` | string | "" | Log group servers configuration |
| `enable_tx_metrics` | bool | true | Enable transaction metrics |
| `enable_log_service_metrics` | bool | true | Enable log service metrics |
| `enable_key_cache` | bool | true | Enable key cache |
| `enable_shard_heap_defragment` | bool | false | Enable shard heap defragmentation |
| `bthread_worker_num` | int | 0 | Number of bthread workers |
| `max_standby_lag` | int | 0 | Maximum standby lag |
| `kickout_data_for_test` | bool | false | Clean data for test |

### [storage]
Storage handler configuration parameters.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `storage_type` | string | "rocksdb" | Storage type (rocksdb, dynamodb, data_store_service) |
| `rocksdb_storage_path` | string | "" | RocksDB storage path |
| `enable_cache_replacement` | bool | true | Enable cache replacement |
| `rocksdb_max_write_buffer_number` | int | 3 | RocksDB max write buffer number |
| `rocksdb_max_background_jobs` | int | 4 | RocksDB max background jobs |
| `rocksdb_target_file_size_base` | string | "64MB" | RocksDB target file size base |

### [dynamodb]
DynamoDB-specific configuration (when storage_type = "dynamodb").

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `dynamodb_endpoint` | string | "" | DynamoDB endpoint |
| `dynamodb_region` | string | "ap-northeast-1" | DynamoDB region |
| `dynamodb_keyspace` | string | "eloq_data" | DynamoDB keyspace |
| `aws_access_key_id` | string | "" | AWS access key ID |
| `aws_secret_key` | string | "" | AWS secret key |

### [log_service]
Log service configuration parameters.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `log_service_data_path` | string | "" | Log service data path |
| `rocksdb_scan_threads` | int | 4 | RocksDB scan threads |
| `txlog_group_replica_num` | int | 3 | Replica number of one log group |
| `logserver_snapshot_interval` | int | 3600 | Log server snapshot interval |
| `enable_txlog_request_checkpoint` | bool | true | Enable txlog request checkpoint |
| `notify_checkpointer_threshold_size` | string | "1GB" | Notify checkpointer threshold size |

### [metrics]
Metrics configuration parameters.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enable_metrics` | bool | true | Enable metrics |
| `metrics_port` | int | 9090 | Metrics port |

## Command Line Flags

All configuration parameters can be overridden using command line flags. The flag names match the parameter names in the configuration file.

### Example Usage

```bash
# Use configuration file only
./data_substrate --config_file=config/sample_data_substrate.ini

# Override specific parameters via command line
./data_substrate --config_file=config/sample_data_substrate.ini \
  --core_num=16 \
  --node_memory_limit_mb=16384 \
  --storage_type=dynamodb

# Use command line flags only (no config file)
./data_substrate \
  --eloq_data_path=/custom/path \
  --core_num=8 \
  --storage_type=rocksdb \
  --rocksdb_storage_path=/custom/rocksdb/path
```

## Configuration Loading Process

1. **Parse Command Line Flags**: gflags parses command line arguments
2. **Load Configuration File**: INI file is parsed if provided
3. **Apply Overrides**: Command line flags override config file values
4. **Use Defaults**: Any unspecified parameters use default values

## Helper Functions

The configuration system uses helper functions to handle the priority logic:

- `GetConfigValue()`: Gets string values with gflags override
- `GetConfigBool()`: Gets boolean values with gflags override  
- `GetConfigInt()`: Gets integer values with gflags override

## Sample Configuration File

See `config/sample_data_substrate.ini` for a complete example configuration file with all available parameters.

## Integration with Engines

The configuration system is designed to work with multiple database engines:

- **eloqsql**: Uses MariaDB-specific catalog factory and system handler
- **eloqkv**: Uses eloqkv-specific catalog factory
- **eloqdoc**: Uses eloqdoc-specific catalog factory and system handler

Engine-specific configurations are handled through compile-time macros (`#ifdef ELOQSQL`, `#ifdef ELOQKV`, `#ifdef ELOQDOC`).

## Best Practices

1. **Use Configuration Files**: For production deployments, use configuration files for consistency
2. **Command Line Overrides**: Use command line flags for testing and development
3. **Default Values**: Rely on sensible defaults for optional parameters
4. **Validation**: Always validate configuration values before use
5. **Documentation**: Keep configuration documentation up to date with code changes
