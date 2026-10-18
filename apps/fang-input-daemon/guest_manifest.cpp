#include "guest_manifest.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fang::guest {

GuestTargetAbi parse_guest_target_abi(std::string_view name) noexcept {
    if (name == "x86_64") return GuestTargetAbi::X86_64;
    if (name == "arm64-v8a") return GuestTargetAbi::Arm64_v8a;
    if (name == "x86") return GuestTargetAbi::X86;
    if (name == "armeabi-v7a") return GuestTargetAbi::Armeabi_v7a;
    if (name == "host-native") return GuestTargetAbi::HostNative;
    return GuestTargetAbi::Unknown;
}

std::string GuestArtifactManifest::to_json() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"artifact_name\": \"" << artifact_name << "\",\n";
    ss << "  \"artifact_format\": \"" << artifact_format << "\",\n";
    ss << "  \"target_os\": \"" << target_os << "\",\n";
    ss << "  \"target_abi\": \"" << target_abi << "\",\n";
    ss << "  \"protocol_version\": " << protocol_version << ",\n";
    ss << "  \"compiler_id\": \"" << compiler_id << "\",\n";
    ss << "  \"build_configuration\": \"" << build_configuration << "\",\n";
    ss << "  \"backend_capability\": \"" << backend_capability << "\",\n";
    ss << "  \"privileged_backend_enabled\": " << (privileged_backend_enabled ? "true" : "false") << ",\n";
    ss << "  \"is_cross_compiled\": " << (is_cross_compiled ? "true" : "false") << ",\n";
    ss << "  \"is_android_elf_binary\": " << (is_android_elf_binary ? "true" : "false") << ",\n";
    ss << "  \"toolchain_status\": \"" << toolchain_status_name(toolchain_status) << "\",\n";
    ss << "  \"build_rule_version\": \"" << build_rule_version << "\",\n";
    ss << "  \"manifest_schema_version\": \"" << manifest_schema_version << "\",\n";
    ss << "  \"source_revision\": \"" << source_revision << "\",\n";
    ss << "  \"source_tree_state\": \"" << source_tree_state << "\",\n";
    ss << "  \"source_tree_dirty_files\": [";
    for (size_t i = 0; i < source_tree_dirty_files.size(); ++i) {
        ss << "\"" << source_tree_dirty_files[i] << "\"";
        if (i + 1 < source_tree_dirty_files.size()) ss << ", ";
    }
    ss << "],\n";
    ss << "  \"source_evidence\": {\n";
    ss << "    \"status\": \"" << (source_evidence.is_collected() ? "collected" : "unavailable") << "\",\n";
    ss << "    \"algorithm\": \"" << source_evidence.algorithm << "\",\n";
    ss << "    \"fingerprint\": \"" << source_evidence.fingerprint << "\",\n";
    ss << "    \"scope\": \"" << source_evidence.scope << "\",\n";
    ss << "    \"encoding\": \"" << source_evidence.encoding << "\",\n";
    ss << "    \"captured_at_utc\": \"" << source_evidence.captured_at_utc << "\",\n";
    ss << "    \"unavailable_reasons\": [";
    for (size_t i = 0; i < source_evidence.unavailable_reasons.size(); ++i) {
        ss << "\"" << source_evidence.unavailable_reasons[i] << "\"";
        if (i + 1 < source_evidence.unavailable_reasons.size()) ss << ", ";
    }
    ss << "]\n";
    ss << "  },\n";
    ss << "  \"provenance_collected\": [";
    for (size_t i = 0; i < provenance_collected.size(); ++i) {
        ss << "\"" << provenance_collected[i] << "\"";
        if (i + 1 < provenance_collected.size()) ss << ", ";
    }
    ss << "],\n";
    ss << "  \"provenance_unavailable\": [";
    for (size_t i = 0; i < provenance_unavailable.size(); ++i) {
        ss << "\"" << provenance_unavailable[i] << "\"";
        if (i + 1 < provenance_unavailable.size()) ss << ", ";
    }
    ss << "],\n";
    ss << "  \"artifact_path\": \"" << artifact_path << "\",\n";
    ss << "  \"android_api_level\": " << android_api_level << ",\n";
    ss << "  \"ndk_version\": \"" << ndk_version << "\",\n";
    ss << "  \"artifact_size_bytes\": " << artifact_size_bytes << ",\n";
    ss << "  \"artifact_sha256\": \"" << artifact_sha256 << "\",\n";
    ss << "  \"build_result\": \"" << build_result << "\",\n";
    ss << "  \"elf_machine\": \"" << elf_machine << "\",\n";
    ss << "  \"elf_class\": " << static_cast<unsigned>(elf_class) << ",\n";
    ss << "  \"elf_data\": " << static_cast<unsigned>(elf_data) << ",\n";
    ss << "  \"missing_prerequisites\": [";
    for (size_t i = 0; i < missing_prerequisites.size(); ++i) {
        ss << "\"" << missing_prerequisites[i] << "\"";
        if (i + 1 < missing_prerequisites.size()) {
            ss << ", ";
        }
    }
    ss << "]\n";
    ss << "}\n";
    return ss.str();
}

static std::string extract_json_string_value(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\":";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '\"') return {};
    ++pos;
    auto end_pos = json.find('\"', pos);
    if (end_pos == std::string_view::npos) return {};
    return std::string(json.substr(pos, end_pos - pos));
}

static std::string extract_json_value_from_object(std::string_view object_json, std::string_view key) {
    // Extract a value from a nested JSON object
    std::string needle = "\"" + std::string(key) + "\":";
    auto pos = object_json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    while (pos < object_json.size() && (object_json[pos] == ' ' || object_json[pos] == '\t' || object_json[pos] == '\r' || object_json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= object_json.size()) return {};
    if (object_json[pos] == '\"') {
        // String value
        ++pos;
        auto end_pos = object_json.find('\"', pos);
        if (end_pos == std::string_view::npos) return {};
        return std::string(object_json.substr(pos, end_pos - pos));
    } else if (object_json[pos] == '[') {
        // Array value - not handled for simple extraction
        return {};
    }
    return {};
}

static std::string extract_json_object(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\":";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '{') return {};
    int depth = 1;
    size_t start = pos;
    ++pos;
    while (pos < json.size() && depth > 0) {
        if (json[pos] == '{') depth++;
        else if (json[pos] == '}') depth--;
        ++pos;
    }
    if (depth == 0) {
        return std::string(json.substr(start, pos - start));
    }
    return {};
}

static bool extract_json_bool_value(std::string_view json, std::string_view key, bool default_val = false) {
    std::string needle = "\"" + std::string(key) + "\":";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return default_val;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (json.substr(pos, 4) == "true") return true;
    if (json.substr(pos, 5) == "false") return false;
    return default_val;
}

static uint64_t extract_json_uint_value(std::string_view json, std::string_view key, uint64_t default_val = 0) {
    std::string needle = "\"" + std::string(key) + "\":";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return default_val;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    uint64_t val = 0;
    bool found_digit = false;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        found_digit = true;
        val = val * 10 + (json[pos] - '0');
        ++pos;
    }
    return found_digit ? val : default_val;
}

bool GuestArtifactManifest::from_json(
    std::string_view json, GuestArtifactManifest& out_manifest, std::string* error) {
    if (json.empty() || json.front() != '{') {
        if (error) *error = "Invalid JSON: missing root object";
        return false;
    }

    out_manifest.artifact_name = extract_json_string_value(json, "artifact_name");
    out_manifest.artifact_format = extract_json_string_value(json, "artifact_format");
    out_manifest.target_os = extract_json_string_value(json, "target_os");
    out_manifest.target_abi = extract_json_string_value(json, "target_abi");
    out_manifest.protocol_version = static_cast<uint16_t>(extract_json_uint_value(json, "protocol_version", 1));
    out_manifest.compiler_id = extract_json_string_value(json, "compiler_id");
    out_manifest.build_configuration = extract_json_string_value(json, "build_configuration");
    out_manifest.backend_capability = extract_json_string_value(json, "backend_capability");
    out_manifest.privileged_backend_enabled = extract_json_bool_value(json, "privileged_backend_enabled", false);
    out_manifest.is_cross_compiled = extract_json_bool_value(json, "is_cross_compiled", false);
    out_manifest.is_android_elf_binary = extract_json_bool_value(json, "is_android_elf_binary", false);
    out_manifest.build_rule_version = extract_json_string_value(json, "build_rule_version");
    out_manifest.manifest_schema_version = extract_json_string_value(json, "manifest_schema_version");
    if (out_manifest.manifest_schema_version.empty()) out_manifest.manifest_schema_version = out_manifest.build_rule_version;
    out_manifest.source_revision = extract_json_string_value(json, "source_revision");
    out_manifest.source_tree_state = extract_json_string_value(json, "source_tree_state");
    if (out_manifest.source_tree_state.empty()) out_manifest.source_tree_state = "unavailable";
    if (out_manifest.build_rule_version.empty()) out_manifest.build_rule_version = "stage9-v1";

    // Parse source_evidence object
    std::string source_evidence_json = extract_json_object(json, "source_evidence");
    if (!source_evidence_json.empty()) {
        out_manifest.source_evidence.algorithm = extract_json_value_from_object(source_evidence_json, "algorithm");
        out_manifest.source_evidence.fingerprint = extract_json_value_from_object(source_evidence_json, "fingerprint");
        out_manifest.source_evidence.scope = extract_json_value_from_object(source_evidence_json, "scope");
        out_manifest.source_evidence.encoding = extract_json_value_from_object(source_evidence_json, "encoding");
        out_manifest.source_evidence.captured_at_utc = extract_json_value_from_object(source_evidence_json, "captured_at_utc");
        std::string status_str = extract_json_value_from_object(source_evidence_json, "status");
        if (status_str == "collected") {
            out_manifest.source_evidence.status = SourceEvidence::Status::Collected;
        } else {
            out_manifest.source_evidence.status = SourceEvidence::Status::Unavailable;
        }
        // Parse unavailable_reasons array
        out_manifest.source_evidence.unavailable_reasons.clear();
        std::string needle = "\"unavailable_reasons\":";
        auto pos = source_evidence_json.find(needle);
        if (pos != std::string_view::npos) {
            auto bracket_start = source_evidence_json.find('[', pos);
            auto bracket_end = source_evidence_json.find(']', bracket_start);
            if (bracket_start != std::string_view::npos && bracket_end != std::string_view::npos) {
                std::string_view arr = source_evidence_json.substr(bracket_start + 1, bracket_end - bracket_start - 1);
                size_t idx = 0;
                while (idx < arr.size()) {
                    auto q1 = arr.find('\"', idx);
                    if (q1 == std::string_view::npos) break;
                    auto q2 = arr.find('\"', q1 + 1);
                    if (q2 == std::string_view::npos) break;
                    out_manifest.source_evidence.unavailable_reasons.push_back(std::string(arr.substr(q1 + 1, q2 - q1 - 1)));
                    idx = q2 + 1;
                }
            }
        }
    }

    auto parse_string_array = [&](std::string_view key, std::vector<std::string>& out) {
        out.clear();
        std::string needle = "\"" + std::string(key) + "\":";
        auto pos = json.find(needle);
        if (pos == std::string_view::npos) return;
        auto bracket_start = json.find('[', pos);
        auto bracket_end = json.find(']', bracket_start);
        if (bracket_start == std::string_view::npos || bracket_end == std::string_view::npos) return;
        std::string_view arr = json.substr(bracket_start + 1, bracket_end - bracket_start - 1);
        size_t idx = 0;
        while (idx < arr.size()) {
            auto q1 = arr.find('\"', idx);
            if (q1 == std::string_view::npos) break;
            auto q2 = arr.find('\"', q1 + 1);
            if (q2 == std::string_view::npos) break;
            out.push_back(std::string(arr.substr(q1 + 1, q2 - q1 - 1)));
            idx = q2 + 1;
        }
    };
    parse_string_array("source_tree_dirty_files", out_manifest.source_tree_dirty_files);
    parse_string_array("provenance_collected", out_manifest.provenance_collected);
    parse_string_array("provenance_unavailable", out_manifest.provenance_unavailable);
    out_manifest.artifact_path = extract_json_string_value(json, "artifact_path");
    out_manifest.android_api_level = static_cast<uint32_t>(extract_json_uint_value(json, "android_api_level", 26));
    out_manifest.ndk_version = extract_json_string_value(json, "ndk_version");
    out_manifest.artifact_size_bytes = extract_json_uint_value(json, "artifact_size_bytes", 0);
    out_manifest.artifact_sha256 = extract_json_string_value(json, "artifact_sha256");
    out_manifest.build_result = extract_json_string_value(json, "build_result");
    if (out_manifest.build_result.empty()) out_manifest.build_result = "unbuilt";
    out_manifest.elf_machine = extract_json_string_value(json, "elf_machine");
    out_manifest.elf_class = static_cast<uint8_t>(extract_json_uint_value(json, "elf_class", 0));
    out_manifest.elf_data = static_cast<uint8_t>(extract_json_uint_value(json, "elf_data", 0));

    std::string status_str = extract_json_string_value(json, "toolchain_status");
    if (status_str == "ToolchainAvailable") {
        out_manifest.toolchain_status = ToolchainStatus::ToolchainAvailable;
    } else if (status_str == "MissingNdk") {
        out_manifest.toolchain_status = ToolchainStatus::MissingNdk;
    } else if (status_str == "MissingClang") {
        out_manifest.toolchain_status = ToolchainStatus::MissingClang;
    } else {
        out_manifest.toolchain_status = ToolchainStatus::UnsupportedHost;
    }

    // Extract missing prerequisites array
    out_manifest.missing_prerequisites.clear();
    std::string_view needle = "\"missing_prerequisites\":";
    auto pos = json.find(needle);
    if (pos != std::string_view::npos) {
        auto bracket_start = json.find('[', pos);
        auto bracket_end = json.find(']', bracket_start);
        if (bracket_start != std::string_view::npos && bracket_end != std::string_view::npos) {
            std::string_view arr = json.substr(bracket_start + 1, bracket_end - bracket_start - 1);
            size_t idx = 0;
            while (idx < arr.size()) {
                auto q1 = arr.find('\"', idx);
                if (q1 == std::string_view::npos) break;
                auto q2 = arr.find('\"', q1 + 1);
                if (q2 == std::string_view::npos) break;
                out_manifest.missing_prerequisites.push_back(std::string(arr.substr(q1 + 1, q2 - q1 - 1)));
                idx = q2 + 1;
            }
        }
    }

    return out_manifest.validate(error);
}

bool GuestArtifactManifest::validate(std::string* error) const {
    if (artifact_name.empty()) {
        if (error) *error = "Artifact name cannot be empty";
        return false;
    }
    if (target_os.empty()) {
        if (error) *error = "Target OS cannot be empty";
        return false;
    }
    if (target_abi.empty()) {
        if (error) *error = "Target ABI cannot be empty";
        return false;
    }
    if (protocol_version == 0) {
        if (error) *error = "Protocol version must be greater than zero";
        return false;
    }

    // Invariant: Never claim a Windows PE executable is an Android guest binary
    if (artifact_format == "PE-COFF" && is_android_elf_binary) {
        if (error) *error = "Invalid manifest invariant: PE-COFF artifact format cannot claim to be an Android ELF binary";
        return false;
    }
    if (target_os == "windows" && is_android_elf_binary) {
        if (error) *error = "Invalid manifest invariant: Windows target cannot claim to be an Android ELF binary";
        return false;
    }
    if (is_android_elf_binary && artifact_format != "ELF") {
        if (error) *error = "Invalid manifest invariant: Android guest binary must specify ELF artifact format";
        return false;
    }
    if (is_android_elf_binary && toolchain_status != ToolchainStatus::ToolchainAvailable) {
        if (error) *error = "Invalid manifest invariant: Android ELF binary cannot be marked complete when toolchain is missing";
        return false;
    }

    return true;
}

GuestToolchainDetector::EnvironmentReport GuestToolchainDetector::inspect_ndk_path(
    const std::string& custom_ndk_path, uint32_t min_version) {
    EnvironmentReport report;
    if (custom_ndk_path.empty()) {
        report.missing_tools.push_back("NDK path is empty");
        return report;
    }

    std::error_code ec;
    std::filesystem::path ndk_path(custom_ndk_path);
    if (!std::filesystem::exists(ndk_path, ec) || !std::filesystem::is_directory(ndk_path, ec)) {
        report.missing_tools.push_back("NDK path does not exist or is not a directory: " + custom_ndk_path);
        return report;
    }

    report.ndk_path = std::filesystem::weakly_canonical(ndk_path, ec).string();

    // Parse source.properties for NDK version
    std::filesystem::path source_props = ndk_path / "source.properties";
    if (std::filesystem::exists(source_props, ec)) {
        std::ifstream f(source_props);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("Pkg.Revision") != std::string::npos) {
                auto eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string ver = line.substr(eq + 1);
                    while (!ver.empty() && (ver.front() == ' ' || ver.front() == '\t')) ver.erase(ver.begin());
                    while (!ver.empty() && (ver.back() == ' ' || ver.back() == '\t' || ver.back() == '\r' || ver.back() == '\n')) ver.pop_back();
                    report.ndk_version = ver;
                    // Parse major version
                    uint32_t maj = 0;
                    for (char c : ver) {
                        if (c >= '0' && c <= '9') {
                            maj = maj * 10 + (c - '0');
                        } else if (c == '.') {
                            break;
                        }
                    }
                    report.ndk_major_version = maj;
                    if (maj >= min_version) {
                        report.ndk_found = true;
                        report.version_supported = true;
                    } else {
                        report.ndk_found = false;
                        report.version_supported = false;
                        report.diagnostic = "Android NDK version " + ver + " is below minimum supported version r" + std::to_string(min_version);
                        report.missing_tools.push_back(report.diagnostic);
                        return report;
                    }
                }
            }
        }
    } else {
        report.ndk_found = false;
        report.ndk_version = "unknown";
        report.version_supported = false;
        report.diagnostic = "NDK source.properties not found in: " + custom_ndk_path;
        report.missing_tools.push_back(report.diagnostic);
        return report;
    }

    // Locate LLVM prebuilt toolchain clang
    std::filesystem::path llvm_bin = ndk_path / "toolchains" / "llvm" / "prebuilt";
    if (std::filesystem::exists(llvm_bin, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(llvm_bin, ec)) {
            if (entry.is_directory(ec)) {
                std::filesystem::path clang_candidate = entry.path() / "bin" / "clang++.exe";
                if (!std::filesystem::exists(clang_candidate, ec)) {
                    clang_candidate = entry.path() / "bin" / "clang.exe";
                }
                if (!std::filesystem::exists(clang_candidate, ec)) {
                    clang_candidate = entry.path() / "bin" / "clang++";
                }
                if (!std::filesystem::exists(clang_candidate, ec)) {
                    clang_candidate = entry.path() / "bin" / "clang";
                }
                if (std::filesystem::exists(clang_candidate, ec)) {
                    report.clang_found = true;
                    report.clang_path = clang_candidate.string();
                    break;
                }
            }
        }
    }

    if (!report.clang_found) {
        report.diagnostic = "Android NDK LLVM Clang toolchain not found under: " + (ndk_path / "toolchains" / "llvm" / "prebuilt").string();
        report.missing_tools.push_back(report.diagnostic);
    } else {
        report.diagnostic = "Android NDK toolchain ready: version r" + report.ndk_version + " with clang++ at " + report.clang_path;
    }

    return report;
}

GuestToolchainDetector::EnvironmentReport GuestToolchainDetector::inspect_environment(uint32_t min_version) {
    // Check environment variables for Android NDK
    const char* ndk_home = std::getenv("ANDROID_NDK_ROOT");
    if (!ndk_home) ndk_home = std::getenv("ANDROID_NDK_HOME");
    if (!ndk_home) ndk_home = std::getenv("NDK_ROOT");

    std::filesystem::path detected_ndk;
    std::error_code ec;
    if (ndk_home && std::filesystem::exists(ndk_home, ec)) {
        detected_ndk = ndk_home;
    } else {
        // Check default SDK path: %LOCALAPPDATA%\Android\Sdk\ndk
        const char* local_app_data = std::getenv("LOCALAPPDATA");
        if (local_app_data) {
            std::filesystem::path sdk_ndk = std::filesystem::path(local_app_data) / "Android" / "Sdk" / "ndk";
            if (std::filesystem::exists(sdk_ndk, ec) && !std::filesystem::is_empty(sdk_ndk, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(sdk_ndk, ec)) {
                    if (entry.is_directory(ec)) {
                        detected_ndk = entry.path();
                        break;
                    }
                }
            }
        }
    }

    if (!detected_ndk.empty() && std::filesystem::exists(detected_ndk, ec)) {
        return inspect_ndk_path(detected_ndk.string(), min_version);
    }

    EnvironmentReport report;
    report.missing_tools.push_back("Android NDK r" + std::to_string(min_version) + "+ (ANDROID_NDK_ROOT, ANDROID_NDK_HOME, or %LOCALAPPDATA%\\Android\\Sdk\\ndk)");
    return report;
}

GuestArtifactManifest GuestToolchainDetector::generate_manifest_for_target(
    GuestTargetAbi abi,
    const std::string& build_config,
    const std::string& backend_capability,
    bool privileged_backend) {
    GuestArtifactManifest manifest;
    manifest.protocol_version = 1;
    manifest.build_configuration = build_config.empty() ? "Release" : build_config;
    manifest.backend_capability = backend_capability.empty() ? "RecordingStub" : backend_capability;
    manifest.privileged_backend_enabled = privileged_backend;
    manifest.build_rule_version = "stage9-v1";
    manifest.android_api_level = 26;

    if (abi == GuestTargetAbi::HostNative) {
#if defined(_WIN32)
        manifest.target_os = "windows";
        manifest.target_abi = "x86_64";
        manifest.artifact_name = "fang-input-daemon.exe";
        manifest.artifact_format = "PE-COFF";
        manifest.compiler_id = "MSVC / Clang-cl (Host Native)";
#else
        manifest.target_os = "linux";
        manifest.target_abi = "x86_64";
        manifest.artifact_name = "fang-input-daemon";
        manifest.artifact_format = "ELF";
        manifest.compiler_id = "Host Clang/GCC";
#endif
        manifest.is_cross_compiled = false;
        manifest.is_android_elf_binary = false;
        manifest.toolchain_status = ToolchainStatus::ToolchainAvailable;
        manifest.build_result = "success";
        return manifest;
    }

    // Android target requested (e.g. x86_64 or arm64-v8a)
    std::string abi_str = std::string(guest_target_abi_name(abi));
    manifest.target_os = "android";
    manifest.target_abi = abi_str;
    manifest.artifact_name = "fang-input-daemon-android-" + abi_str;

    auto report = inspect_environment(25);
    if (report.ndk_found && report.version_supported && report.clang_found) {
        manifest.artifact_format = "ELF";
        manifest.compiler_id = "Android NDK Clang r" + std::to_string(report.ndk_major_version) + " (" + report.clang_path + ")";
        manifest.ndk_version = report.ndk_version;
        manifest.is_cross_compiled = true;
        manifest.is_android_elf_binary = true;
        manifest.toolchain_status = ToolchainStatus::ToolchainAvailable;
        manifest.build_result = "success";
    } else {
        manifest.artifact_format = "Unavailable (Missing Toolchain)";
        manifest.compiler_id = "None";
        manifest.is_cross_compiled = false;
        manifest.is_android_elf_binary = false;
        manifest.toolchain_status = report.ndk_found ? ToolchainStatus::MissingClang : ToolchainStatus::MissingNdk;
        manifest.missing_prerequisites = report.missing_tools;
        manifest.build_result = "unbuilt";
    }

    return manifest;
}

} // namespace fang::guest
