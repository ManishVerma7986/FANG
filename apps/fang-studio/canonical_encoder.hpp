#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

namespace fang::studio::canonical {

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

struct CanonicalEntry {
    std::string path;
    CanonicalRole role;
    CanonicalKind kind;
    uint64_t size;
    std::string hash;  // 64-char lowercase hex
};

struct CanonicalInput {
    std::string repo_root;
    std::string source_revision;
    std::string abi;
    uint32_t api_level;
    std::string configuration;
    std::string target_triple;
    std::string ndk_version;
    std::vector<std::string> compile_args;
    std::vector<CanonicalEntry> entries;
};

struct CanonicalResult {
    std::vector<uint8_t> canonical_bytes;
    std::string fingerprint_sha256;  // 64-char lowercase hex
    std::vector<std::string> errors;
    bool success() const { return errors.empty(); }
};

// Build canonical binary stream (fang-cim-c14n-v1) from structured input
// This is the core deterministic encoding function that both PowerShell and C++
// implementations must produce identical output for identical inputs.
CanonicalResult build_canonical_stream(const CanonicalInput& input);

// Compute SHA-256 of a byte vector, returning 64-char lowercase hex string
std::string sha256_hex(const std::vector<uint8_t>& data);

// Compute SHA-256 of a file, returning 64-char lowercase hex string
std::string sha256_file_hex(const std::string& file_path);

// Helper to convert hex string to raw bytes (32 bytes for SHA-256)
std::vector<uint8_t> hex_to_bytes(const std::string& hex);

} // namespace fang::studio::canonical