# Windows Development Setup

## Verified host

The initial environment inspection on 2026-10-14 found:

- Windows 11 Home Single Language, x64, build 26300.
- Intel Core i5-11260H, 6 cores / 12 logical processors.
- 16 GiB physical memory.
- Intel UHD Graphics and NVIDIA GeForce RTX 3050 Laptop GPU.
- Visual Studio Build Tools 2026, MSVC toolset 14.50.35717 (compiler 19.50.35729).
- CMake 4.2.3 bundled with Visual Studio and its Ninja executable.
- Git 2.52.0.
- Windows 11 SDK 10.0.26100.0, installed and used for the verified builds.

The Windows SDK provides the headers and system libraries required by MSVC. The old MinGW GCC 6.3 found on PATH is not used.

## Build and test

The checked-in CMake preset selects the Visual Studio 18 2026 x64 generator. The PowerShell scripts use CMake from PATH when present, otherwise they locate the CMake bundled with the latest Visual Studio C++ Build Tools instance.

If PowerShell reports that script execution is disabled, enable locally-created scripts for the current terminal only. This does not change persistent user or machine policy:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy RemoteSigned
```

```powershell
./scripts/build.ps1 -Configuration Debug
./scripts/test.ps1 -Configuration Debug
./scripts/build.ps1 -Configuration Release
./scripts/test.ps1 -Configuration Release
```

To use a different Visual Studio generator, configure with CMake directly and select a generator installed on that machine. The CMake project itself requires CMake 3.24 or newer; the checked-in Visual Studio 2026 preset requires a CMake release that recognizes that generator.

## Scope

This foundation builds only the host timing command-line benchmark and its unit/CLI tests. It does not implement an Android guest, emulator runtime, graphics path, UI, audio, or input delivery. No third-party libraries have been introduced; the unit tests use a small in-repository harness because their scope does not justify a dependency.