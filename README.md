<div align="center">
<a href='https://www.eloqdata.com'>
<img src="images/eloqkv_github_logo.jpg" alt="EloqKV" height=150></img>
</a>
  
---

[![License](https://img.shields.io/badge/License-GPL-blue.svg)](https://github.com/eloqdata/eloqkv/blob/readme/LICENSE)
[![Language](https://img.shields.io/badge/language-C++-orange)](https://isocpp.org/)
[![GitHub issues](https://img.shields.io/github/issues/eloqdata/eloqkv)](https://github.com/eloqdata/eloqkv/issues)
[![Release](https://img.shields.io/badge/release-latest-blue)](https://www.eloqdata.com/download)
<a href="https://discord.com/invite/nmYjBkfak6">
  <img alt="EloqKV" src="https://img.shields.io/badge/discord-blue.svg?logo=discord&logoColor=white">
</a>
</div>

# EloqKV  
EloqKV is the most **Cost-Effective, Redis-compatible** database designed for developers who need **ACID transactions, tiered storage, and Session-style syntax** — all while keeping Redis' simplicity.

- [Key Features](#key-features)
- [Quick Start](#quick-start)
- [Architecture](#architecture)
- [Benchmark](#benchmark)
- [Build From Source](#build-from-source)
- [License](#license)
- [See Also](#see-also)


**Why Choose EloqKV Over Redis?**  
| Feature                      | Redis                        | EloqKV                               |
| ---------------------------- | ---------------------------  | ------------------------------------ |
| **Performance**              | Single-threaded              | Multi-threaded (1.6million QPS/node) |
| **Transactions**             | `MULTI/EXEC` (No Rollback)   | `BEGIN/COMMIT/ROLLBACK` (ACID)       |
| **Distributed Transactions** | `CROSSSLOT` Error            | ACID distributed transactions        |
| **Data Durability**          | AOF/RDB snapshots            | Replicated WAL + Tiered Storage      |
| **Cold Data**                | Memory-only                  | Auto-tiering to disk                 |
| **Client Transparency**      | Cluster needs specific client| Single Client                        |


---

## Key Features

### ⚡ **High Performance** 
- **Multi-threaded**: Built with thread-per-core execution and message-passing architecture to fully utilize modern CPUs.
- **Single Node**: Up to **1.6M QPS**, comparable to purpose-built cache systems like DragonflyDB (benchmarked on AWS c6g.8xlarge).
- **Native Distributed**: No Sharding. Scale horizontally with distributed transactions so that you can still use it as a single node EloqKv. 

### 🛠️ **ACID Transactions with Session-Style Syntax**
Besides the standard Redis transaction syntax (MULTI/EXEC), we also support Session-style interactive transactions.

```redis  
-- Transfer funds between accounts atomically  
BEGIN  
  GET user:1000:balance     	 -- returns 1000
  INCRBY user:1000:balance -500  -- returns ok
  INCRBY user:2000:balance +500  -- returns ok
COMMIT  
-- Rollback on failure  
```  
- No more Lua scripts or `MULTI` limitations — write transactions like a SQL database.

### 🌐 **Distributed ACID Transactions** 
**Cross-node strong consistency without `hashtag` constraints**  
```redis  
-- Example of cross-node transfer
BEGIN  
  INCRBY user:1000:balance -500      -- node A  
  HSET order:2000:status "paid"      -- node B  
COMMIT  
```
-   **No  `CROSSSLOT`  Errors**：Enables atomic operations across multiple nodes, unlike Redis Cluster which blocks cross-slot transactions.
    
### 🗃️ **Tiered Storage**  
- **Hot Data**: In-memory for microsecond access.  
- **Cold Data**: Automatically offloaded to disk.  
*Save 70% on memory costs compared to Redis.*  

### 🔄 **Redis API Compatibility**  
```bash  
redis-cli -h eloqkv-server SET key "value"  # Works out of the box!  
```  
- Zero code changes needed. Check out our [supported Redis commands](https://www.eloqdata.com/eloqkv/kvstore_compatibility). 


---

## Quick Start
### Using Docker
We recommend using Docker for a quick start with the EloqKV service.

**1. Start a Single Node using Docker:**  
```bash  
# Create subnet for containers.
docker network create --subnet=172.20.0.0/16 eloqnet

docker run -d --net eloqnet --ip 172.20.0.10 -p 6379:6379 --name=eloqkv eloqdata/eloqkv
```  

**2. Verify Installation:**  
```bash  
redis-cli -h 172.20.0.10

172.20.0.10:6379> set hello world
OK
172.20.0.10:6379> get hello
"world"
```  

### Run with EloqCtl
EloqCtl is the cluster management tool for EloqKV.

To deploy an EloqKV cluster in production, download [EloqCtl](https://www.eloqdata.com/downloadeloqctl) and follow the [deployment guide](https://www.eloqdata.com/eloqsql/cluster-deployment).

### Run with Tarball
Download the EloqKV tarball from the [EloqData website](https://www.eloqdata.com/download/eloqkv).

Follow the [instruction guide](https://www.eloqdata.com/eloqkv/install-from-binary) to set up and run EloqKV on your local machine.

---

## Architecture


<div align="center">
<a href='https://www.eloqdata.com'>
<img src="images/eloqkv_architecture2.png" alt="EloqKV Arch" width=600></img>
</a>
</div>

EloqKV is a decoupled, distributed database built on [Data Substrate](https://www.eloqdata.com/blog/2024/08/11/data-substrate), the innovative new database foundation developed by EloqData.

Each EloqKV instance includes a frontend, compatible with the Redis protocol, deployed together with the core TxService to handle data operations. A logically independent LogService handles Write Ahead Logging (WAL) to ensure persistence, while a Persistent Storage Service manages memory state checkpoints and cold data storage.

In EloqKV, the TxService is responsible for concurrency control, ensuring that transactional operations are consistent. The Log Service can replicate logs and distributes them across different availability zones (AZs) to provide resilience against AZ-level failures. The storage service supports various persistent storage engines, including local options like RocksDB, remote clusters like Cassandra, and cloud storage solutions such as AWS DynamoDB and Object Storage. This persistent storage store cold data for cache misses and provide high availability, even during node failures.

## Benchmark

EloqKV is a **fully featured key-value database** that supports both **distributed caching** and **durable transactional storage**. In both use cases, it delivers outstanding performance compared to other solutions.

### Cache Mode

In cache scenarios, EloqKV significantly outperforms Redis and achieves performance close to Dragonfly. Note that Dragonfly was specifically designed and optimized for. This begs the question of whether designing special database software for limited use cases is profitable. Unlike Redis and DragonflyDB, EloqKV is much more than a single node memory cache. It excels in clustered, durable, and fully ACID-compliant transactional setups.
📖 [See full benchmark ](https://www.eloqdata.com/blog/2024/08/17/benchmark-single-node)


<div align="center">
<a href='https://www.eloqdata.com'>
<img src="images/eloqkvperf1.png" alt="EloqKV" width=500></img>
</a>
</div>

### Persistent Transactional Mode

When running with full durability and ACID guarantees, EloqKV outperforms other Redis-compatible stores like Apache KVRocks.
Unlike KVRocks, which lacks true transactional support, EloqKV offers **real, rollback-capable transactions** with high throughput.
📖 [See full benchmark ](https://www.eloqdata.com/blog/2024/08/25/benchmark-txlog)


<div align="center">
<a href='https://www.eloqdata.com'>
<img src="images/eloqkvperf4.png" alt="EloqKV" width=500></img>
</a>
</div>
  
---

## Build from Source  

### 1. Install Dependencies:
We recommend using our Docker image with pre-installed dependencies for a quick build and run of EloqKV.

```bash
docker pull eloqdata/eloq-build-ubuntu2404:latest
```

Or, you can manually run the following script to install dependencies on your local machine.

```bash
bash scripts/install_dependency_ubuntu2404.sh
```

### 2. Initialize Submodules
Fetch the Transaction Service and its dependencies:

```
git submodule update --init --recursive
```

### 3. Build EloqKV
```bash
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=./install -DWITH_KV_STORE=ROCKSDB ..
make -j
make install
```

### 4. Run EloqKV
```bash
cd install
./bin/eloqkv --port=6389
```

---

## License

EloqKV is under under a dual license. You may choose to use it under the terms of either:
1. GNU General Public License, Version 2 (GPLv2), or
2. GNU Affero General Public License, Version 3 (AGPLv3).

See the [LICENSE](./LICENSE) file for details.

---

## See Also

- [EloqKV Doc](https://www.eloqdata.com/eloqkv/introduction)
- [Try EloqCloud for EloqKV](https://cloud.eloqdata.com)
- [Watch: EloqKV at ApacheCon](https://www.youtube.com/watch?v=33gotnJh7rc)
- [Watch: EloqKV at Monster Scale Cummit](https://www.youtube.com/watch?v=XSuwjiNt0N4)
  
[![GitHub Stars](https://img.shields.io/github/stars/eloqdata/eloqkv?style=social)](https://github.com/eloqdata/eloqkv/stargazers)
**Star This Repo ⭐** to Support Our Journey — Every Star Helps Us Reach More Developers!  

