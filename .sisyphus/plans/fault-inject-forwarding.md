# Fault Injection Forwarding to Log Service

## Context

### Original Request
Enable the EloqKV `fault_inject` Redis command to affect the `eloq_log_service` component when running in the same process.

### Problem Analysis
The codebase has two separate `FaultInject` singleton classes in different namespaces:
- `txservice::FaultInject` - used by tx_service (receives Redis `fault_inject` command)
- `txlog::FaultInject` - used by eloq_log_service (e.g., `log_state_rocksdb_cloud_impl.cpp:571`)

Even when running in the same process, these are **isolated singletons**. The Redis command only injects into `txservice::FaultInject`, so fault injection points in log_service code (like `override_log_retention_seconds`) are never triggered.

### Solution
Conditionally forward the `override_log_retention_seconds` fault injection from `txservice::FaultInject::InjectFault()` to `txlog::FaultInject::Instance().InjectFault()`.

**Compile Conditions Required:**
- `-DWITH_LOG_SERVICE=ON` → defines `WITH_LOG_SERVICE`
- `-DOPEN_LOG_SERVICE=OFF` → does NOT define `OPEN_LOG_SERVICE` (embedded log service)
- `-DWITH_DATA_STORE=ELOQDSS_ROCKSDB_CLOUD_S3` → defines `LOG_STATE_TYPE_RKDB_S3`

**Preprocessor Guard:**
```cpp
#if defined(WITH_LOG_SERVICE) && !defined(OPEN_LOG_SERVICE) && defined(LOG_STATE_TYPE_RKDB_S3)
```

---

## Work Objectives

### Core Objective
Conditionally make `txservice::FaultInject::InjectFault()` forward the `override_log_retention_seconds` fault injection to `txlog::FaultInject`, so that the log retention override in eloq_log_service is triggered when running embedded log service with RocksDB Cloud S3 storage.

### Concrete Deliverables
- Modified `data_substrate/tx_service/include/fault/fault_inject.h` - move `InjectFault` to declaration only
- Modified `data_substrate/tx_service/src/fault/fault_inject.cpp` - implement `InjectFault` with forwarding

### Definition of Done
- [ ] `fault_inject override_log_retention_seconds 0 retention_seconds=10` from Redis CLI affects log_service code
- [ ] Existing fault injection behavior for tx_service remains unchanged
- [ ] Code compiles without errors

### Must Have
- Forward ONLY `override_log_retention_seconds` to `txlog::FaultInject::Instance().InjectFault()`
- Only forward when compile conditions are met: `WITH_LOG_SERVICE && !OPEN_LOG_SERVICE && LOG_STATE_TYPE_RKDB_S3`
- Handle "remove" case - forward removal to txlog as well (for this specific fault name)

### Must NOT Have (Guardrails)
- Do NOT modify `txlog::FaultInject` class
- Do NOT create circular header dependencies
- Do NOT change the public API of `txservice::FaultInject`
- Do NOT forward fault injections other than `override_log_retention_seconds`
- Do NOT forward when compile conditions are not met (e.g., OPEN_LOG_SERVICE mode)

---

## Verification Strategy

### Test Decision
- **Infrastructure exists**: YES (unit tests exist in the codebase)
- **User wants tests**: Manual verification for this change
- **QA approach**: Manual verification with fault injection command

### Manual Verification Procedure
1. Build EloqKV with:
   ```bash
   cmake -DWITH_FAULT_INJECT=ON -DWITH_LOG_SERVICE=ON -DOPEN_LOG_SERVICE=OFF -DWITH_DATA_STORE=ELOQDSS_ROCKSDB_CLOUD_S3 ..
   make -j$(nproc)
   ```
2. Start EloqKV server
3. Use redis-cli to inject fault: `fault_inject override_log_retention_seconds 0 retention_seconds=10`
4. Verify in logs that both `txservice::FaultInject` and `txlog::FaultInject` receive the injection
5. Trigger log purge operation and verify the retention override takes effect

---

## Task Flow

```
Task 1 (header change) → Task 2 (cpp implementation) → Task 3 (verification)
```

---

## TODOs

- [x] 1. Move InjectFault implementation from header to declaration only

  **What to do**:
  - In `data_substrate/tx_service/include/fault/fault_inject.h`
  - Change the inline `InjectFault` method to a declaration only
  - Keep the method signature: `void InjectFault(std::string fault_name, std::string paras);`

  **Must NOT do**:
  - Do not change any other methods
  - Do not modify the FaultEntry class

  **Parallelizable**: NO (Task 2 depends on this)

  **References**:
  - `data_substrate/tx_service/include/fault/fault_inject.h:197-219` - Current inline implementation to be replaced with declaration

  **Acceptance Criteria**:
  - [ ] Line 197-219 replaced with single declaration: `void InjectFault(std::string fault_name, std::string paras);`
  - [ ] Header file compiles (no syntax errors)

  **Commit**: NO (groups with Task 2)

---

- [x] 2. Implement InjectFault with conditional forwarding to txlog::FaultInject

  **What to do**:
  - In `data_substrate/tx_service/src/fault/fault_inject.cpp`
  - Add conditional include for log_service's fault_inject header (guarded by preprocessor)
  - Add the `InjectFault` implementation that:
    1. Logs the fault injection (existing behavior)
    2. **Conditionally** forwards ONLY `override_log_retention_seconds` to `txlog::FaultInject::Instance().InjectFault()`
    3. Handles "remove" case - also forwards removal for this fault name
    4. Handles "at_once" case - triggers action locally
    5. Otherwise stores in local map

  **Implementation**:
  ```cpp
  // Add after existing includes (around line 28):
  #if defined(WITH_LOG_SERVICE) && !defined(OPEN_LOG_SERVICE) && defined(LOG_STATE_TYPE_RKDB_S3)
  #include "fault_inject.h"  // txlog::FaultInject from eloq_log_service
  #endif
  
  // Add after TriggerAction implementation (around line 247):
  void FaultInject::InjectFault(std::string fault_name, std::string paras)
  {
      LOG(INFO) << "FaultInject name=" << fault_name << "  paras=" << paras;
      
  #if defined(WITH_LOG_SERVICE) && !defined(OPEN_LOG_SERVICE) && defined(LOG_STATE_TYPE_RKDB_S3)
      // Forward override_log_retention_seconds to txlog::FaultInject for embedded log_service
      if (fault_name == "override_log_retention_seconds")
      {
          txlog::FaultInject::Instance().InjectFault(fault_name, paras);
      }
  #endif
      
      // To remove the pointed fault inject.
      if (paras.compare("remove") == 0)
      {
          std::lock_guard<std::mutex> lk(mux_);
          injected_fault_map_.erase(fault_name);
          return;
      }

      FaultEntry fentry(fault_name, paras);
      if (fault_name.compare("at_once") == 0)
      {
          // If fault name equal "at_once", run it at once
          TriggerAction(&fentry);
      }
      else
      {
          std::lock_guard<std::mutex> lk(mux_);
          injected_fault_map_.try_emplace(fault_name, fentry);
      }
  }
  ```

  **Must NOT do**:
  - Do not modify TriggerAction
  - Do not change the forwarding to remote nodes logic
  - Do not forward fault names other than `override_log_retention_seconds`

  **Parallelizable**: NO (depends on Task 1)

  **References**:
  - `data_substrate/tx_service/src/fault/fault_inject.cpp:22-28` - Existing includes
  - `data_substrate/tx_service/src/fault/fault_inject.cpp:247-249` - End of file where to add implementation
  - `data_substrate/eloq_log_service/include/fault_inject.h:194-215` - txlog::FaultInject::InjectFault signature
  - `data_substrate/tx_service/include/fault/fault_inject.h:197-219` - Original inline implementation to move
  - `data_substrate/build_eloq_log_service.cmake:25-26` - Where `LOG_STATE_TYPE_RKDB_S3` is defined
  - `data_substrate/eloq_log_service/src/log_state_rocksdb_cloud_impl.cpp:571-596` - Usage of `override_log_retention_seconds` fault

  **Acceptance Criteria**:
  - [ ] Conditional include added: `#if defined(...) ... #include "fault_inject.h" ... #endif`
  - [ ] `FaultInject::InjectFault` method implemented in .cpp file
  - [ ] Method conditionally forwards ONLY `override_log_retention_seconds` to `txlog::FaultInject`
  - [ ] Code compiles without errors with full flags: `cmake -DWITH_FAULT_INJECT=ON -DWITH_LOG_SERVICE=ON -DOPEN_LOG_SERVICE=OFF -DWITH_DATA_STORE=ELOQDSS_ROCKSDB_CLOUD_S3 .. && make -j$(nproc)` → success
  - [ ] Code also compiles without log service flags (no forwarding): `cmake -DWITH_FAULT_INJECT=ON .. && make -j$(nproc)` → success

  **Commit**: YES
  - Message: `feat(fault_inject): forward override_log_retention_seconds to txlog::FaultInject for embedded log_service`
  - Files: `data_substrate/tx_service/include/fault/fault_inject.h`, `data_substrate/tx_service/src/fault/fault_inject.cpp`
  - Pre-commit: `make -j$(nproc)`

---

- [x] 3. Verify fault injection reaches log_service

  **What to do**:
  - Build with full fault injection and embedded log service flags
  - Start EloqKV
  - Test fault injection command
  - Verify logs show conditional forwarding working

  **Parallelizable**: NO (depends on Task 2)

  **References**:
  - `data_substrate/eloq_log_service/src/log_state_rocksdb_cloud_impl.cpp:571-596` - Where `override_log_retention_seconds` fault injection is consumed

  **Acceptance Criteria**:
  - [ ] Build succeeds with full flags:
    ```bash
    cmake -DWITH_FAULT_INJECT=ON -DWITH_LOG_SERVICE=ON -DOPEN_LOG_SERVICE=OFF -DWITH_DATA_STORE=ELOQDSS_ROCKSDB_CLOUD_S3 ..
    make -j$(nproc)
    ```
  - [ ] Redis CLI command `fault_inject override_log_retention_seconds 0 retention_seconds=10` executes without error
  - [ ] Log output shows: "FaultInject name=override_log_retention_seconds" (from txservice)
  - [ ] When log purge runs, log shows: "Fault inject: overriding log retention with 10 seconds"

  **Commit**: NO (verification only)

---

## Commit Strategy

| After Task | Message | Files | Verification |
|------------|---------|-------|--------------|
| 2 | `feat(fault_inject): forward override_log_retention_seconds to txlog::FaultInject for embedded log_service` | fault_inject.h, fault_inject.cpp | make -j$(nproc) |

---

## Success Criteria

### Verification Commands
```bash
# Build with full embedded log service flags
cd build
cmake -DWITH_FAULT_INJECT=ON -DWITH_LOG_SERVICE=ON -DOPEN_LOG_SERVICE=OFF -DWITH_DATA_STORE=ELOQDSS_ROCKSDB_CLOUD_S3 ..
make -j$(nproc)

# Also verify builds without log service flags (no forwarding, should still compile)
cmake -DWITH_FAULT_INJECT=ON ..
make -j$(nproc)

# Test (manual)
# 1. Start eloqkv (built with full flags)
# 2. redis-cli fault_inject override_log_retention_seconds 0 retention_seconds=10
# 3. Check logs for forwarding confirmation
```

### Final Checklist
- [ ] `override_log_retention_seconds` fault injection forwards to txlog::FaultInject (when compile conditions met)
- [ ] Other fault injections do NOT forward (only `override_log_retention_seconds`)
- [ ] No forwarding when compile conditions are not met (e.g., OPEN_LOG_SERVICE mode)
- [ ] Existing txservice fault injection still works
- [ ] No header circular dependencies
- [ ] Code compiles with and without log service flags
