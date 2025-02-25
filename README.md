


# EloqKV  
**Redis-Compatible, Database-Powered — ACID Transactions, Tiered Storage & SQL-Style Syntax, Built for Real Workloads.**  

[![GitHub Stars](https://img.shields.io/github/stars/eloqdata/eloqkv?style=social)](https://github.com/eloqdata/eloqkv/stargazers)
---

## 1. Overview  
EloqKV is a **Redis-compatible database** designed for developers who need **ACID transactions, tiered storage, and SQL-style syntax** — all while keeping Redis' simplicity.  

**Why Choose EloqKV Over Redis?**  
| Feature                      | Redis                      | EloqKV                               |
| ---------------------------- | -------------------------- | ------------------------------------ |
| **Transactions**             | `MULTI/EXEC` (No Rollback) | `BEGIN/COMMIT/ROLLBACK` (ACID)       |
| **Distributed Transactions** | `CROSSSLOT` Error          | ACID distributed transactions        |
| **Data Durability**          | AOF/RDB snapshots          | WAL + Tiered Storage                 |
| **Scalability**              | Single-threaded            | Multi-threaded (1.6million QPS/node) |
| **Cold Data**                | Memory-only                | Auto-tiering to disk                 |

👉 **Use Cases**: Real-time analytics, financial systems, IoT data streams — anywhere you need Redis’ speed **but** can’t compromise on reliability.  

---

## 2. Key Features  

### 🛠️ **ACID Transactions with SQL-Style Syntax**  
```redis  
-- Transfer funds between accounts atomically  
BEGIN  
  GET user:1000:balance     	 -- returns 1000
  INCRBY user:1000:balance -500  -- returns ok
  INCRBY user:2000:balance +500  -- returns ok
COMMIT  
-- Rollback on failure  
```  
*No more Lua scripts or `MULTI` limitations — write transactions like a SQL database.*  
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

### ⚡ **Vertical & Horizontal Scaling**  
- Single node: Up to **1.6M QPS** (benchmarked on AWS c6g.8xlarge).  
- Distributed: Scale horizontally with distributed transactions so that you can still use it as a single node EloqKv.  

### 🔄 **Redis API Compatibility**  
```bash  
redis-cli -h eloqkv-server SET key "value"  # Works out of the box!  
```  
*Zero code changes needed. Check out our [supported Redis commands](https://www.eloqdata.com/eloqkv/kvstore_compatibility).*  


---

## 3. Architecture  

![EloqKv Architecture](https://www.eloqdata.com/assets/images/eloq_arch_new2-4da3661f1895e2bf97f30463b59919b6.png "EloqKv Architecture")

---

## 4. Install with Docker  
**1. Start a Single Node:**  
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

---

## 5. Build from Source  

Install dependencies:
```bash
bash scripts/install_dependency_ubuntu2404.sh
```

Build:
```bash
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=./install -DWITH_KV_STORE=ROCKSDB -DBUILD_WITH_TESTS=ON -DWITH_LOG_SERVICE=OFF -DFORK_HM_PROCESS=OFF ..
make -j
make install
```

Run:
```bash
cd install
./bin/eloqkv
```

---

**Star This Repo ⭐** to Support Our Journey — Every Star Helps Us Reach More Developers!  

