#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fang::guest {

enum class GuestTargetAbi : uint8_t {
    Unknown,
    X86_64,
    Arm64_v8a,
    X86,
    Armeabi_v7a,
    HostNative
};

constexpr std::string_view guest_target_abi_name(GuestTargetAbi abi) noexcept {
    switch (abi) {
    case GuestTargetAbi::X86_64:      return "x86_64";
    case GuestTargetAbi::Arm64_v8a:   return "arm64-v8a";
    case GuestTargetAbi::X86:         return "x86";
    case GuestTargetAbi::Armeabi_v7a: return "armeabi-v7a";
    case GuestTargetAbi::HostNative:  return "host-native";
    default:                          return "unknown";
    }
}

GuestTargetAbi parse_guest_target_abi(std::string_view name) noexcept;

enum class ToolchainStatus : uint8_t {
    ToolchainAvailable,
    MissingNdk,
    MissingClang,
    UnsupportedHost
};

constexpr std::string_view toolchain_status_name(ToolchainStatus status) noexcept {
    switch (status) {
    case ToolchainStatus::ToolchainAvailable: return "ToolchainAvailable";
    case ToolchainStatus::MissingNdk:          return "MissingNdk";
    case ToolchainStatus::MissingClang:        return "MissingClang";
    case ToolchainStatus::UnsupportedHost:     return "UnsupportedHost";
    default:                                  return "Unknown";
    }
}

struct SourceEvidence {
    enum class Status : uint8_t {
        Unavailable,
        Collected
    };
    Status status{Status::Unavailable};
    std::string algorithm;              // e.g., "sha256"
    std::string fingerprint;            // hex digest
    std::string scope;                  // e.g., "build-inputs-v1"
    std::string encoding;               // e.g., "fang-cim-c14n-v1"
    std::string captured_at_utc;        // ISO 8601 UTC timestamp
    std::vector<std::string> unavailable_reasons;

    bool is_collected() const noexcept { return status == Status::Collected; }
};

struct GuestArtifactManifest {
    std::string artifact_name;
    std::string artifact_format;      // "ELF", "PE-COFF", "Unavailable"
    std::string target_os;            // "android", "linux", "windows"
    std::string target_abi;           // "x86_64", "arm64-v8a", etc.
    uint16_t protocol_version{1};
    std::string compiler_id;
    std::string build_configuration;  // "Debug", "Release"
    std::string backend_capability;   // "RecordingStub", "UinputKernel", etc.
    bool privileged_backend_enabled{false};
    bool is_cross_compiled{false};
    bool is_android_elf_binary{false};
    ToolchainStatus toolchain_status{ToolchainStatus::ToolchainAvailable};
    std::vector<std::string> missing_prerequisites;
    std::string build_rule_version{"stage9-v1"};
    std::string manifest_schema_version{"stage9-v1"};
    std::string source_revision;
    std::string source_tree_state;          // "clean", "dirty", "unavailable"
    std::vector<std::string> source_tree_dirty_files;
    std::vector<std::string> provenance_collected;
    std::vector<std::string> provenance_unavailable;

    // Source evidence fingerprint
    SourceEvidence source_evidence;

    // Extended pipeline & artifact validation metadata
    std::string artifact_path;
    uint32_t android_api_level{26};
    std::string ndk_version;
    uint64_t artifact_size_bytes{0};
    std::string artifact_sha256;
    std::string build_result{"unbuilt"};
    std::string elf_machine;
    uint8_t elf_class{0};
    uint8_t elf_data{0};

    std::string to_json() const;
    static bool from_json(std::string_view json, GuestArtifactManifest& out_manifest, std::string* error = nullptr);
    bool validate(std::string* error = nullptr) const;
};

// Toolchain inspection utility
class GuestToolchainDetector {
public:
    struct EnvironmentReport {
        bool ndk_found{false};
        std::string ndk_path;
        std::string ndk_version;
        uint32_t ndk_major_version{0};
        bool version_supported{false};
        bool clang_found{false};
        std::string clang_path;
        std::vector<std::string> missing_tools;
        std::string diagnostic;
    };

    static EnvironmentReport inspect_environment(uint32_t min_version = 25);
    static EnvironmentReport inspect_ndk_path(const std::string& custom_ndk_path, uint32_t min_version = 25);
    static GuestArtifactManifest generate_manifest_for_target(
        GuestTargetAbi abi,
        const std::string& build_config,
        const std::string& backend_capability,
        bool privileged_backend);
};

} // namespace fang::guest
