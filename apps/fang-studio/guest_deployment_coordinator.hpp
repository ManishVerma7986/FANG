#pragma once

#include "guest_manifest.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fang::studio {

enum class GuestDeploymentPhase : uint8_t {
    Unconfigured,
    BlockedMissingToolchain,
    ArtifactMissing,
    ArtifactArchitectureMismatch,
    ArtifactCorrupt,
    ArtifactManifestMismatch,
    ArtifactReady,
    DeploymentBlockedOffline,
    Deployed,
    DeploymentFailed
};

enum class ProvenanceAssessment : uint8_t {
    Unassessed,
    QualifiedCurrent,
    QualifiedDirty,
    Stale,
    Unavailable,
    LegacyUnverified
};

constexpr std::string_view provenance_assessment_name(ProvenanceAssessment assessment) noexcept {
    switch (assessment) {
    case ProvenanceAssessment::Unassessed:         return "Unassessed";
    case ProvenanceAssessment::QualifiedCurrent:   return "QualifiedCurrent";
    case ProvenanceAssessment::QualifiedDirty:     return "QualifiedDirty";
    case ProvenanceAssessment::Stale:              return "Stale";
    case ProvenanceAssessment::Unavailable:        return "Unavailable";
    case ProvenanceAssessment::LegacyUnverified:   return "LegacyUnverified";
    default:                                       return "Unknown";
    }
}

constexpr std::string_view guest_deployment_phase_name(GuestDeploymentPhase phase) noexcept {
    switch (phase) {
    case GuestDeploymentPhase::Unconfigured:                  return "Unconfigured";
    case GuestDeploymentPhase::BlockedMissingToolchain:       return "BlockedMissingToolchain";
    case GuestDeploymentPhase::ArtifactMissing:                return "ArtifactMissing";
    case GuestDeploymentPhase::ArtifactArchitectureMismatch:   return "ArtifactArchitectureMismatch";
    case GuestDeploymentPhase::ArtifactCorrupt:               return "ArtifactCorrupt";
    case GuestDeploymentPhase::ArtifactManifestMismatch:      return "ArtifactManifestMismatch";
    case GuestDeploymentPhase::ArtifactReady:                 return "ArtifactReady";
    case GuestDeploymentPhase::DeploymentBlockedOffline:      return "DeploymentBlockedOffline";
    case GuestDeploymentPhase::Deployed:                      return "Deployed";
    case GuestDeploymentPhase::DeploymentFailed:              return "DeploymentFailed";
    default:                                                  return "Unknown";
    }
}

struct GuestDeploymentTarget {
    fang::guest::GuestTargetAbi abi{fang::guest::GuestTargetAbi::X86_64};
    std::string staging_dir;
    std::string guest_install_path{"/data/local/tmp/fang-input-daemon"};
    std::string build_config{"Release"};
    uint32_t android_api_level{26};
    bool require_privileged_backend{false};
};

struct GuestDeploymentReport {
    GuestDeploymentPhase phase{GuestDeploymentPhase::Unconfigured};
    ProvenanceAssessment provenance_assessment{ProvenanceAssessment::Unassessed};
    std::string diagnostic;
    std::string resolved_artifact_path;
    uint64_t artifact_size_bytes{0};
    std::string artifact_sha256;
    fang::guest::GuestArtifactManifest manifest;
    bool is_ready_for_push{false};
    bool is_deployed{false};
};

class IGuestDeploymentCoordinator {
public:
    virtual ~IGuestDeploymentCoordinator() = default;

    virtual GuestDeploymentReport assess_deployment_readiness(const GuestDeploymentTarget& target) = 0;
    virtual bool deploy_to_guest(
        const std::string& serial_or_avd,
        const GuestDeploymentTarget& target,
        GuestDeploymentReport& out_report) = 0;
    virtual void teardown_guest_deployment(
        const std::string& serial_or_avd,
        GuestDeploymentReport& report) = 0;
    virtual std::string_view coordinator_name() const noexcept = 0;
};

// Deterministic mock/stub coordinator for unit testing and offline test scenarios
class MockGuestDeploymentCoordinator final : public IGuestDeploymentCoordinator {
public:
    explicit MockGuestDeploymentCoordinator(
        GuestDeploymentPhase initial_phase = GuestDeploymentPhase::ArtifactReady,
        std::string initial_diag = "Mock deployment ready");

    GuestDeploymentReport assess_deployment_readiness(const GuestDeploymentTarget& target) override;
    bool deploy_to_guest(
        const std::string& serial_or_avd,
        const GuestDeploymentTarget& target,
        GuestDeploymentReport& out_report) override;
    void teardown_guest_deployment(
        const std::string& serial_or_avd,
        GuestDeploymentReport& report) override;
    std::string_view coordinator_name() const noexcept override { return "mock-deployment"; }

    void set_phase(GuestDeploymentPhase phase) noexcept { phase_ = phase; }
    void set_diagnostic(std::string diag) { diagnostic_ = std::move(diag); }
    void set_simulate_push_success(bool ok) noexcept { push_success_ = ok; }

private:
    GuestDeploymentPhase phase_{GuestDeploymentPhase::ArtifactReady};
    std::string diagnostic_{"Mock deployment ready"};
    bool push_success_{true};
};

struct ElfHeaderInspection {
    bool is_valid_elf{false};
    std::string format;               // "ELF", "PE-COFF", "Truncated", "Unknown"
    uint8_t elf_class{0};             // 1 = 32-bit (ELFCLASS32), 2 = 64-bit (ELFCLASS64)
    uint8_t data_encoding{0};         // 1 = Little-endian (LSB), 2 = Big-endian (MSB)
    uint8_t os_abi{0};                // 0 = System V / Android, 3 = Linux
    uint16_t object_type{0};          // 2 = ET_EXEC, 3 = ET_DYN (PIE executable)
    uint16_t machine_arch{0};         // 0x3E (62) = EM_X86_64, 0xB7 (183) = EM_AARCH64, 0x03 (3) = EM_386, 0x28 (40) = EM_ARM
    uint32_t version{0};
    uint64_t entry_point{0};
    uint16_t header_size{0};
    uint16_t program_header_count{0};
    std::string error_message;
};

// Production guest deployment coordinator: checks toolchains, inspects staging directories,
// validates binary ELF vs PE-COFF headers, calculates SHA-256, and safely enforces offline boundaries.
class GuestDeploymentCoordinator final : public IGuestDeploymentCoordinator {
public:
    GuestDeploymentCoordinator() = default;

    GuestDeploymentReport assess_deployment_readiness(const GuestDeploymentTarget& target) override;
    bool deploy_to_guest(
        const std::string& serial_or_avd,
        const GuestDeploymentTarget& target,
        GuestDeploymentReport& out_report) override;
    void teardown_guest_deployment(
        const std::string& serial_or_avd,
        GuestDeploymentReport& report) override;
    std::string_view coordinator_name() const noexcept override { return "guest-deployment-coordinator"; }

    // Helpers
    static bool inspect_binary_format(
        const std::string& file_path,
        std::string& out_format,
        bool& out_is_elf,
        std::string* error = nullptr);

    static bool inspect_elf_header(
        const std::string& file_path,
        ElfHeaderInspection& out_inspection,
        std::string* error = nullptr);

    static bool validate_elf_for_abi(
        const std::string& file_path,
        fang::guest::GuestTargetAbi expected_abi,
        ElfHeaderInspection& out_inspection,
        std::string* error = nullptr);

    static bool validate_artifact_manifest(
        const std::string& manifest_path,
        const std::string& artifact_path,
        fang::guest::GuestTargetAbi expected_abi,
        fang::guest::GuestArtifactManifest& out_manifest,
        std::string* error = nullptr,
        uint32_t expected_api_level = 26);
    static std::string calculate_file_sha256(const std::string& file_path);

    // Provenance helpers
    static bool collect_current_source_evidence(
        const std::string& repo_root,
        const std::string& abi,
        uint32_t api_level,
        const std::string& configuration,
        const std::string& target_triple,
        const std::string& ndk_version,
        const std::vector<std::string>& compile_args,
        fang::guest::SourceEvidence& out_evidence,
        std::string* error = nullptr);
    static ProvenanceAssessment assess_provenance(
        const fang::guest::GuestArtifactManifest& manifest,
        const fang::guest::SourceEvidence& current_evidence,
        std::string* diagnostic = nullptr);
};

} // namespace fang::studio
