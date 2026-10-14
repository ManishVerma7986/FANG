# Phase 0/1 Verification Record

**Date:** 2026-10-14  
**Scope:** Windows environment foundation and host timing benchmark only

## Environment

- Windows 11 Home Single Language, x64, build 26300 (display version 26H2).
- Intel Core i5-11260H, 6 cores / 12 logical processors; 16 GiB installed RAM.
- Intel UHD Graphics and NVIDIA GeForce RTX 3050 Laptop GPU.
- Visual Studio Build Tools 2026 18.5.11716.220; MSVC 19.50.35729.0, toolset 14.50.35717.
- CMake 4.2.3-msvc3 and bundled Ninja; Windows 11 SDK 10.0.26100.0.
- Git 2.52.0.windows.1. MinGW GCC 6.3.0 is present on PATH but is not used.
- The Windows SDK was installed with `winget install --id Microsoft.WindowsSDK.10.0.26100 --exact --source winget --silent --accept-package-agreements --accept-source-agreements` after an initial configure proved the SDK libraries were missing.

The first configure attempt failed because `WindowsSDKDir` was unset and MSVC could not link `kernel32.lib` (LNK1181). The standalone SDK installation resolved that blocker.

## Commands and results

The following commands completed successfully:

The CMake and CTest commands below were invoked through `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` and the adjacent `ctest.exe`, since neither executable was on PATH.

```powershell
cmake --preset fang-windows
cmake --build --preset fang-debug --parallel
ctest --preset fang-debug
cmake --build --preset fang-release --parallel
ctest --preset fang-release
./scripts/test.ps1 -Configuration Debug
./scripts/test.ps1 -Configuration Release
./scripts/benchmark.ps1 -DurationMs 1000 -IntervalUs 1000 -Runs 3
./scripts/benchmark.ps1 -DurationMs 1000 -IntervalUs 1000 -Runs 3
```

The PowerShell script runs used a process-scoped execution-policy override and restored it afterward; no persistent policy was changed. Debug and Release each passed both CTest tests (2/2 per configuration). The tests cover deterministic summary/argument validation and the executable's rejection of a zero-duration request. Both generated benchmark JSON files parsed successfully, reported Release build metadata, and had three trials of 1,000 raw samples each.

## Benchmark observations

Configuration: Release; high-resolution waitable timer; QPC frequency recorded in each raw JSON; requested interval 1,000 microseconds; requested duration 1,000 milliseconds per trial; 3 trials per run; 2 runs. The host was not otherwise controlled or isolated, and no warm-up was applied.

Across the six trials:

- Per-trial interval median: 1.0277-1.0420 ms.
- Per-trial interval P95: 1.2394-1.3255 ms.
- Per-trial wake-lateness median: 320.3-367.95 microseconds.
- Per-trial wake-lateness P95: 597.3-666.6 microseconds.

These are host timer wake-up observations only. They do not establish timer resolution, CPU execution latency, Android performance, game FPS, GPU throughput/utilization, or input-to-photon latency. They are not an optimization comparison. Raw samples are retained locally at:

- `out/benchmarks/host-timing-20261014-154207.json`
- `out/benchmarks/host-timing-20261014-154219.json`

`out/` is ignored by Git, so these machine-specific measurements are not part of source control by default.

## Acceptance status

- Debug and Release configure/build on the inspected host: **VERIFIED**.
- Unit/statistics and invalid-argument checks: **VERIFIED**, 2/2 CTest tests per configuration.
- Valid benchmark execution, JSON parsing, raw samples, and repeated runs: **VERIFIED** on this host.
- Setup, measurement semantics, and limitations documented: **VERIFIED**.
- Clean-checkout reproduction: **NOT VERIFIED**; the commands were run in the current working tree, not a fresh clone.
- Android guest, graphics, audio, input, game FPS, and GPU telemetry: **NOT STARTED** in this milestone.

## Next milestone

Phase 2 is core services. Before committing to a guest/runtime implementation, Phase 4 still requires an explicit guest OS and execution/graphics architecture decision with licensing, boot, security, and compatibility evidence.