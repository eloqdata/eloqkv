# Learnings - Fault Inject Forwarding

## Conventions & Patterns

(To be populated as work progresses)

## Task 1 & 2: Fault Inject Forwarding Implementation

### Implementation Pattern
- **Header Movement**: Moving inline implementation to .cpp is straightforward - replace lines 197-219 with single declaration `void InjectFault(std::string fault_name, std::string paras);`
- **Preprocessor Guards**: Use triple condition guard: `defined(WITH_LOG_SERVICE) && !defined(OPEN_LOG_SERVICE) && defined(LOG_STATE_TYPE_RKDB_S3)`
- **Cross-Namespace Forwarding**: Include `fault_inject.h` (from eloq_log_service) inside preprocessor guard to avoid circular dependencies. LSP will report error in conditional code, but it compiles fine when conditions are met.

### Code Structure
- **Header file**: Lines 197 - replaced inline implementation with declaration only
- **CPP file includes**: Lines 24-26 - conditional include added after main include
- **CPP file implementation**: Lines 253-283 - InjectFault method with conditional forwarding before "remove" case handling

### Key Patterns
1. **Conditional Forwarding**: Check fault_name FIRST before forwarding, so only specific fault names (override_log_retention_seconds) forward
2. **Removal Handling**: Forward removal (paras="remove") to txlog as well - happens AFTER forwarding check but BEFORE lock guard
3. **No Forwarding for Other Faults**: Only override_log_retention_seconds forwards; "at_once" and other faults only affect local map
4. **Lock Ordering**: Forwarding happens OUTSIDE the lock_guard - good practice to avoid deadlock when crossing module boundaries

### Compilation Notes
- Code compiles without log service flags (conditional include becomes no-op)
- Code compiles with full flags (conditional include pulls txlog::FaultInject)
- LSP reports error on txlog::FaultInject when include is disabled - expected and harmless
- Python structure verification confirms all required pieces are in place

### Cross-Module Integration
- This pattern enables single-process integration between txservice and eloq_log_service fault injection
- No header circular dependencies because include is conditionally guarded
- Forwarding is opt-in via compile flags, allowing same codebase to work standalone

## Build Error Fix: Ambiguous Include Path

### Problem Encountered
- Build error: `'txlog::FaultInject' has not been declared` at fault_inject.cpp:260
- Root cause: Multiple files named `fault_inject.h` in different directories
- Include path search order issue: `tx_service/include/fault/` was searched before `eloq_log_service/include/`

### Solution Implemented
- Changed include from ambiguous: `#include "fault_inject.h"`
- To explicit relative path: `#include "../../../eloq_log_service/include/fault_inject.h"`
- Path calculation: From `tx_service/src/fault/` → up 3 levels to `data_substrate/` → down to `eloq_log_service/include/`

### Verification
- Compilation of fault_inject.cpp.o succeeded with explicit path
- No namespace resolution errors after fix
- Pattern applicable to other multi-module C++ projects with duplicate filenames

### Key Lesson
When multiple files share the same name across different module directories, relying on include_directories() ordering is fragile. Use explicit relative or absolute paths to disambiguate, especially when:
1. Files are in different namespaces (txservice vs txlog)
2. Include search paths overlap
3. Files are in subdirectories with differing names
