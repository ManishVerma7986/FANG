# Phase 3 Development Shell

## Current slice

`fang-studio` is a small native Win32 GUI built with the installed Windows SDK and MSVC. It is linked to `fang_core` and operates a real `ServiceHost` containing one development-shell service. Start/stop buttons invoke that host; the displayed lifecycle state and JSON-line log view come from the core objects. Capability rows come from `CapabilityRegistry`.

The shell reports Android guest boot and guest graphics integration as unavailable because no guest/runtime backend is implemented. It does not offer APK selection, guest launch, resource controls, GPU claims, or fake runtime buttons.

## Build and launch

```powershell
./scripts/build.ps1 -Configuration Debug
./out/build/Debug/fang-studio.exe
```

Release builds are at `out/build/Release/fang-studio.exe`.

## Status and limits

This is an early development shell, not the completed Studio product or a functional Android gaming player. It has no persistent settings, runtime process management, crash reporting, log export, input controls, guest display, or actual host GPU discovery. Logs are retained in memory only while the shell is open.

The shell is compiled in Debug/Release and core lifecycle behavior is covered by CTest. The `studio-gui-smoke` test launches the real shell, verifies its initial runtime/capability status, invokes the Start/Stop button notifications, checks lifecycle state and log updates, and confirms clean exit. The next substantial implementation depends on the Phase 4 guest/execution/graphics architecture and licensing decision.