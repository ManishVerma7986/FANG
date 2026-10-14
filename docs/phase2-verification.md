# Phase 2 Verification Record

**Date:** 2026-10-14  
**Scope:** Fang Core services only; no runtime or guest implementation

## Implementation

- Added the dependency-free `fang_core` static library.
- Added strict schema-version-1 configuration parsing and defaults.
- Added typed `CoreError` and `Result<T>` / `Result<void>` error propagation.
- Added synchronized JSON-line logging with severity filtering, escaping, and sink-failure reporting.
- Added owned service registration, lifecycle state reporting, serialized start/stop, reverse-order shutdown, cleanup of partially started services, and startup/shutdown error propagation.
- Added capability reporting that distinguishes unavailable, detected, initialized, and verified; reports cannot skip upward evidence stages.

## Commands and results

Using the installed Visual Studio 2026 CMake/CTest binaries:

```powershell
cmake --build --preset fang-debug --parallel
ctest --preset fang-debug
cmake --build --preset fang-release --parallel
ctest --preset fang-release
```

Both Debug and Release builds succeeded. Each CTest run passed 3/3 registered tests: host timing unit tests, core services tests, and invalid host-timing argument rejection. Core tests cover valid/invalid configuration, unsupported and duplicate keys, staged capability reports, structured-log filtering/escaping/concurrent writes/sink failures, lifecycle state/ownership/reverse shutdown, shutdown failure propagation, failing-service cleanup, exception translation, and concurrent start/stop.

## Acceptance status

- Configuration defaults, validation, and versioned parsing: **VERIFIED** by core tests.
- Structured logging and stable error-code propagation: **VERIFIED** by core tests.
- Lifecycle transitions, ownership, deterministic reverse shutdown, rollback, and concurrent start/stop: **VERIFIED** by core tests.
- Capability evidence stages and skip prevention: **VERIFIED** by core tests.
- Debug and Release builds/tests: **VERIFIED** on the inspected host.
- Persistent configuration storage/migration, concrete rotating sinks, application lifecycle wiring, and guest integration: **NOT STARTED**.
- Clean-checkout reproduction remains **NOT VERIFIED**; validation used the current working tree.

## Security, licensing, and limitations

No third-party dependencies were introduced. Configuration parsing operates on a caller-provided stream; this phase does not select a config path or persist settings. Logger message/context strings must be valid UTF-8, the stream is borrowed and must outlive the logger, and callers select the sink explicitly. `ServiceHost` destruction is best-effort; applications must call `stop()` to observe shutdown errors. Service callbacks must not recursively call mutating methods on their owning host.

## Next gate

Phase 3 may build only a minimal Windows shell if it reflects actual core/runtime state. Before any substantial execution, guest boot, or graphics work, the project must complete Phase 4's explicit Android guest, execution strategy, graphics integration, security boundary, compatibility, and licensing decision.