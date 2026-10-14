# Android GLES Probe

The first-party probe measures a deterministic full-screen GLES shader workload in an Android `GLSurfaceView`. It is intended to compare renderer configurations and expose frame-budget pressure; it is not a game, a guest-runtime benchmark, or evidence of FPS for a target application.

## Build

Requirements: Android SDK platform 36, build-tools 36.0.0, and a JDK. The build script uses Android Studio's bundled JBR when `JAVA_HOME` is invalid. It compiles/signs locally with `aapt2`, `javac`, `d8`, `zipalign`, and `apksigner`; no NDK, Gradle, network dependency, or third-party APK is required.

```powershell
./scripts/build-android-gles-probe.ps1
```

The APK is written to `out/android-gles-probe/fang-android-gles-probe.apk`.

## Run

Start the approved API 36 x86_64 AVD with the configuration being evaluated. For the current reference, use Android Emulator with WHPX and `-gpu host`. Keep resolution, refresh rate, emulator version, driver, AVD memory, power state, shader iterations, warm-up, and duration fixed between comparisons.

```powershell
./scripts/run-android-gles-probe.ps1 -Iterations 8 -DurationSeconds 30 -WarmupSeconds 5
```

The runner installs the locally built APK, runs the workload, waits for the current run ID's result log, and saves a JSON summary, raw CSV, and additive `.manifest.json` sidecar under `out/android-gles-probe/runs/`. The summary JSON and CSV schemas are unchanged. Existing runs remain valid without sidecars; no migration or rewrite is performed.

`iterations` is clamped to 1–64, duration to 5–120 seconds, and warm-up to 1–30 seconds. Keep the device foregrounded until the `RESULT` line appears.

## Interpretation

The probe records the GL renderer/version, refresh rate, frame count, p50/p95/p99 synchronous draw-completion time, and the share of draws exceeding one refresh interval. The measured draw time brackets GLES command submission through `glFinish`; it includes driver/host synchronization and is not a pure GPU timer query. It does not measure input latency, app startup, audio, guest boot, or real-game behavior.

The sidecar uses schema version 1. Every environment value is represented as `{status, source, value}`; `status` is `collected` or `unavailable`, and unavailable values also include a reason. It records Windows build/architecture, CPU/RAM, GPU names/driver versions, host displays, active Windows power scheme, emulator version, WHPX capability check, requested acceleration/GPU modes, guest identity/display/renderer, allowlisted AVD `config.ini` settings, installed system image package/revision/API/ABI/tag from `source.properties`, APK hash, Git revision/dirty flag, run ID/timestamp, workload settings, and artifact names. The active hypervisor accelerator and effective Windows power-mode overlay are marked unavailable because the runner cannot independently verify them. Requested command-line modes and the emulator acceleration check are not proof that the running guest used that backend. Results are correlated by a unique run ID, without clearing global logcat.

Collection is timed separately and completes before the Activity is launched. In the 2026-10-14 validation runs, metadata collection took 2.093 seconds and 1.893 seconds before launch; each Activity still reported its configured 5.0-second measurement interval and 300 draw samples. A later image-identity run also completed its configured 5.0-second interval. This demonstrates these queries are outside the measured GLES interval for those runs; it does not establish that collection has no effect on subsequent thermal/background state.

The host-GPU reference previously reported the NVIDIA RTX 3050 through Android Emulator. That does not establish QEMU rendering, sustained GPU utilization, thermal performance, or application compatibility. Do not use this microbenchmark alone to select a shipping backend or make gaming-FPS claims.

The schema-v1 validator requires `run_started_utc` to identify UTC and checks a collected guest refresh rate against the summary. New runs must pass strict provenance validation, including OS architecture, physical memory, AVD settings, system-image identity, and guest refresh rate. For compatibility, the validator still accepts historical schema-v1 sidecars that omit those additive fields; those older records are not rewritten and do not count as complete Phase 9 provenance. The summary JSON and CSV schemas are unchanged, and existing results without sidecars remain usable.

## Initial Reference Result (2026-10-14)

Host: Windows 11 build 26300, Android Emulator 37.2.12 with WHPX, API 36 x86_64, 1080x2400 at 60 Hz, NVIDIA RTX 3050 driver 610.60, guest GLES 3.1.

| Shader iterations | Duration | Frames | Draw p50 | Draw p95 | Draw p99 | Over 60 Hz budget |
|---:|---:|---:|---:|---:|---:|---:|
| 2 | 10 s | 600 | 1.511 ms | 2.739 ms | 3.071 ms | 0.000% |
| 8 | 10 s | 600 | 3.043 ms | 3.951 ms | 4.645 ms | 0.000% |
| 32 | 10 s | 600 | 3.157 ms | 4.468 ms | 4.610 ms | 0.000% |
| 64 | 10 s | 600 | 3.629 ms | 5.248 ms | 6.053 ms | 0.000% |
| 64 | 30 s | 1800 | 3.900 ms | 5.385 ms | 6.349 ms | 0.000% |

This shows no 60 Hz draw-completion bottleneck for this synthetic shader on the Android Emulator reference path. The probe calls `glFinish` each draw, so its timings include synchronization and are not comparable to asynchronous game rendering. No game, thermal telemetry, QEMU renderer, or host-GPU utilization metric was tested. Raw output is in `out/android-gles-probe/runs/`, which is local build/measurement output rather than a committed product benchmark.

For the full evidence-transfer rules, Phase 9 real-workload gate, and limitations of using this synthetic probe, see [Performance Evidence and Phase 9 Gate](performance-validation.md). The current per-run JSON/CSV does not embed every host/emulator field; use the reference environment recorded above only for those historical runs, and do not treat it as a complete Phase 9 manifest.