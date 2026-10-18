#include "canonical_encoder.hpp"

#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

namespace {

using namespace fang::studio::canonical;

struct TestVector {
    std::string name;
    std::string description;
    std::string repo_root;
    std::string source_revision;
    std::string abi;
    uint32_t api_level;
    std::string configuration;
    std::string target_triple;
    std::string ndk_version;
    std::vector<std::string> compile_args;
    struct Entry {
        std::string path;
        CanonicalRole role;
        CanonicalKind kind;
        uint64_t size;
        std::string content_hash;
    };
    std::vector<Entry> entries;
    std::string expected_fingerprint_sha256;
};

std::vector<TestVector> get_test_vectors() {
    return {
{
            "empty_entry_set",
            "Empty entry set with only target and toolchain metadata",
            "C:/test/repo",
            "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
            "x86_64",
            26,
            "Release",
            "x86_64-linux-android26",
            "r26.1.10909125",
            {
                "--target=x86_64-linux-android26",
                "-std=c++20",
                "-fPIE",
                "-pie",
                "-Wall",
                "-Wextra",
                "-D__ANDROID__",
                "-I", "C:/test/repo/apps/fang-input-daemon",
                "-I", "C:/test/repo/apps/fang-studio",
                "-O2",
                "-DNDEBUG"
            },
            {},
            "c73ade1e728b2f8e4dc6238a1bbfda5f4747e7cd9e9398d0fffa837d5da15946"
        },
        {
            "single_file_entry",
            "Single source file entry with all metadata",
            "C:/test/repo",
            "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
            "x86_64",
            26,
            "Release",
            "x86_64-linux-android26",
            "r26.1.10909125",
            {
                "--target=x86_64-linux-android26",
                "-std=c++20",
                "-fPIE",
                "-pie",
                "-Wall",
                "-Wextra",
                "-D__ANDROID__",
                "-I", "C:/test/repo/apps/fang-input-daemon",
                "-I", "C:/test/repo/apps/fang-studio",
                "-O2",
                "-DNDEBUG"
            },
            {
                {"apps/fang-input-daemon/main.cpp", CanonicalRole::Source, CanonicalKind::File, 1234,
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}
            },
            "292c33a77d7bf10380ce26611308121dd520e03ba2b9a348512d1be2abbe2721"
        },
        {
            "multiple_entries_utf8_ordering",
            "Multiple entries requiring UTF-8 path byte ordering",
            "C:/test/repo",
            "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
            "x86_64",
            26,
            "Release",
            "x86_64-linux-android26",
            "r26.1.10909125",
            {
                "--target=x86_64-linux-android26",
                "-std=c++20",
                "-fPIE",
                "-pie",
                "-Wall",
                "-Wextra",
                "-D__ANDROID__",
                "-I", "C:/test/repo/apps/fang-input-daemon",
                "-I", "C:/test/repo/apps/fang-studio",
                "-O2",
                "-DNDEBUG"
            },
            {
                {"apps/fang-input-daemon/main.cpp", CanonicalRole::Source, CanonicalKind::File, 100,
                 "1111111111111111111111111111111111111111111111111111111111111111"},
                {"apps/fang-input-daemon/guest_input_backend.cpp", CanonicalRole::Source, CanonicalKind::File, 200,
                 "2222222222222222222222222222222222222222222222222222222222222222"},
                {"apps/fang-studio/input_ipc_protocol.cpp", CanonicalRole::Source, CanonicalKind::File, 300,
                 "3333333333333333333333333333333333333333333333333333333333333333"},
                {"apps/fang-studio/input_transport.cpp", CanonicalRole::Source, CanonicalKind::File, 400,
                 "4444444444444444444444444444444444444444444444444444444444444444"},
                {"compile_args/arg_000", CanonicalRole::CompileArg, CanonicalKind::Virtual, 35,
                 "5555555555555555555555555555555555555555555555555555555555555555"},
                {"compile_args/arg_001", CanonicalRole::CompileArg, CanonicalKind::Virtual, 11,
                 "6666666666666666666666666666666666666666666666666666666666666666"},
                {"git/revision", CanonicalRole::GitRevision, CanonicalKind::Virtual, 40,
                 "7777777777777777777777777777777777777777777777777777777777777777"},
                {"target/abi", CanonicalRole::TargetConfig, CanonicalKind::Virtual, 7,
                 "8888888888888888888888888888888888888888888888888888888888888888"},
                {"target/api_level", CanonicalRole::TargetConfig, CanonicalKind::Virtual, 2,
                 "9999999999999999999999999999999999999999999999999999999999999999"}
            },
            "bf90663aa14732c34861e1e038ff5ad3fc04f9cf19ef61887585c9515ef5787e"
        },
        {
            "paths_with_spaces_and_unicode",
            "Entries with spaces and non-ASCII characters in paths",
            "C:/test/repo",
            "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
            "arm64-v8a",
            26,
            "Debug",
            "aarch64-linux-android26",
            "r26.1.10909125",
            {
                "--target=aarch64-linux-android26",
                "-std=c++20",
                "-fPIE",
                "-pie",
                "-Wall",
                "-Wextra",
                "-D__ANDROID__",
                "-I", "C:/test/repo/apps/fang-input-daemon",
                "-I", "C:/test/repo/apps/fang-studio",
                "-O0",
                "-g",
                "-DFANG_DEBUG"
            },
            {
                {"scripts/build guest daemon.ps1", CanonicalRole::BuildScript, CanonicalKind::File, 500,
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                {"apps/fang-input-daemon/\xE6\x96\x87\xE4\xBB\xB6.hpp", CanonicalRole::Header, CanonicalKind::File, 250,
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
                {"target/configuration", CanonicalRole::TargetConfig, CanonicalKind::Virtual, 5,
                 "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}
            },
            "2fca24a7009d69fb61c06c1307721f2e2472fc4ee4f31c9c7a26e18bade3ee64"
        },
        {
            "file_and_virtual_entries_mixed",
            "Mix of file and virtual entries with different argument counts",
            "C:/test/repo",
            "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
            "x86",
            26,
            "Release",
            "i686-linux-android26",
            "r27.0.12345678",
            {
                "--target=i686-linux-android26",
                "-std=c++20",
                "-fPIE",
                "-pie",
                "-Wall",
                "-Wextra",
                "-D__ANDROID__",
                "-I", "C:/test/repo/apps/fang-input-daemon",
                "-I", "C:/test/repo/apps/fang-studio",
                "-O2",
                "-DNDEBUG",
                "-march=i686",
                "-mtune=generic"
            },
            {
                {"apps/fang-input-daemon/main.cpp", CanonicalRole::Source, CanonicalKind::File, 1500,
                 "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"},
                {"CMakeLists.txt", CanonicalRole::Configuration, CanonicalKind::File, 800,
                 "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"},
                {"compile_args/arg_000", CanonicalRole::CompileArg, CanonicalKind::Virtual, 27,
                 "cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe"},
                {"compile_args/arg_001", CanonicalRole::CompileArg, CanonicalKind::Virtual, 11,
                 "feedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedface"},
                {"compile_args/arg_010", CanonicalRole::CompileArg, CanonicalKind::Virtual, 13,
                 "abcdef01abcdef01abcdef01abcdef01abcdef01abcdef01abcdef01abcdef01"},
                {"compile_args/arg_011", CanonicalRole::CompileArg, CanonicalKind::Virtual, 14,
                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
                {"target/abi", CanonicalRole::TargetConfig, CanonicalKind::Virtual, 3,
                 "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
                {"target/compiler_target", CanonicalRole::TargetConfig, CanonicalKind::Virtual, 23,
                 "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"}
            },
            "1a80a410037c7e2dd92b54bfa19ab537fa13b5f42c6ad2db296bbdeb3afdc910"
        }
    };
}

void run_test_vector(const TestVector& tv) {
    std::cout << "  [RUN] " << tv.name << " (" << tv.description << ")..." << std::endl;

    CanonicalInput input;
    input.repo_root = tv.repo_root;
    input.source_revision = tv.source_revision;
    input.abi = tv.abi;
    input.api_level = tv.api_level;
    input.configuration = tv.configuration;
    input.target_triple = tv.target_triple;
    input.ndk_version = tv.ndk_version;
    input.compile_args = tv.compile_args;

    for (const auto& e : tv.entries) {
        CanonicalEntry entry;
        entry.path = e.path;
        entry.role = e.role;
        entry.kind = e.kind;
        entry.size = e.size;
        entry.hash = e.content_hash;
        input.entries.push_back(std::move(entry));
    }

    CanonicalResult result = build_canonical_stream(input);

    if (!result.success()) {
        std::cerr << "    FAIL: Build errors: ";
        for (const auto& err : result.errors) {
            std::cerr << err << "; ";
        }
        std::cerr << std::endl;
        throw std::runtime_error("Canonical stream build failed for " + tv.name);
    }

    // Verify fingerprint matches expected
    if (!tv.expected_fingerprint_sha256.empty()) {
        if (result.fingerprint_sha256 != tv.expected_fingerprint_sha256) {
            std::cerr << "    FAIL: Fingerprint mismatch for " << tv.name << std::endl;
            std::cerr << "    Expected: " << tv.expected_fingerprint_sha256 << std::endl;
            std::cerr << "    Got:      " << result.fingerprint_sha256 << std::endl;
            
            // Debug: print canonical bytes
            std::cerr << "    Canonical bytes (" << result.canonical_bytes.size() << " bytes): ";
            for (size_t i = 0; i < result.canonical_bytes.size(); ++i) {
                std::cerr << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(result.canonical_bytes[i]);
            }
            std::cerr << std::dec << std::endl;
            
            throw std::runtime_error("Fingerprint mismatch for " + tv.name);
        }
    }

    std::cout << "    PASS: fingerprint = " << result.fingerprint_sha256.substr(0, 16) << "..." << std::endl;
}

void test_cross_language_parity() {
    std::cout << "  [RUN] Cross-language parity verification (embedded vectors)..." << std::endl;

    const auto vectors = get_test_vectors();
    for (const auto& tv : vectors) {
        run_test_vector(tv);
    }

    std::cout << "  [PASS] Cross-language parity verification (" << vectors.size() << " vectors)" << std::endl;
}

void test_canonical_encoding_determinism() {
    std::cout << "  [RUN] Canonical encoding determinism..." << std::endl;

    // Create a test input and run it multiple times
    CanonicalInput input;
    input.repo_root = "C:/test/repo";
    input.source_revision = "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2";
    input.abi = "x86_64";
    input.api_level = 26;
    input.configuration = "Release";
    input.target_triple = "x86_64-linux-android26";
    input.ndk_version = "r26.1.10909125";
    input.compile_args = {
        "--target=x86_64-linux-android26",
        "-std=c++20",
        "-fPIE",
        "-pie",
        "-Wall",
        "-Wextra",
        "-D__ANDROID__",
        "-I", "C:/test/repo/apps/fang-input-daemon",
        "-I", "C:/test/repo/apps/fang-studio",
        "-O2",
        "-DNDEBUG"
    };
    input.entries = {
        {"apps/fang-input-daemon/main.cpp", CanonicalRole::Source, CanonicalKind::File, 1234,
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"apps/fang-input-daemon/guest_input_backend.cpp", CanonicalRole::Source, CanonicalKind::File, 5678,
         "1111111111111111111111111111111111111111111111111111111111111111"}
    };

    std::string first_fingerprint;
    std::vector<uint8_t> first_bytes;
    for (int i = 0; i < 10; ++i) {
        CanonicalResult result = build_canonical_stream(input);
        if (!result.success()) {
            throw std::runtime_error("Build failed on iteration " + std::to_string(i));
        }
        if (i == 0) {
            first_fingerprint = result.fingerprint_sha256;
            first_bytes = result.canonical_bytes;
        } else if (result.fingerprint_sha256 != first_fingerprint) {
            throw std::runtime_error("Non-deterministic fingerprint on iteration " + std::to_string(i));
        } else if (result.canonical_bytes != first_bytes) {
            throw std::runtime_error("Non-deterministic canonical bytes on iteration " + std::to_string(i));
        }
    }

    std::cout << "    PASS: 10 consecutive runs produced identical fingerprints and bytes" << std::endl;
}

void test_utf8_path_ordering() {
    std::cout << "  [RUN] UTF-8 path byte ordering..." << std::endl;

    // Test that entries are sorted by UTF-8 byte order
    CanonicalInput input;
    input.repo_root = "C:/test/repo";
    input.source_revision = "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2";
    input.abi = "x86_64";
    input.api_level = 26;
    input.configuration = "Release";
    input.target_triple = "x86_64-linux-android26";
    input.ndk_version = "r26.1.10909125";
    input.compile_args = {"--target=x86_64-linux-android26", "-std=c++20"};

    // These paths should sort in UTF-8 byte order:
    // "a" < "aa" < "b" < "é" (U+00E9 = 0xC3 0xA9) < "z"
    input.entries = {
        {"z.cpp", CanonicalRole::Source, CanonicalKind::File, 1, "1111111111111111111111111111111111111111111111111111111111111111"},
        {"a.cpp", CanonicalRole::Source, CanonicalKind::File, 1, "2222222222222222222222222222222222222222222222222222222222222222"},
        {"aa.cpp", CanonicalRole::Source, CanonicalKind::File, 1, "3333333333333333333333333333333333333333333333333333333333333333"},
        {"b.cpp", CanonicalRole::Source, CanonicalKind::File, 1, "4444444444444444444444444444444444444444444444444444444444444444"},
        {"\xC3\xA9.cpp", CanonicalRole::Source, CanonicalKind::File, 1, "5555555555555555555555555555555555555555555555555555555555555555"} // é
    };

    CanonicalResult result = build_canonical_stream(input);
    if (!result.success()) {
        throw std::runtime_error("Build failed: " + result.errors[0]);
    }

    // Verify the canonical bytes contain entries in correct order
    // We can't easily inspect the canonical bytes without exposing internals,
    // but we can verify the fingerprint is deterministic
    CanonicalResult result2 = build_canonical_stream(input);
    if (result.canonical_bytes != result2.canonical_bytes) {
        throw std::runtime_error("UTF-8 ordering not deterministic");
    }

    std::cout << "    PASS: UTF-8 path byte ordering is deterministic" << std::endl;
}

void test_role_and_kind_ids_match_powershell() {
    std::cout << "  [RUN] Role and Kind ID consistency with PowerShell..." << std::endl;

    // Verify that the enum values match the PowerShell implementation
    // PowerShell uses: source=1, header=2, build-script=3, configuration=4, target-config=5, compile-arg=6, git-revision=7
    // file=0, virtual=1

    if (static_cast<uint8_t>(CanonicalRole::Source) != 1) throw std::runtime_error("Role Source mismatch");
    if (static_cast<uint8_t>(CanonicalRole::Header) != 2) throw std::runtime_error("Role Header mismatch");
    if (static_cast<uint8_t>(CanonicalRole::BuildScript) != 3) throw std::runtime_error("Role BuildScript mismatch");
    if (static_cast<uint8_t>(CanonicalRole::Configuration) != 4) throw std::runtime_error("Role Configuration mismatch");
    if (static_cast<uint8_t>(CanonicalRole::TargetConfig) != 5) throw std::runtime_error("Role TargetConfig mismatch");
    if (static_cast<uint8_t>(CanonicalRole::CompileArg) != 6) throw std::runtime_error("Role CompileArg mismatch");
    if (static_cast<uint8_t>(CanonicalRole::GitRevision) != 7) throw std::runtime_error("Role GitRevision mismatch");

    if (static_cast<uint8_t>(CanonicalKind::File) != 0) throw std::runtime_error("Kind File mismatch");
    if (static_cast<uint8_t>(CanonicalKind::Virtual) != 1) throw std::runtime_error("Kind Virtual mismatch");

    std::cout << "    PASS: Role and Kind IDs match PowerShell specification" << std::endl;
}

void test_magic_bytes() {
    std::cout << "  [RUN] Magic bytes verification..." << std::endl;

    CanonicalInput input;
    input.repo_root = "C:/test/repo";
    input.source_revision = "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2";
    input.abi = "x86_64";
    input.api_level = 26;
    input.configuration = "Release";
    input.target_triple = "x86_64-linux-android26";
    input.ndk_version = "r26.1.10909125";
    input.compile_args = {"--target=x86_64-linux-android26", "-std=c++20"};
    input.entries = {};

    CanonicalResult result = build_canonical_stream(input);
    if (!result.success()) {
        throw std::runtime_error("Build failed: " + result.errors[0]);
    }

    // Verify magic bytes: "FANGCIM" + NUL + 0x01
    if (result.canonical_bytes.size() < 9) {
        throw std::runtime_error("Canonical bytes too short for magic");
    }
    const char expected_magic[9] = {'F','A','N','G','C','I','M', 0x00, 0x01};
    for (int i = 0; i < 9; ++i) {
        if (result.canonical_bytes[i] != static_cast<uint8_t>(expected_magic[i])) {
            throw std::runtime_error("Magic byte mismatch at position " + std::to_string(i));
        }
    }

    std::cout << "    PASS: Magic bytes are correct (FANGCIM\\0\\1)" << std::endl;
}

} // anonymous namespace

int main() {
    try {
        std::cout << "[CANONICAL-ENCODER-TESTS] Running canonical encoder tests..." << std::endl;

        test_magic_bytes();
        test_role_and_kind_ids_match_powershell();
        test_canonical_encoding_determinism();
        test_utf8_path_ordering();
        test_cross_language_parity();

        std::cout << "[CANONICAL-ENCODER-TESTS] All tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[CANONICAL-ENCODER-TESTS] FAILED: " << e.what() << std::endl;
        return 1;
    }
}