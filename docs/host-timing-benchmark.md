# Host Timing Benchmark

## Measurement model

The benchmark measures Windows host timer scheduling, not Android or game performance.

- The monotonic clock is `QueryPerformanceCounter`; its frequency is included in the JSON output.
- Each trial schedules one-shot waitable-timer deadlines at `trial_start + n * interval`. A high-resolution waitable timer is requested; a standard waitable timer is used if the high-resolution form is unavailable. Fang does not change global timer resolution or thread priority.
- `interval_ns` is the elapsed QPC time from the previous observed wake to the current observed wake. For the first sample, the previous point is the trial start.
- `wake_lateness_ns` is the observed QPC time minus that sample's scheduled deadline. Negative values are retained if a wake is observed early.
- The configured `duration_ms` is the deadline window. Only deadlines at or before its end are sampled, so the sample count per trial is `floor(duration_ms * 1000 / interval_us)`.
- `last_sample_elapsed_ns` reports elapsed QPC time at the last recorded wake, not the duration of timer setup or JSON output.

Timer setup, QPC calls, waitable-timer calls, scheduler activity, system load, power state, and thermal behavior can affect results. No measurement-overhead correction is applied. This benchmark does not measure timer resolution, timer interrupt frequency, actual CPU execution latency, GPU throughput, game FPS, or input-to-photon latency.

## Statistics and output

The JSON includes configuration, UTC timestamp, host/OS/CPU/display-device-name metadata where Windows exposes it, compiler/build type, timer backend, each raw sample, per-trial summaries, and combined summaries. Enumerated display-device names do not prove that an adapter rendered a workload; GPU utilization and driver-version telemetry are not collected.

Windows version metadata is recorded as the registry's major.minor.build values and display-version label. Windows 10 and 11 share the `10.0` version prefix; use the build number and host records to distinguish releases rather than relying on the legacy registry `ProductName` string.

The output omits the Windows computer name, but still contains host hardware details, OS version, and a timestamp. Review a result file before sharing it.

Mean is the arithmetic mean. Median averages the two middle values for even sample counts. P50, P95, and P99 use the nearest-rank method: sort ascending and select one-indexed rank `ceil(p * N)`. The JSON records this method. Wake-lateness statistics are signed nanoseconds; interval statistics are nanoseconds.

Run multiple trials and retain the raw JSON. Comparisons should use the same host, power mode, background conditions, duration, interval, and build configuration. This benchmark does not establish an Android gaming performance baseline.

## Example

```powershell
./scripts/benchmark.ps1 -DurationMs 1000 -IntervalUs 1000 -Runs 5
```

The script writes results to `out/benchmarks/host-timing-<timestamp>.json` unless `-OutputFile` is provided. Invalid options return a nonzero process status and a diagnostic.