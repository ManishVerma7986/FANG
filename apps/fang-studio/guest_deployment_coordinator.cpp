#include "guest_deployment_coordinator.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <array>
#include <memory>

namespace fang::studio {

namespace {

// Portable FIPS-180-2 SHA-256 implementation
namespace sha256 {
    inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    inline uint32_t choose(uint32_t e, uint32_t f, uint32_t g) { return (e & f) ^ (~e & g); }
    inline uint32_t majority(uint32_t a, uint32_t b, uint32_t c) { return (a & b) ^ (a & c) ^ (b & c); }
    inline uint32_t sig0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    inline uint32_t sig1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    inline uint32_t theta0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    inline uint32_t theta1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    struct Context {
        uint32_t state[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        uint64_t count{0};
        uint8_t buffer[64]{0};

        void transform(const uint8_t data[64]) {
            uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
            uint32_t m[64];
            for (int i = 0; i < 16; ++i) {
                m[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
                       (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
                       (static_cast<uint32_t>(data[i * 4 + 3]));
            }
            for (int i = 16; i < 64; ++i) {
                m[i] = theta1(m[i - 2]) + m[i - 7] + theta0(m[i - 15]) + m[i - 16];
            }
            for (int i = 0; i < 64; ++i) {
                uint32_t t1 = h + sig1(e) + choose(e, f, g) + K[i] + m[i];
                uint32_t t2 = sig0(a) + majority(a, b, c);
                h = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
            }
            state[0] += a; state[1] += b; state[2] += c; state[3] += d;
            state[4] += e; state[5] += f; state[6] += g; state[7] += h;
        }

        void update(const uint8_t* data, size_t len) {
            for (size_t i = 0; i < len; ++i) {
                buffer[count % 64] = data[i];
                count++;
                if (count % 64 == 0) {
                    transform(buffer);
                }
            }
        }

        std::string finalize() {
            uint64_t bit_count = count * 8;
            uint8_t pad = 0x80;
            update(&pad, 1);
            while (count % 64 != 56) {
                uint8_t zero = 0;
                update(&zero, 1);
            }
            uint8_t len_bytes[8];
            for (int i = 0; i < 8; ++i) {
                len_bytes[i] = static_cast<uint8_t>((bit_count >> ((7 - i) * 8)) & 0xFF);
            }
            update(len_bytes, 8);
            std::ostringstream oss;
            for (int i = 0; i < 8; ++i) {
                oss << std::hex << std::setw(8) << std::setfill('0') << state[i];
            }
            return oss.str();
        }
    };
} // namespace sha256

// ----------------------------------------------------------------------------
// Helper functions for big-endian encoding (fang-cim-c14n-v1)
// ----------------------------------------------------------------------------

namespace {

inline void write_u32be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void write_u64be(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void write_u8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

inline void write_bytes(std::vector<uint8_t>& out, const uint8_t* data, size_t len) {
    out.insert(out.end(), data, data + len);
}

inline void write_string_utf8(std::vector<uint8_t>& out, const std::string& str) {
    write_u32be(out, static_cast<uint32_t>(str.size()));
    write_bytes(out, reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

inline void write_hex_digest(std::vector<uint8_t>& out, const std::string& hex) {
    // hex is 64-char lowercase hex string, convert to 32 raw bytes
    for (size_t i = 0; i < 32 && i * 2 < hex.size(); ++i) {
        std::string byte_str = hex.substr(i * 2, 2);
        out.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }
}

// Role IDs (must match PowerShell implementation)
enum class CanonicalRole : uint8_t {
    Source = 1,
    Header = 2,
    BuildScript = 3,
    Configuration = 4,
    TargetConfig = 5,
    CompileArg = 6,
    GitRevision = 7
};

// Kind IDs
enum class CanonicalKind : uint8_t {
    File = 0,
    Virtual = 1
};

} // anonymous namespace

// ----------------------------------------------------------------------------
// Canonical Input Manifest (fang-cim-c14n-v1) implementation
// ----------------------------------------------------------------------------

struct CanonicalEntry {
    std::string path;
    CanonicalRole role;
    CanonicalKind kind;
    uint64_t size;
    std::string hash;  // 64-char lowercase hex
};

static std::string hash_file_sha256(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return {};
    sha256::Context ctx;
    uint8_t buffer[8192];
    while (file.read(reinterpret_cast<char*>(buffer), sizeof(buffer))) {
        ctx.update(buffer, static_cast<size_t>(file.gcount()));
    }
    if (file.gcount() > 0) {
        ctx.update(buffer, static_cast<size_t>(file.gcount()));
    }
    return ctx.finalize();
}

static void add_canonical_entry(std::vector<CanonicalEntry>& entries,
                                const std::string& repo_root,
                                const std::string& rel_path,
                                CanonicalRole role,
                                CanonicalKind kind = CanonicalKind::File,
                                std::vector<std::string>* errors = nullptr) {
    std::string full_path = repo_root + "/" + rel_path;
    std::ifstream file(full_path, std::ios::binary);
    if (!file) {
        if (errors) errors->push_back("Build input not found: " + rel_path);
        return;
    }
    file.seekg(0, std::ios::end);
    uint64_t size = static_cast<uint64_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    if (!file) {
        if (errors) errors->push_back("Failed to read build input: " + rel_path);
        return;
    }
    sha256::Context ctx;
    ctx.update(buffer.data(), buffer.size());
    std::string hash = ctx.finalize();
    entries.push_back({rel_path, role, kind, size, hash});
}

static void add_virtual_entry(std::vector<CanonicalEntry>& entries,
                              const std::string& path,
                              CanonicalRole role,
                              const std::string& content) {
    sha256::Context ctx;
    ctx.update(reinterpret_cast<const uint8_t*>(content.data()), content.size());
    std::string hash = ctx.finalize();
    entries.push_back({path, role, CanonicalKind::Virtual, static_cast<uint64_t>(content.size()), hash});
}

static std::string build_canonical_fingerprint(const std::string& repo_root,
                                               const std::string& source_revision,
                                               const std::string& abi,
                                               uint32_t api_level,
                                               const std::string& configuration,
                                               const std::string& target_triple,
                                               const std::string& ndk_version,
                                               const std::vector<std::string>& compile_args,
                                               std::vector<std::string>* errors = nullptr) {
    std::vector<CanonicalEntry> entries;
    
    // 1. Compiled source files
    const std::vector<std::pair<std::string, CanonicalRole>> sources = {
        {"apps/fang-input-daemon/main.cpp", CanonicalRole::Source},
        {"apps/fang-input-daemon/guest_input_backend.cpp", CanonicalRole::Source},
        {"apps/fang-input-daemon/guest_input_daemon.cpp", CanonicalRole::Source},
        {"apps/fang-input-daemon/guest_manifest.cpp", CanonicalRole::Source},
        {"apps/fang-studio/input_ipc_protocol.cpp", CanonicalRole::Source},
        {"apps/fang-studio/input_transport.cpp", CanonicalRole::Source},
        {"apps/fang-studio/socket_input_transport.cpp", CanonicalRole::Source}
    };
    for (const auto& [path, role] : sources) {
        add_canonical_entry(entries, repo_root, path, role, CanonicalKind::File, errors);
    }
    
    // 2. Project headers
    const std::vector<std::string> header_dirs = {
        "apps/fang-input-daemon",
        "apps/fang-studio"
    };
    for (const auto& dir : header_dirs) {
        std::error_code ec;
        std::filesystem::path dir_path = repo_root + "/" + dir;
        if (std::filesystem::exists(dir_path, ec)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".hpp") {
                    std::string rel_path = std::filesystem::relative(entry.path(), repo_root).generic_string();
                    std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
                    add_canonical_entry(entries, repo_root, rel_path, CanonicalRole::Header, CanonicalKind::File, errors);
                }
            }
        }
    }
    
    // 3. Build scripts
    const std::vector<std::string> build_scripts = {
        "scripts/build-guest-daemon.ps1",
        "scripts/build-guest-daemon.sh",
        "scripts/build-guest-daemon.cmd"
    };
    for (const auto& script : build_scripts) {
        add_canonical_entry(entries, repo_root, script, CanonicalRole::BuildScript, CanonicalKind::File, errors);
    }
    
    // 4. CMake configuration
    add_canonical_entry(entries, repo_root, "CMakeLists.txt", CanonicalRole::Configuration, CanonicalKind::File, errors);
    
    // 5. Target configuration (virtual entries)
    add_virtual_entry(entries, "target/abi", CanonicalRole::TargetConfig, abi);
    add_virtual_entry(entries, "target/api_level", CanonicalRole::TargetConfig, std::to_string(api_level));
    add_virtual_entry(entries, "target/configuration", CanonicalRole::TargetConfig, configuration);
    add_virtual_entry(entries, "target/compiler_target", CanonicalRole::TargetConfig, target_triple);
    
    // 6. Compiler arguments (virtual entries, in exact order)
    for (size_t i = 0; i < compile_args.size(); ++i) {
        std::string arg_path = "compile_args/arg_" + std::to_string(i);
        // Pad with zeros to 3 digits
        if (i < 10) arg_path += "00";
        else if (i < 100) arg_path += "0";
        arg_path = "compile_args/arg_" + std::to_string(i);
        add_virtual_entry(entries, arg_path, CanonicalRole::CompileArg, compile_args[i]);
    }
    
    // 7. Git revision (virtual entry)
    if (!source_revision.empty()) {
        add_virtual_entry(entries, "git/revision", CanonicalRole::GitRevision, source_revision);
    }
    
    // Sort entries by UTF-8 path bytes
    std::sort(entries.begin(), entries.end(), [](const CanonicalEntry& a, const CanonicalEntry& b) {
        return a.path < b.path;
    });
    
    // Build canonical binary stream
    std::vector<uint8_t> canonical;
    
    // MAGIC: "FANGCIM" + NUL + format-version(1)
    canonical.insert(canonical.end(), {'F','A','N','G','C','I','M', 0x00, 0x01});
    
    // SCOPE: u32be length + UTF-8 bytes
    const std::string scope = "fang-guest-daemon-inputs-v1";
    write_u32be(canonical, static_cast<uint32_t>(scope.size()));
    write_bytes(canonical, reinterpret_cast<const uint8_t*>(scope.data()), scope.size());
    
    // TARGET component
    const std::string component = "fang-input-daemon";
    write_string_utf8(canonical, component);
    
    // TARGET abi
    write_string_utf8(canonical, abi);
    
    // TARGET api_level (u32be)
    write_u32be(canonical, api_level);
    
    // TARGET configuration
    write_string_utf8(canonical, configuration);
    
    // TOOLCHAIN: compiler identity
    write_string_utf8(canonical, "clang");
    
    // TOOLCHAIN: compiler version (NDK version)
    write_string_utf8(canonical, ndk_version);
    
    // TOOLCHAIN: target triple
    write_string_utf8(canonical, target_triple);
    
    // TOOLCHAIN: argument count (u32be)
    write_u32be(canonical, static_cast<uint32_t>(compile_args.size()));
    
    // TOOLCHAIN: each argument in exact order
    for (const auto& arg : compile_args) {
        write_string_utf8(canonical, arg);
    }
    
    // ENTRIES: entry count (u32be)
    write_u32be(canonical, static_cast<uint32_t>(entries.size()));
    
    // ENTRIES: each entry sorted by UTF-8 path bytes (already sorted)
    for (const auto& entry : entries) {
        // path: u32be length + UTF-8 bytes
        write_string_utf8(canonical, entry.path);
        
        // kind: u8
        write_u8(canonical, static_cast<uint8_t>(entry.kind));
        
        // role: u8
        write_u8(canonical, static_cast<uint8_t>(entry.role));
        
        // size_bytes: u64be
        write_u64be(canonical, entry.size);
        
        // sha256: 32 raw bytes
        write_hex_digest(canonical, entry.hash);
    }
    
    // Final fingerprint = SHA-256 of all preceding bytes
    sha256::Context ctx;
    ctx.update(canonical.data(), canonical.size());
    return ctx.finalize();
}

std::string get_expected_artifact_name(fang::guest::GuestTargetAbi abi) {
    if (abi == fang::guest::GuestTargetAbi::HostNative) {
#if defined(_WIN32)
        return "fang-input-daemon.exe";
#else
        return "fang-input-daemon";
#endif
    }
    return "fang-input-daemon-android-" + std::string(fang::guest::guest_target_abi_name(abi));
}

} // namespace

// ----------------------------------------------------------------------------
// MockGuestDeploymentCoordinator
// ----------------------------------------------------------------------------

MockGuestDeploymentCoordinator::MockGuestDeploymentCoordinator(
    GuestDeploymentPhase initial_phase,
    std::string initial_diag)
    : phase_(initial_phase), diagnostic_(std::move(initial_diag)) {}

GuestDeploymentReport MockGuestDeploymentCoordinator::assess_deployment_readiness(
    const GuestDeploymentTarget& target) {
    GuestDeploymentReport report;
    report.phase = phase_;
    report.diagnostic = diagnostic_;
    report.is_ready_for_push = (phase_ == GuestDeploymentPhase::ArtifactReady ||
                                phase_ == GuestDeploymentPhase::Deployed);
    report.is_deployed = (phase_ == GuestDeploymentPhase::Deployed);
    report.resolved_artifact_path = "mock/staging/" + get_expected_artifact_name(target.abi);
    report.artifact_size_bytes = 1048576;
    report.artifact_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    report.manifest = fang::guest::GuestToolchainDetector::generate_manifest_for_target(
        target.abi, target.build_config, "RecordingStub", target.require_privileged_backend);
    return report;
}

bool MockGuestDeploymentCoordinator::deploy_to_guest(
    const std::string& /*serial_or_avd*/,
    const GuestDeploymentTarget& target,
    GuestDeploymentReport& out_report) {
    out_report = assess_deployment_readiness(target);
    if (!push_success_) {
        out_report.phase = GuestDeploymentPhase::DeploymentFailed;
        out_report.diagnostic = "Mock deployment push simulated failure.";
        out_report.is_deployed = false;
        return false;
    }
    out_report.phase = GuestDeploymentPhase::Deployed;
    out_report.diagnostic = "Mock deployment push simulated success.";
    out_report.is_deployed = true;
    phase_ = GuestDeploymentPhase::Deployed;
    return true;
}

void MockGuestDeploymentCoordinator::teardown_guest_deployment(
    const std::string& /*serial_or_avd*/,
    GuestDeploymentReport& report) {
    report.phase = GuestDeploymentPhase::Unconfigured;
    report.diagnostic = "Mock deployment torn down.";
    report.is_deployed = false;
    if (phase_ == GuestDeploymentPhase::Deployed) {
        phase_ = GuestDeploymentPhase::ArtifactReady;
    }
}

// ----------------------------------------------------------------------------
// GuestDeploymentCoordinator
// ----------------------------------------------------------------------------

bool GuestDeploymentCoordinator::inspect_elf_header(
    const std::string& file_path,
    ElfHeaderInspection& out_inspection,
    std::string* error) {
    out_inspection = ElfHeaderInspection{};

    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        out_inspection.error_message = "Failed to open artifact file: " + file_path;
        if (error) *error = out_inspection.error_message;
        return false;
    }

    uint8_t buf[64]{0};
    file.read(reinterpret_cast<char*>(buf), sizeof(buf));
    const auto bytes_read = static_cast<size_t>(file.gcount());
    if (bytes_read < 4) {
        out_inspection.format = "Truncated";
        out_inspection.error_message = "Artifact file is smaller than 4 bytes (truncated or corrupt)";
        if (error) *error = out_inspection.error_message;
        return false;
    }

    if (buf[0] == 'M' && buf[1] == 'Z') {
        out_inspection.format = "PE-COFF";
        out_inspection.error_message = "Artifact is a Windows PE-COFF binary";
        if (error) *error = out_inspection.error_message;
        return false;
    }

    if (!(buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F')) {
        out_inspection.format = "Unknown";
        out_inspection.error_message = "Artifact does not start with valid ELF magic (0x7F 'ELF')";
        if (error) *error = out_inspection.error_message;
        return false;
    }

    out_inspection.format = "ELF";
    out_inspection.elf_class = buf[4];
    out_inspection.data_encoding = buf[5];
    out_inspection.version = buf[6];
    out_inspection.os_abi = buf[7];

    if (out_inspection.elf_class != 1 && out_inspection.elf_class != 2) {
        out_inspection.error_message = "Invalid ELF class identifier: " + std::to_string(out_inspection.elf_class);
        if (error) *error = out_inspection.error_message;
        return false;
    }

    const size_t min_header_size = (out_inspection.elf_class == 2) ? 64 : 52;
    if (bytes_read < min_header_size) {
        out_inspection.format = "Truncated";
        out_inspection.error_message = "ELF header truncated: read " + std::to_string(bytes_read) +
            " bytes, required " + std::to_string(min_header_size) + " bytes";
        if (error) *error = out_inspection.error_message;
        return false;
    }

    // Little-endian parsing (standard for Android targets)
    out_inspection.object_type = static_cast<uint16_t>(buf[16] | (buf[17] << 8));
    out_inspection.machine_arch = static_cast<uint16_t>(buf[18] | (buf[19] << 8));

    if (out_inspection.elf_class == 2) { // 64-bit
        uint64_t ep = 0;
        for (int i = 0; i < 8; ++i) {
            ep |= (static_cast<uint64_t>(buf[24 + i]) << (i * 8));
        }
        out_inspection.entry_point = ep;
        out_inspection.header_size = static_cast<uint16_t>(buf[52] | (buf[53] << 8));
        out_inspection.program_header_count = static_cast<uint16_t>(buf[56] | (buf[57] << 8));
    } else { // 32-bit
        uint32_t ep = static_cast<uint32_t>(buf[24] | (buf[25] << 8) | (buf[26] << 16) | (buf[27] << 24));
        out_inspection.entry_point = ep;
        out_inspection.header_size = static_cast<uint16_t>(buf[40] | (buf[41] << 8));
        out_inspection.program_header_count = static_cast<uint16_t>(buf[44] | (buf[45] << 8));
    }

    out_inspection.is_valid_elf = true;
    return true;
}

bool GuestDeploymentCoordinator::inspect_binary_format(
    const std::string& file_path,
    std::string& out_format,
    bool& out_is_elf,
    std::string* error) {
    ElfHeaderInspection insp;
    bool ok = inspect_elf_header(file_path, insp, error);
    out_format = insp.format;
    out_is_elf = insp.is_valid_elf;
    return ok || insp.format == "PE-COFF";
}

bool GuestDeploymentCoordinator::validate_elf_for_abi(
    const std::string& file_path,
    fang::guest::GuestTargetAbi expected_abi,
    ElfHeaderInspection& out_inspection,
    std::string* error) {
    if (!inspect_elf_header(file_path, out_inspection, error)) {
        return false;
    }

    const std::string abi_name = std::string(fang::guest::guest_target_abi_name(expected_abi));

    // Validate 32-bit vs 64-bit ELF class
    if (expected_abi == fang::guest::GuestTargetAbi::X86_64 ||
        expected_abi == fang::guest::GuestTargetAbi::Arm64_v8a) {
        if (out_inspection.elf_class != 2) {
            out_inspection.error_message = "ELF class mismatch: binary is 32-bit (ELFCLASS32), expected 64-bit (ELFCLASS64) for " + abi_name;
            if (error) *error = out_inspection.error_message;
            return false;
        }
    } else if (expected_abi == fang::guest::GuestTargetAbi::X86 ||
               expected_abi == fang::guest::GuestTargetAbi::Armeabi_v7a) {
        if (out_inspection.elf_class != 1) {
            out_inspection.error_message = "ELF class mismatch: binary is 64-bit (ELFCLASS64), expected 32-bit (ELFCLASS32) for " + abi_name;
            if (error) *error = out_inspection.error_message;
            return false;
        }
    }

    // Validate data encoding: Android guest ABIs are little-endian
    if (out_inspection.data_encoding != 1) {
        out_inspection.error_message = "ELF data encoding mismatch: binary is big-endian (ELFDATA2MSB), expected little-endian (ELFDATA2LSB) for " + abi_name;
        if (error) *error = out_inspection.error_message;
        return false;
    }

    // Validate object type: ET_EXEC (2) or ET_DYN (3, PIE)
    if (out_inspection.object_type != 2 && out_inspection.object_type != 3) {
        out_inspection.error_message = "ELF object type mismatch: type is " + std::to_string(out_inspection.object_type) + " (expected executable ET_EXEC=2 or ET_DYN=3)";
        if (error) *error = out_inspection.error_message;
        return false;
    }

    // Validate machine architecture
    uint16_t expected_machine = 0;
    std::string expected_arch_str;
    switch (expected_abi) {
    case fang::guest::GuestTargetAbi::X86_64:
        expected_machine = 0x3E; // EM_X86_64 (62)
        expected_arch_str = "x86_64 (0x3E)";
        break;
    case fang::guest::GuestTargetAbi::Arm64_v8a:
        expected_machine = 0xB7; // EM_AARCH64 (183)
        expected_arch_str = "AArch64 (0xB7)";
        break;
    case fang::guest::GuestTargetAbi::X86:
        expected_machine = 0x03; // EM_386 (3)
        expected_arch_str = "i386 (0x03)";
        break;
    case fang::guest::GuestTargetAbi::Armeabi_v7a:
        expected_machine = 0x28; // EM_ARM (40)
        expected_arch_str = "ARM32 (0x28)";
        break;
    default:
        break;
    }

    if (expected_machine != 0 && out_inspection.machine_arch != expected_machine) {
        std::ostringstream ss;
        ss << "ELF machine architecture mismatch: binary machine is 0x"
           << std::hex << out_inspection.machine_arch << std::dec
           << ", expected " << expected_arch_str << " for ABI " << abi_name;
        out_inspection.error_message = ss.str();
        if (error) *error = out_inspection.error_message;
        return false;
    }

    // Program headers check
    if (out_inspection.program_header_count == 0) {
        out_inspection.error_message = "ELF executable contains 0 program headers; binary is corrupt or incomplete";
        if (error) *error = out_inspection.error_message;
        return false;
    }

    return true;
}

static std::string get_current_source_revision(const std::string& repo_root) {
    std::string result;
    std::string cmd = "git -C \"" + repo_root + "\" rev-parse HEAD 2>\\nul";
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    _pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) result.pop_back();
    return result;
}

bool GuestDeploymentCoordinator::validate_artifact_manifest(
    const std::string& manifest_path,
    const std::string& artifact_path,
    fang::guest::GuestTargetAbi expected_abi,
    fang::guest::GuestArtifactManifest& out_manifest,
    std::string* error,
    uint32_t expected_api_level) {
    std::ifstream mf(manifest_path);
    if (!mf) {
        if (error) *error = "Failed to open companion manifest: " + manifest_path;
        return false;
    }

    std::ostringstream ss;
    ss << mf.rdbuf();
    std::string json = ss.str();

    std::string parse_err;
    if (!fang::guest::GuestArtifactManifest::from_json(json, out_manifest, &parse_err)) {
        if (error) *error = "Manifest parsing failed: " + parse_err;
        return false;
    }

    const std::string expected_abi_str = std::string(fang::guest::guest_target_abi_name(expected_abi));
    if (out_manifest.target_abi != expected_abi_str) {
        if (error) *error = "Manifest target ABI mismatch: manifest specifies '" + out_manifest.target_abi +
            "', expected '" + expected_abi_str + "'";
        return false;
    }

    if (!out_manifest.artifact_sha256.empty()) {
        std::string actual_sha256 = calculate_file_sha256(artifact_path);
        if (actual_sha256 != out_manifest.artifact_sha256) {
            if (error) *error = "Artifact SHA-256 digest (" + actual_sha256.substr(0, 16) +
                "...) does not match manifest digest (" + out_manifest.artifact_sha256.substr(0, 16) + "...)"; 
            return false;
        }
    }

    // Cross-check manifest ELF metadata against the actual artifact bytes.
    ElfHeaderInspection actual_elf{};
    std::string elf_err;
    if (!inspect_elf_header(artifact_path, actual_elf, &elf_err)) {
        if (error) *error = "Artifact ELF inspection failed during manifest validation: " + elf_err;
        return false;
    }
    if (actual_elf.elf_class != out_manifest.elf_class) {
        if (error) *error = "Manifest ELF class (" + std::to_string(out_manifest.elf_class) +
            ") does not match artifact ELF class (" + std::to_string(actual_elf.elf_class) + ")";
        return false;
    }
    if (actual_elf.data_encoding != out_manifest.elf_data) {
        if (error) *error = "Manifest ELF data encoding does not match artifact";
        return false;
    }
    if (out_manifest.elf_machine != std::to_string(actual_elf.machine_arch)) {
        // normalize: compare hex form too
        std::ostringstream hx; hx << "0x" << std::hex << actual_elf.machine_arch;
        if (out_manifest.elf_machine != hx.str()) {
            if (error) *error = "Manifest ELF machine (" + out_manifest.elf_machine +
                ") does not match artifact ELF machine (" + hx.str() + ")";
            return false;
        }
    }
    if (out_manifest.build_result != "Success") {
        if (error) *error = "Manifest build_result is '" + out_manifest.build_result + "', expected 'Success'; artifact is not build-ready";
        return false;
    }
    if (out_manifest.toolchain_status != fang::guest::ToolchainStatus::ToolchainAvailable) {
        if (error) *error = "Manifest toolchain_status is not ToolchainAvailable; NDK/toolchain prerequisites are missing";
        return false;
    }
    if (!out_manifest.is_cross_compiled) {
        if (error) *error = "Manifest is_cross_compiled is false; artifact is not a cross-compiled guest binary";
        return false;
    }
    if (!out_manifest.is_android_elf_binary) {
        if (error) *error = "Manifest is_android_elf_binary is false; artifact is not an Android ELF";
        return false;
    }
    if (out_manifest.android_api_level < expected_api_level) {
        if (error) *error = "Manifest android_api_level " + std::to_string(out_manifest.android_api_level) +
            " is below expected minimum " + std::to_string(expected_api_level);
        return false;
    }
    if (out_manifest.elf_class == 0 || out_manifest.elf_data == 0 || out_manifest.elf_machine.empty()) {
        if (error) *error = "Manifest ELF metadata (class/data/machine) is incomplete";
        return false;
    }

    return true;
}

std::string GuestDeploymentCoordinator::calculate_file_sha256(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return {};

    sha256::Context ctx;
    uint8_t buffer[8192];
    while (file.read(reinterpret_cast<char*>(buffer), sizeof(buffer))) {
        ctx.update(buffer, static_cast<size_t>(file.gcount()));
    }
    if (file.gcount() > 0) {
        ctx.update(buffer, static_cast<size_t>(file.gcount()));
    }
    return ctx.finalize();
}

// ----------------------------------------------------------------------------
// Source Evidence Collection
// ----------------------------------------------------------------------------

static std::string run_git_command(const std::string& repo_root, const std::string& args) {
    std::string cmd = "git -C \"" + repo_root + "\" " + args + " 2>\\nul";
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string result;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    _pclose(pipe);
    return result;
}

static std::string get_utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc;
#ifdef _WIN32
    gmtime_s(&tm_utc, &time_t);
#else
    gmtime_r(&time_t, &tm_utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

struct FingerprintEntry {
    std::string path;
    std::string hash;
    bool is_deleted = false;
    bool is_untracked = false;
};

static std::string hash_file_sha256(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return {};
    sha256::Context ctx;
    uint8_t buffer[8192];
    while (file.read(reinterpret_cast<char*>(buffer), sizeof(buffer))) {
        ctx.update(buffer, static_cast<size_t>(file.gcount()));
    }
    if (file.gcount() > 0) {
        ctx.update(buffer, static_cast<size_t>(file.gcount()));
    }
    return ctx.finalize();
}

bool GuestDeploymentCoordinator::collect_current_source_evidence(
    const std::string& repo_root,
    const std::string& abi,
    uint32_t api_level,
    const std::string& configuration,
    const std::string& target_triple,
    const std::string& ndk_version,
    const std::vector<std::string>& compile_args,
    fang::guest::SourceEvidence& out_evidence,
    std::string* error) {
    out_evidence = fang::guest::SourceEvidence{};
    out_evidence.status = fang::guest::SourceEvidence::Status::Unavailable;
    out_evidence.algorithm = "sha256";
    out_evidence.scope = "fang-guest-daemon-inputs-v1";
    out_evidence.encoding = "fang-cim-c14n-v1";
    out_evidence.captured_at_utc = get_utc_timestamp();

    // Get current HEAD revision
    std::string head_rev = run_git_command(repo_root, "rev-parse HEAD");
    if (head_rev.empty()) {
        out_evidence.unavailable_reasons.push_back("git rev-parse HEAD failed or returned empty");
        if (error) *error = "Failed to get current HEAD revision";
        return false;
    }
    while (!head_rev.empty() && (head_rev.back() == '\n' || head_rev.back() == '\r' || head_rev.back() == ' ')) {
        head_rev.pop_back();
    }

    // Get porcelain v2 status with NUL delimiter (for source_tree_state)
    std::string status_output = run_git_command(repo_root, "status --porcelain=v2 -z");
    if (status_output.empty()) {
        out_evidence.unavailable_reasons.push_back("git status --porcelain=v2 -z failed or returned empty");
        if (error) *error = "Failed to get git status";
        return false;
    }

    std::vector<std::string> errors;
    std::string fingerprint = build_canonical_fingerprint(
        repo_root,
        head_rev,
        abi,
        api_level,
        configuration,
        target_triple,
        ndk_version,
        compile_args,
        &errors
    );

    if (!errors.empty()) {
        out_evidence.unavailable_reasons.insert(out_evidence.unavailable_reasons.end(), errors.begin(), errors.end());
        out_evidence.status = fang::guest::SourceEvidence::Status::Unavailable;
        if (error) *error = "Failed to compute canonical fingerprint: " + errors[0];
        return false;
    }

    out_evidence.fingerprint = fingerprint;
    out_evidence.status = fang::guest::SourceEvidence::Status::Collected;
    return true;
}

ProvenanceAssessment GuestDeploymentCoordinator::assess_provenance(
    const fang::guest::GuestArtifactManifest& manifest,
    const fang::guest::SourceEvidence& current_evidence,
    std::string* diagnostic) {
    // Legacy manifest without provenance fields
    if (manifest.manifest_schema_version.empty() || 
        manifest.manifest_schema_version == "stage9-v1" ||
        manifest.manifest_schema_version == "stage9-v2" ||
        !manifest.source_evidence.is_collected()) {
        if (diagnostic) *diagnostic = "Legacy manifest without source evidence; provenance unverified";
        return ProvenanceAssessment::LegacyUnverified;
    }

    // Check scope and encoding compatibility
    const std::string expected_scope = "fang-guest-daemon-inputs-v1";
    const std::string expected_encoding = "fang-cim-c14n-v1";
    if (manifest.source_evidence.scope != expected_scope || 
        manifest.source_evidence.encoding != expected_encoding) {
        if (diagnostic) *diagnostic = "Manifest uses incompatible scope/encoding: " + manifest.source_evidence.scope + "/" + manifest.source_evidence.encoding;
        return ProvenanceAssessment::LegacyUnverified;
    }

    // Current evidence unavailable
    if (!current_evidence.is_collected()) {
        if (diagnostic) *diagnostic = "Current source evidence unavailable: " + 
            (current_evidence.unavailable_reasons.empty() ? "unknown reason" : current_evidence.unavailable_reasons[0]);
        return ProvenanceAssessment::Unavailable;
    }

    // Check current evidence scope/encoding matches expected
    if (current_evidence.scope != expected_scope || current_evidence.encoding != expected_encoding) {
        if (diagnostic) *diagnostic = "Current evidence uses incompatible scope/encoding";
        return ProvenanceAssessment::Unavailable;
    }

    // Recorded evidence unavailable
    if (!manifest.source_evidence.is_collected()) {
        if (diagnostic) *diagnostic = "Recorded source evidence unavailable in manifest";
        return ProvenanceAssessment::Unavailable;
    }

    // Compare fingerprints
    if (current_evidence.fingerprint == manifest.source_evidence.fingerprint) {
        // Check if build was dirty
        if (manifest.source_tree_state == "dirty") {
            if (diagnostic) *diagnostic = "Source evidence matches recorded build evidence (dirty build); not reproducible";
            return ProvenanceAssessment::QualifiedDirty;
        } else {
            if (diagnostic) *diagnostic = "Source evidence matches recorded build evidence";
            return ProvenanceAssessment::QualifiedCurrent;
        }
    } else {
        if (diagnostic) *diagnostic = "Source evidence differs from recorded build evidence";
        return ProvenanceAssessment::Stale;
    }
}

// ----------------------------------------------------------------------------
// GuestDeploymentCoordinator::assess_deployment_readiness
// ----------------------------------------------------------------------------

GuestDeploymentReport GuestDeploymentCoordinator::assess_deployment_readiness(const GuestDeploymentTarget& target) {
    GuestDeploymentReport report;

    const std::string expected_name = get_expected_artifact_name(target.abi);

    // Locate candidate artifact
    std::vector<std::filesystem::path> search_paths;
    if (!target.staging_dir.empty()) {
        search_paths.push_back(std::filesystem::path(target.staging_dir));
    }
    search_paths.push_back(std::filesystem::path("out/build/Release"));
    search_paths.push_back(std::filesystem::path("out/build/Debug"));
    search_paths.push_back(std::filesystem::path("out/staging"));
    search_paths.push_back(std::filesystem::path("."));

    std::filesystem::path resolved_path;
    for (const auto& dir : search_paths) {
        auto candidate = dir / expected_name;
        if (std::filesystem::exists(candidate)) {
            resolved_path = candidate;
            break;
        }
    }

    // 1. Host Native Target Evaluation
    if (target.abi == fang::guest::GuestTargetAbi::HostNative) {
        if (resolved_path.empty()) {
            report.phase = GuestDeploymentPhase::ArtifactMissing;
            report.diagnostic = "Host native daemon binary '" + expected_name + "' not found in build outputs.";
            return report;
        }

        std::string fmt;
        bool is_elf = false;
        std::string err;
        if (!inspect_binary_format(resolved_path.string(), fmt, is_elf, &err)) {
            report.phase = GuestDeploymentPhase::ArtifactCorrupt;
            report.diagnostic = "Host native artifact format unreadable: " + err;
            return report;
        }

        report.phase = GuestDeploymentPhase::ArtifactReady;
        report.resolved_artifact_path = resolved_path.string();
        report.artifact_size_bytes = std::filesystem::file_size(resolved_path);
        report.artifact_sha256 = calculate_file_sha256(resolved_path.string());
        report.is_ready_for_push = true;
        report.diagnostic = "Host native daemon artifact verified (" + fmt + ", " +
            std::to_string(report.artifact_size_bytes) + " bytes).";
        
        // Provenance assessment for host native
        fang::guest::SourceEvidence current_evidence;
        std::string prov_diagnostic;
        std::string repo_root = std::filesystem::current_path().string();
        
        // Build target triple for host native (not Android)
        std::string host_target_triple = "x86_64-pc-windows-msvc"; // Default for Windows host
        std::vector<std::string> host_compile_args = {
            "-std=c++20",
            "-fPIE",
            "-Wall",
            "-Wextra",
            "-I", repo_root + "/apps/fang-input-daemon",
            "-I", repo_root + "/apps/fang-studio",
            "-O2",
            "-DNDEBUG"
        };
        
        if (collect_current_source_evidence(
                repo_root,
                "x86_64",  // host ABI
                0,         // API level not applicable for host
                target.build_config,
                host_target_triple,
                "msvc",    // compiler ID for host
                host_compile_args,
                current_evidence)) {
            report.provenance_assessment = assess_provenance(report.manifest, current_evidence, &prov_diagnostic);
        } else {
            report.provenance_assessment = ProvenanceAssessment::Unavailable;
            prov_diagnostic = "Failed to collect current source evidence for provenance assessment";
        }
        if (!prov_diagnostic.empty()) {
            if (!report.diagnostic.empty()) report.diagnostic += " | ";
            report.diagnostic += "Provenance: " + prov_diagnostic;
        }
        return report;
    }

    // 2. Android Guest Target Evaluation
    auto env = fang::guest::GuestToolchainDetector::inspect_environment();

    // Check if artifact was resolved
    if (resolved_path.empty()) {
        if (!env.ndk_found || !env.clang_found) {
            report.phase = GuestDeploymentPhase::BlockedMissingToolchain;
            report.diagnostic = "Android NDK r25+ toolchain is missing on host, and pre-compiled ELF binary '" +
                expected_name + "' was not found in staging. Install Android NDK to cross-compile for Android guest.";
        } else {
            report.phase = GuestDeploymentPhase::ArtifactMissing;
            report.diagnostic = "Android ELF binary '" + expected_name +
                "' not found in staging directory. Rebuild Android guest target before deploying.";
        }
        return report;
    }

    // Artifact exists: perform deep ELF validation for the target ABI
    ElfHeaderInspection elf_info;
    std::string elf_err;
    if (!validate_elf_for_abi(resolved_path.string(), target.abi, elf_info, &elf_err)) {
        if (elf_info.format == "PE-COFF" ||
            elf_err.find("architecture mismatch") != std::string::npos ||
            elf_err.find("ELF class mismatch") != std::string::npos ||
            elf_err.find("data encoding mismatch") != std::string::npos ||
            elf_err.find("object type mismatch") != std::string::npos) {
            report.phase = GuestDeploymentPhase::ArtifactArchitectureMismatch;
            if (elf_info.format == "PE-COFF") {
                report.diagnostic = "Artifact '" + resolved_path.string() + "' format is Windows PE-COFF; Android guest target requires an ELF executable for ABI " + std::string(fang::guest::guest_target_abi_name(target.abi)) + ".";
            } else {
                report.diagnostic = "Artifact '" + resolved_path.string() + "' architecture/ABI mismatch: " + elf_err;
            }
        } else if (elf_info.format == "Truncated" ||
                   elf_err.find("truncated") != std::string::npos ||
                   elf_err.find("corrupt") != std::string::npos ||
                   elf_err.find("program headers") != std::string::npos ||
                   !elf_info.is_valid_elf) {
            report.phase = GuestDeploymentPhase::ArtifactCorrupt;
            report.diagnostic = "Candidate artifact '" + resolved_path.string() + "' is corrupt or unreadable: " + elf_err;
        } else {
            report.phase = GuestDeploymentPhase::ArtifactArchitectureMismatch;
            report.diagnostic = "Artifact '" + resolved_path.string() + "' validation failed: " + elf_err;
        }
        return report;
    }

    // Inspect companion manifest if present
    std::filesystem::path manifest_candidate1 = resolved_path.parent_path() / "guest_manifest.json";
    std::filesystem::path manifest_candidate2 = resolved_path.string() + ".manifest.json";
    std::filesystem::path manifest_to_validate;
    if (std::filesystem::exists(manifest_candidate1)) {
        manifest_to_validate = manifest_candidate1;
    } else if (std::filesystem::exists(manifest_candidate2)) {
        manifest_to_validate = manifest_candidate2;
    }

    if (!manifest_to_validate.empty()) {
        std::string manifest_err;
        if (!validate_artifact_manifest(manifest_to_validate.string(), resolved_path.string(), target.abi, report.manifest, &manifest_err, target.android_api_level)) {
            report.phase = GuestDeploymentPhase::ArtifactManifestMismatch;
            report.diagnostic = "Companion manifest validation failed: " + manifest_err;
            return report;
        }
        // Freshness check: if source revision is recorded, compare it to the current repository HEAD.
        if (!report.manifest.source_revision.empty()) {
            std::string current_revision = get_current_source_revision(std::filesystem::current_path().string());
            if (!current_revision.empty() && current_revision != report.manifest.source_revision) {
                report.phase = GuestDeploymentPhase::ArtifactManifestMismatch;
                report.diagnostic = "Artifact is stale: manifest source_revision does not match current source revision (" + current_revision.substr(0, 12) + "). Rebuild the guest daemon.";
                return report;
            }
        }

        // Build configuration consistency.
        if (!report.manifest.build_configuration.empty() && report.manifest.build_configuration != target.build_config) {
            report.phase = GuestDeploymentPhase::ArtifactManifestMismatch;
            report.diagnostic = "Artifact build_configuration (" + report.manifest.build_configuration + ") does not match requested build configuration (" + target.build_config + ").";
            return report;
        }

        // Provenance assessment
        fang::guest::SourceEvidence current_evidence;
        std::string prov_diagnostic;
        std::string repo_root = std::filesystem::current_path().string();
        
        // Extract target info from manifest and target
        std::string abi_str = std::string(fang::guest::guest_target_abi_name(target.abi));
        std::string target_triple = abi_str + "-linux-android" + std::to_string(target.android_api_level);
        std::string ndk_version = report.manifest.ndk_version.empty() ? "unknown" : report.manifest.ndk_version;
        
        std::vector<std::string> compile_args = {
            "--target=" + target_triple,
            "-std=c++20",
            "-fPIE",
            "-pie",
            "-Wall",
            "-Wextra",
            "-D__ANDROID__",
            "-I", repo_root + "/apps/fang-input-daemon",
            "-I", repo_root + "/apps/fang-studio"
        };
        if (target.build_config == "Release") {
            compile_args.push_back("-O2");
            compile_args.push_back("-DNDEBUG");
        } else {
            compile_args.push_back("-O0");
            compile_args.push_back("-g");
            compile_args.push_back("-DFANG_DEBUG");
        }
        
        if (collect_current_source_evidence(
                repo_root,
                abi_str,
                target.android_api_level,
                target.build_config,
                target_triple,
                ndk_version,
                compile_args,
                current_evidence)) {
            report.provenance_assessment = assess_provenance(report.manifest, current_evidence, &prov_diagnostic);
        } else {
            report.provenance_assessment = ProvenanceAssessment::Unavailable;
            prov_diagnostic = "Failed to collect current source evidence for provenance assessment";
        }
        // Append provenance diagnostic to main diagnostic
        if (!prov_diagnostic.empty()) {
            if (!report.diagnostic.empty()) report.diagnostic += " | ";
            report.diagnostic += "Provenance: " + prov_diagnostic;
        }
    } else {
        // Populate manifest fields from inspected ELF artifact
        report.manifest.artifact_path = resolved_path.string();
        report.manifest.artifact_size_bytes = std::filesystem::file_size(resolved_path);
        report.manifest.artifact_sha256 = calculate_file_sha256(resolved_path.string());
        report.manifest.build_result = "Success";
        report.manifest.elf_class = elf_info.elf_class;
        report.manifest.elf_data = elf_info.data_encoding;
        report.manifest.elf_machine = std::to_string(elf_info.machine_arch);

        // Provenance assessment for artifact without manifest
        fang::guest::SourceEvidence current_evidence;
        std::string prov_diagnostic;
        std::string repo_root = std::filesystem::current_path().string();
        
        // Extract target info from target parameter
        std::string abi_str = std::string(fang::guest::guest_target_abi_name(target.abi));
        std::string target_triple = abi_str + "-linux-android" + std::to_string(target.android_api_level);
        std::string ndk_version = "unknown"; // No manifest to get NDK version from
        
        std::vector<std::string> compile_args = {
            "--target=" + target_triple,
            "-std=c++20",
            "-fPIE",
            "-pie",
            "-Wall",
            "-Wextra",
            "-D__ANDROID__",
            "-I", repo_root + "/apps/fang-input-daemon",
            "-I", repo_root + "/apps/fang-studio"
        };
        if (target.build_config == "Release") {
            compile_args.push_back("-O2");
            compile_args.push_back("-DNDEBUG");
        } else {
            compile_args.push_back("-O0");
            compile_args.push_back("-g");
            compile_args.push_back("-DFANG_DEBUG");
        }
        
        if (collect_current_source_evidence(
                repo_root,
                abi_str,
                target.android_api_level,
                target.build_config,
                target_triple,
                ndk_version,
                compile_args,
                current_evidence)) {
            report.provenance_assessment = assess_provenance(report.manifest, current_evidence, &prov_diagnostic);
        } else {
            report.provenance_assessment = ProvenanceAssessment::Unavailable;
            prov_diagnostic = "Failed to collect current source evidence for provenance assessment";
        }
        if (!prov_diagnostic.empty()) {
            if (!report.diagnostic.empty()) report.diagnostic += " | ";
            report.diagnostic += "Provenance: " + prov_diagnostic;
        }
    }

    report.phase = GuestDeploymentPhase::ArtifactReady;
    report.resolved_artifact_path = resolved_path.string();
    report.artifact_size_bytes = std::filesystem::file_size(resolved_path);
    report.artifact_sha256 = calculate_file_sha256(resolved_path.string());
    report.is_ready_for_push = true;
    report.diagnostic = "Android ELF artifact verified (" + std::string(fang::guest::guest_target_abi_name(target.abi)) +
        ", " + std::to_string(report.artifact_size_bytes) + " bytes, SHA256: " +
        report.artifact_sha256.substr(0, 12) + "...).";
    return report;
}

// ----------------------------------------------------------------------------
// MockGuestDeploymentCoordinator
// ----------------------------------------------------------------------------

bool GuestDeploymentCoordinator::deploy_to_guest(
    const std::string& serial_or_avd,
    const GuestDeploymentTarget& target,
    GuestDeploymentReport& out_report) {
    out_report = assess_deployment_readiness(target);
    if (!out_report.is_ready_for_push) {
        return false;
    }

    // Host native target in loopback mode
    if (target.abi == fang::guest::GuestTargetAbi::HostNative) {
        out_report.phase = GuestDeploymentPhase::Deployed;
        out_report.is_deployed = true;
        out_report.diagnostic = "Host native daemon staged and running in loopback test mode.";
        return true;
    }

    // Android guest target: safely enforce offline boundaries
    // Live guest deployment via ADB push is strictly blocked offline
    out_report.phase = GuestDeploymentPhase::DeploymentBlockedOffline;
    out_report.is_deployed = false;
    out_report.diagnostic = "Live guest deployment via ADB push ('adb -s " + serial_or_avd +
        " push " + out_report.resolved_artifact_path + " " + target.guest_install_path +
        "') is blocked offline. Live deployment remains unauthorized.";
    return false;
}

void GuestDeploymentCoordinator::teardown_guest_deployment(
    const std::string& /*serial_or_avd*/,
    GuestDeploymentReport& report) {
    report.phase = GuestDeploymentPhase::Unconfigured;
    report.diagnostic = "Guest deployment unconfigured.";
    report.is_deployed = false;
}

} // namespace fang::studio
