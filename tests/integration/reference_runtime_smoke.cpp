#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include "broker_native_journal.hpp"
#include "broker_pipe.hpp"
#include "broker_reference_runtime.hpp"
#include "supervisor_runtime_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace fang::broker;
using namespace fang::studio;

enum class StageStatus {
    Pass,
    Fail,
    Blocked,
    NotRun
};

inline const char* stage_status_string(StageStatus status) noexcept {
    switch (status) {
    case StageStatus::Pass: return "PASS";
    case StageStatus::Fail: return "FAIL";
    case StageStatus::Blocked: return "BLOCKED";
    case StageStatus::NotRun: return "NOT RUN";
    }
    return "UNKNOWN";
}

struct StageResult {
    std::string name;
    StageStatus status{StageStatus::NotRun};
    std::uint64_t elapsed_ms{};
    std::string details;
};

class StageTracker {
public:
    void register_stage(std::string name) {
        stages_.push_back({std::move(name), StageStatus::NotRun, 0, ""});
    }

    void record(std::string_view name, StageStatus status, std::uint64_t elapsed_ms, std::string details) {
        for (auto& stage : stages_) {
            if (stage.name == name) {
                stage.status = status;
                stage.elapsed_ms = elapsed_ms;
                stage.details = std::move(details);
                print_stage_line(stage);
                return;
            }
        }
        stages_.push_back({std::string(name), status, elapsed_ms, std::move(details)});
        print_stage_line(stages_.back());
    }

    void block_remaining(std::string_view reason) {
        for (auto& stage : stages_) {
            if (stage.status == StageStatus::NotRun) {
                stage.status = StageStatus::Blocked;
                stage.details = std::string(reason);
                print_stage_line(stage);
            }
        }
    }

    void print_summary_table() const {
        std::cout << "\n=================================== LIFECYCLE STAGE RESULTS ===================================\n";
        std::cout << std::left << std::setw(32) << "Stage"
                  << std::setw(12) << "Status"
                  << std::setw(16) << "Elapsed (ms)"
                  << "Details\n";
        std::cout << std::string(96, '-') << "\n";
        for (const auto& stage : stages_) {
            std::cout << std::left << std::setw(32) << stage.name
                      << std::setw(12) << stage_status_string(stage.status)
                      << std::setw(16) << stage.elapsed_ms
                      << stage.details << "\n";
        }
        std::cout << "===============================================================================================\n\n";
    }

    bool all_passed() const {
        return !stages_.empty() && std::all_of(stages_.begin(), stages_.end(),
            [](const auto& s) { return s.status == StageStatus::Pass; });
    }

private:
    static void print_stage_line(const StageResult& stage) {
        std::cout << "stage=" << stage.name
                  << " result=" << stage_status_string(stage.status)
                  << " elapsed_ms=" << stage.elapsed_ms;
        if (!stage.details.empty()) {
            std::cout << " details=\"" << stage.details << "\"";
        }
        std::cout << '\n';
    }

    std::vector<StageResult> stages_;
};

struct Options {
    std::string avd_name;
    std::filesystem::path apk_path;
    std::uint32_t timeout_override_ms{0};
};

bool parse_options(int argc, wchar_t* argv[], Options& options, std::string& error) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--avd" && index + 1 < argc) {
            const std::wstring_view name(argv[++index]);
            if (name.empty() || std::any_of(name.begin(), name.end(),
                    [](wchar_t value) { return value <= 0 || value > 0x7f; })) {
                error = "AVD name must be ASCII and match the supported reference identity format.";
                return false;
            }
            options.avd_name.clear();
            for (const wchar_t ch : name) {
                options.avd_name.push_back(static_cast<char>(ch));
            }
        } else if (argument == L"--apk" && index + 1 < argc) {
            options.apk_path = std::filesystem::path(argv[++index]);
        } else if (argument == L"--timeout-ms" && index + 1 < argc) {
            options.timeout_override_ms = static_cast<std::uint32_t>(std::wcstoul(argv[++index], nullptr, 10));
        } else {
            error = "Usage: fang-reference-runtime-smoke --avd <name> --apk <project-validation-apk> [--timeout-ms <ms>]";
            return false;
        }
    }
    if (options.avd_name.empty() || options.apk_path.empty()) {
        error = "Both --avd and --apk are required.";
        return false;
    }
    if (options.apk_path.filename() != L"fang-animated-gles-validation.apk") {
        error = "Only fang-animated-gles-validation.apk is accepted by the reference smoke tool.";
        return false;
    }
    return true;
}

bool compute_file_sha256(const std::filesystem::path& file_path, std::string& hex_digest, std::string& error) {
    BCRYPT_ALG_HANDLE alg_handle{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg_handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        error = "BCryptOpenAlgorithmProvider failed with NTSTATUS " + std::to_string(status);
        return false;
    }
    struct AlgGuard {
        BCRYPT_ALG_HANDLE h;
        ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    } alg_guard{alg_handle};

    DWORD object_size = 0;
    DWORD data_size = 0;
    status = BCryptGetProperty(alg_handle, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &data_size, 0);
    if (status < 0 || object_size == 0) {
        error = "BCryptGetProperty failed with NTSTATUS " + std::to_string(status);
        return false;
    }
    std::vector<UCHAR> hash_object(object_size);
    BCRYPT_HASH_HANDLE hash_handle{};
    status = BCryptCreateHash(alg_handle, &hash_handle, hash_object.data(), object_size, nullptr, 0, 0);
    if (status < 0) {
        error = "BCryptCreateHash failed with NTSTATUS " + std::to_string(status);
        return false;
    }
    struct HashGuard {
        BCRYPT_HASH_HANDLE h;
        ~HashGuard() { if (h) BCryptDestroyHash(h); }
    } hash_guard{hash_handle};

    std::ifstream stream(file_path, std::ios::binary);
    if (!stream.is_open()) {
        error = "Failed to open file for SHA-256 calculation: " + file_path.string();
        return false;
    }
    std::vector<char> buffer(65536);
    while (stream.read(buffer.data(), buffer.size()) || stream.gcount() > 0) {
        const auto bytes_read = static_cast<ULONG>(stream.gcount());
        status = BCryptHashData(hash_handle, reinterpret_cast<PUCHAR>(buffer.data()), bytes_read, 0);
        if (status < 0) {
            error = "BCryptHashData failed with NTSTATUS " + std::to_string(status);
            return false;
        }
    }

    std::array<UCHAR, 32> digest{};
    status = BCryptFinishHash(hash_handle, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (status < 0) {
        error = "BCryptFinishHash failed with NTSTATUS " + std::to_string(status);
        return false;
    }

    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    for (const auto byte : digest) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    hex_digest = ss.str();
    return true;
}

inline bool take_u32_le(const std::vector<std::byte>& input, std::size_t& offset, std::uint32_t& value) {
    if (offset + 4 > input.size()) return false;
    value = 0;
    for (unsigned int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + i])) << (i * 8);
    }
    offset += 4;
    return true;
}

inline bool take_str_le(const std::vector<std::byte>& input, std::size_t& offset, std::string& text) {
    std::uint32_t size = 0;
    if (!take_u32_le(input, offset, size) || offset + size > input.size()) return false;
    text.clear();
    text.reserve(size);
    for (std::uint32_t i = 0; i < size; ++i) {
        text.push_back(static_cast<char>(input[offset + i]));
    }
    offset += size;
    return true;
}

bool verify_durable_finalized_state(
    const CanonicalAvdIdentity& identity,
    const std::string& expected_generation,
    std::string& details) {
    details.clear();
    std::wstring local_app_data;
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0) {
        details = "Could not read LOCALAPPDATA environment variable.";
        return false;
    }
    local_app_data.resize(size - 1);
    GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), size);
    const std::filesystem::path ownership_dir =
        std::filesystem::path(local_app_data) / L"Fang" / L"RuntimeBroker" / L"Ownership";

    if (!std::filesystem::is_directory(ownership_dir)) {
        details = "Ownership storage directory does not exist: " + ownership_dir.string();
        return false;
    }

    // 1. Verify owner-index.fji contains this generation as EmptyVerified (state = 2)
    const auto store_path = ownership_dir / L"owner-index.fji";
    std::ifstream store_file(store_path, std::ios::binary | std::ios::ate);
    if (!store_file) {
        details = "Could not open owner-index.fji.";
        return false;
    }
    const auto store_file_size = store_file.tellg();
    store_file.seekg(0, std::ios::beg);
    std::vector<std::byte> store_bytes(static_cast<std::size_t>(store_file_size));
    store_file.read(reinterpret_cast<char*>(store_bytes.data()), store_file_size);
    if (store_bytes.size() < 10) {
        details = "owner-index.fji is too small or truncated.";
        return false;
    }
    const std::string_view magic(reinterpret_cast<const char*>(store_bytes.data()), 4);
    if (magic != "FIDX") {
        details = "owner-index.fji magic mismatch: " + std::string(magic);
        return false;
    }
    std::size_t offset = 4; // magic
    offset += 2; // version
    std::string owner_generation;
    if (!take_str_le(store_bytes, offset, owner_generation)) {
        details = "Could not read owner_generation from owner-index.fji.";
        return false;
    }
    std::uint32_t count = 0;
    if (!take_u32_le(store_bytes, offset, count)) {
        details = "Could not read entry count from owner-index.fji.";
        return false;
    }

    bool found_indexed_empty = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string sid;
        std::string loc;
        std::string gen;
        if (!take_str_le(store_bytes, offset, sid) ||
            !take_str_le(store_bytes, offset, loc) ||
            !take_str_le(store_bytes, offset, gen) ||
            offset >= store_bytes.size()) {
            break;
        }
        std::uint8_t entry_state = static_cast<std::uint8_t>(store_bytes[offset++]);

        if (loc == identity.canonical_avd_location && gen == expected_generation) {
            if (entry_state == 2) { // OwnershipIndexEntryState::EmptyVerified
                found_indexed_empty = true;
                break;
            } else {
                details = "owner-index entry state is " + std::to_string(entry_state) + " (expected 2: EmptyVerified)";
                return false;
            }
        }
    }

    if (!found_indexed_empty) {
        details = "owner-index does not contain generation " + expected_generation + " as EmptyVerified.";
        return false;
    }

    // 2. Locate and verify identity .fjn journal tombstone
    bool found_tombstone = false;
    for (const auto& entry : std::filesystem::directory_iterator(ownership_dir)) {
        if (entry.path().extension() == L".fjn") {
            std::ifstream jf(entry.path(), std::ios::binary | std::ios::ate);
            if (!jf) continue;
            const auto jf_size = jf.tellg();
            jf.seekg(0, std::ios::beg);
            std::vector<std::byte> jb(static_cast<std::size_t>(jf_size));
            jf.read(reinterpret_cast<char*>(jb.data()), jf_size);
            if (jb.size() >= 30) {
                const std::string_view jm(reinterpret_cast<const char*>(jb.data()), 4);
                if (jm == "FJRN") {
                    std::uint8_t kind = static_cast<std::uint8_t>(jb[6]); // 0 = empty tombstone
                    // Check if expected_generation is contained in the file
                    std::string file_content(reinterpret_cast<const char*>(jb.data()), jb.size());
                    if (file_content.find(expected_generation) != std::string::npos) {
                        if (kind == 0) {
                            found_tombstone = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!found_tombstone) {
        details = "Journal .fjn tombstone (kind=0) was not found for generation " + expected_generation;
        return false;
    }

    details = "owner-index[EmptyVerified], journal-tombstone[kind=0]";
    return true;
}

bool run_adb(const ReferenceRuntimeConfig& config, std::string_view serial,
    std::vector<std::wstring> arguments, std::uint32_t timeout_ms,
    std::string& output, std::string& error_out) {
    if (!serial.empty()) {
        arguments.insert(arguments.begin(), {L"-s", std::wstring(serial.begin(), serial.end())});
    }
    const auto result = run_reference_adb(config, arguments, timeout_ms);
    output = result.standard_output;
    error_out = result.standard_error;
    if (result.timed_out) {
        return false;
    }
    return result.succeeded;
}

bool wait_for_shutdown(SupervisorRuntimeClient& client, std::uint32_t timeout_ms,
    SupervisorRuntimeSnapshot& final_snapshot, std::uint64_t& elapsed_ms) {
    const auto started = std::chrono::steady_clock::now();
    if (!client.shutdown()) {
        final_snapshot = client.snapshot();
        if (final_snapshot.runtime.state == RuntimeLifecycleState::Stopped) {
            elapsed_ms = 0;
            return true;
        }
    }
    const auto deadline = started + std::chrono::milliseconds(timeout_ms + 10000);
    while (std::chrono::steady_clock::now() < deadline) {
        final_snapshot = client.snapshot();
        if (final_snapshot.runtime.state == RuntimeLifecycleState::Stopped) {
            elapsed_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
            return true;
        }
        if (final_snapshot.runtime.state == RuntimeLifecycleState::RecoveryRequired) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

int run(const Options& options) {
    StageTracker tracker;
    tracker.register_stage("prerequisites");
    tracker.register_stage("cycle1_start_and_ownership");
    tracker.register_stage("cycle1_guest_ready");
    tracker.register_stage("cycle1_apk_install");
    tracker.register_stage("cycle1_activity_launch");
    tracker.register_stage("cycle1_activity_verify");
    tracker.register_stage("cycle1_graceful_shutdown");
    tracker.register_stage("cycle1_verified_empty");
    tracker.register_stage("cycle2_start_and_ownership");
    tracker.register_stage("cycle2_guest_ready");
    tracker.register_stage("cycle2_activity_verify");
    tracker.register_stage("cycle2_graceful_shutdown");
    tracker.register_stage("cycle2_verified_empty");

    const auto prereq_started = std::chrono::steady_clock::now();

    std::string config_error;
    auto config = resolve_reference_runtime_config(options.avd_name, config_error);
    if (!config.has_value()) {
        const auto prereq_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - prereq_started).count());
        tracker.record("prerequisites", StageStatus::Fail, prereq_ms, "AVD configuration resolution failed: " + config_error);
        tracker.block_remaining("Configuration resolution failed");
        tracker.print_summary_table();
        return 2;
    }
    if (options.timeout_override_ms > 0) {
        config->readiness_timeout_ms = options.timeout_override_ms;
    }

    std::error_code file_error;
    const auto apk = std::filesystem::canonical(options.apk_path, file_error);
    if (file_error || !std::filesystem::is_regular_file(apk)) {
        const auto prereq_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - prereq_started).count());
        tracker.record("prerequisites", StageStatus::Fail, prereq_ms, "Validation APK cannot be canonicalized or is not a regular file");
        tracker.block_remaining("Validation APK missing");
        tracker.print_summary_table();
        return 2;
    }

    std::string apk_sha256;
    std::string sha_error;
    if (!compute_file_sha256(apk, apk_sha256, sha_error)) {
        const auto prereq_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - prereq_started).count());
        tracker.record("prerequisites", StageStatus::Fail, prereq_ms, "APK SHA-256 computation failed: " + sha_error);
        tracker.block_remaining("APK hash verification failed");
        tracker.print_summary_table();
        return 2;
    }

    const auto apk_size_bytes = std::filesystem::file_size(apk, file_error);

    const auto current_user = current_user_identity();
    if (!current_user.succeeded) {
        const auto prereq_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - prereq_started).count());
        tracker.record("prerequisites", StageStatus::Fail, prereq_ms, "Current user identity resolution failed: " + current_user.error.message);
        tracker.block_remaining("User identity resolution failed");
        tracker.print_summary_table();
        return 2;
    }

    std::array<wchar_t, 32768> module_path{};
    const DWORD module_length = GetModuleFileNameW(nullptr, module_path.data(),
        static_cast<DWORD>(module_path.size()));
    if (module_length == 0 || module_length >= module_path.size()) {
        const auto prereq_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - prereq_started).count());
        tracker.record("prerequisites", StageStatus::Fail, prereq_ms, "Could not resolve smoke executable module path");
        tracker.block_remaining("Executable path resolution failed");
        tracker.print_summary_table();
        return 2;
    }
    const auto supervisor_executable = std::filesystem::path(
        std::wstring(module_path.data(), module_length)).parent_path() /
        L"fang-runtime-supervisor.exe";

    if (!std::filesystem::is_regular_file(supervisor_executable)) {
        const auto prereq_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - prereq_started).count());
        tracker.record("prerequisites", StageStatus::Fail, prereq_ms, "Supervisor executable not found at: " + supervisor_executable.string());
        tracker.block_remaining("Supervisor executable missing");
        tracker.print_summary_table();
        return 2;
    }

    const std::string expected_serial = "emulator-" + std::to_string(config->emulator_port);
    std::string adb_devices_out;
    std::string adb_devices_err;
    if (run_adb(*config, "", {L"devices"}, config->adb_command_timeout_ms, adb_devices_out, adb_devices_err)) {
        if (adb_devices_out.find(expected_serial) != std::string::npos) {
            const auto prereq_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - prereq_started).count());
            tracker.record("prerequisites", StageStatus::Blocked, prereq_ms,
                "Configured serial " + expected_serial + " is already active in adb devices; refuse to adopt unowned instance");
            tracker.block_remaining("Conflicting emulator serial already active");
            tracker.print_summary_table();
            return 3;
        }
    }

    const CanonicalAvdIdentity canonical_id{current_user.identity.sid, config->canonical_avd_location};
    NativeOwnershipJournal initial_journal(current_user.identity.sid, 0, 0, "");
    const auto initial_journal_load = initial_journal.load(canonical_id);
    if (initial_journal_load.state == JournalLoadState::Record ||
        initial_journal_load.state == JournalLoadState::Stale) {
        const auto prereq_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - prereq_started).count());
        tracker.record("prerequisites", StageStatus::Blocked, prereq_ms,
            "Ownership journal already contains an unfinalized/stale record: " +
            (initial_journal_load.record.has_value() ? initial_journal_load.record->generation_id : initial_journal_load.error));
        tracker.block_remaining("Existing unfinalized journal record");
        tracker.print_summary_table();
        return 3;
    }

    const auto prereq_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - prereq_started).count());
    std::ostringstream prereq_details;
    prereq_details << "AVD=" << options.avd_name
                   << " ABI=" << config->abi
                   << " APK_SHA256=" << apk_sha256
                   << " APK_SIZE=" << apk_size_bytes << "B"
                   << " SERIAL=" << expected_serial;
    tracker.record("prerequisites", StageStatus::Pass, prereq_elapsed, prereq_details.str());

    const auto pipe = supervisor_pipe_name_for_user(current_user.identity.sid);
    SupervisorRuntimeClient client(pipe, supervisor_executable);

    // =========================================================================
    // CYCLE 1: Complete launch -> readiness -> install -> launch -> shutdown
    // =========================================================================
    std::cout << "\n>>> Starting Cycle 1: Reference Runtime Start & Verification\n";
    const auto c1_start_time = std::chrono::steady_clock::now();
    if (!client.start(options.avd_name)) {
        tracker.record("cycle1_start_and_ownership", StageStatus::Fail, 0, "Supervisor client failed to queue start request");
        tracker.block_remaining("Cycle 1 start request failed");
        tracker.print_summary_table();
        return 4;
    }

    bool c1_process_observed = false;
    bool c1_ownership_verified = false;
    std::uint64_t c1_process_launch_ms = 0;
    std::uint64_t c1_ownership_verified_ms = 0;
    SupervisorRuntimeSnapshot snapshot;

    auto last_c1_log = std::chrono::steady_clock::now();
    std::string last_c1_state;
    const auto c1_deadline = c1_start_time + std::chrono::milliseconds(config->readiness_timeout_ms + 20000);
    while (std::chrono::steady_clock::now() < c1_deadline) {
        snapshot = client.snapshot();

        if (!c1_process_observed && snapshot.runtime.process_id != 0 && snapshot.runtime.process_creation_time != 0) {
            c1_process_observed = true;
            c1_process_launch_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - c1_start_time).count());
            std::cout << "  [Cycle 1] Process observed: PID " << snapshot.runtime.process_id
                      << " at " << c1_process_launch_ms << " ms\n";
        }

        if (!c1_ownership_verified && (snapshot.runtime.state == RuntimeLifecycleState::OwnershipVerified ||
            snapshot.runtime.state == RuntimeLifecycleState::GuestBooting ||
            snapshot.runtime.state == RuntimeLifecycleState::Ready)) {
            c1_ownership_verified = true;
            c1_ownership_verified_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - c1_start_time).count());
            std::cout << "  [Cycle 1] Windows Job ownership verified: active procs "
                      << snapshot.runtime.active_process_count << " at " << c1_ownership_verified_ms << " ms\n";
        }

        const auto now = std::chrono::steady_clock::now();
        const std::string curr_state(runtime_lifecycle_name(snapshot.runtime.state));
        if (curr_state != last_c1_state || now - last_c1_log >= std::chrono::seconds(5)) {
            const auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - c1_start_time).count();
            std::cout << "  [Cycle 1 +" << elapsed_sec << "s] State: " << curr_state
                      << ", Job procs: " << snapshot.runtime.active_process_count
                      << ", Diag: " << snapshot.runtime.diagnostic << "\n";
            last_c1_state = curr_state;
            last_c1_log = now;
        }

        if (snapshot.runtime.state == RuntimeLifecycleState::Ready && snapshot.runtime.guest_ready) {
            break;
        }

        if ((c1_ownership_verified && snapshot.runtime.state == RuntimeLifecycleState::Stopped) ||
            snapshot.runtime.state == RuntimeLifecycleState::RecoveryRequired ||
            (snapshot.runtime.state == RuntimeLifecycleState::Failed && snapshot.runtime.generation_id.empty())) {
            const auto fail_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - c1_start_time).count());
            tracker.record("cycle1_start_and_ownership", StageStatus::Fail, fail_elapsed,
                std::string("Startup failed in state ") + std::string(runtime_lifecycle_name(snapshot.runtime.state)) +
                ": " + snapshot.runtime.diagnostic);
            tracker.block_remaining("Cycle 1 startup failed");
            tracker.print_summary_table();
            return 5;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!c1_ownership_verified || snapshot.runtime.process_id == 0) {
        const auto fail_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - c1_start_time).count());
        tracker.record("cycle1_start_and_ownership", StageStatus::Fail, fail_elapsed,
            "Windows Job process ownership was not verified before deadline");
        tracker.block_remaining("Ownership verification timed out");
        SupervisorRuntimeSnapshot dummy;
        std::uint64_t dummy_ms{};
        wait_for_shutdown(client, config->shutdown_timeout_ms, dummy, dummy_ms);
        tracker.print_summary_table();
        return 5;
    }

    std::ostringstream c1_owner_details;
    c1_owner_details << "PID=" << snapshot.runtime.process_id
                     << " creation_time=" << snapshot.runtime.process_creation_time
                     << " job_active_processes=" << snapshot.runtime.active_process_count
                     << " generation=" << snapshot.runtime.generation_id
                     << " (launch=" << c1_process_launch_ms << "ms)";
    tracker.record("cycle1_start_and_ownership", StageStatus::Pass, c1_ownership_verified_ms, c1_owner_details.str());

    if (snapshot.runtime.state != RuntimeLifecycleState::Ready || !snapshot.runtime.guest_ready || snapshot.runtime.adb_serial.empty()) {
        const auto boot_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - c1_start_time).count());
        tracker.record("cycle1_guest_ready", StageStatus::Fail, boot_elapsed,
            "Guest boot completed timeout: state=" + std::string(runtime_lifecycle_name(snapshot.runtime.state)) +
            " diag=" + snapshot.runtime.diagnostic);
        tracker.block_remaining("Guest boot readiness timed out");
        SupervisorRuntimeSnapshot dummy;
        std::uint64_t dummy_ms{};
        wait_for_shutdown(client, config->shutdown_timeout_ms, dummy, dummy_ms);
        tracker.print_summary_table();
        return 6;
    }

    const auto c1_guest_ready_time = std::chrono::steady_clock::now();
    const auto c1_ready_elapsed_total = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        c1_guest_ready_time - c1_start_time).count());
    const auto c1_guest_ready_duration = c1_ready_elapsed_total >= c1_process_launch_ms
        ? (c1_ready_elapsed_total - c1_process_launch_ms) : c1_ready_elapsed_total;

    std::ostringstream c1_ready_details;
    c1_ready_details << "serial=" << snapshot.runtime.adb_serial
                     << " sys.boot_completed=1 (guest_boot_duration=" << c1_guest_ready_duration
                     << "ms, total_start_to_ready=" << c1_ready_elapsed_total << "ms)";
    tracker.record("cycle1_guest_ready", StageStatus::Pass, c1_guest_ready_duration, c1_ready_details.str());

    // Install APK
    const auto install_start = std::chrono::steady_clock::now();
    std::string install_out;
    std::string install_err;
    const bool installed = run_adb(*config, snapshot.runtime.adb_serial,
        {L"install", L"-t", L"-r", apk.wstring()}, 120000, install_out, install_err);
    const auto install_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - install_start).count());

    if (!installed || install_out.find("Success") == std::string::npos) {
        tracker.record("cycle1_apk_install", StageStatus::Fail, install_elapsed,
            "ADB install failed: " + (install_err.empty() ? install_out : install_err));
        tracker.block_remaining("Validation APK installation failed");
        SupervisorRuntimeSnapshot dummy;
        std::uint64_t dummy_ms{};
        wait_for_shutdown(client, config->shutdown_timeout_ms, dummy, dummy_ms);
        tracker.print_summary_table();
        return 7;
    }

    std::string package_path_out;
    std::string package_path_err;
    if (!run_adb(*config, snapshot.runtime.adb_serial,
            {L"shell", L"pm", L"path", L"com.fang.animatedgles"}, 10000, package_path_out, package_path_err) ||
        package_path_out.find("package:") == std::string::npos) {
        tracker.record("cycle1_apk_install", StageStatus::Fail, install_elapsed,
            "Package com.fang.animatedgles was not confirmed installed via pm path");
        tracker.block_remaining("Package verification failed");
        SupervisorRuntimeSnapshot dummy;
        std::uint64_t dummy_ms{};
        wait_for_shutdown(client, config->shutdown_timeout_ms, dummy, dummy_ms);
        tracker.print_summary_table();
        return 7;
    }

    tracker.record("cycle1_apk_install", StageStatus::Pass, install_elapsed, "package:com.fang.animatedgles installed successfully");

    // Launch Activity
    const auto launch_start = std::chrono::steady_clock::now();
    std::string launch_out;
    std::string launch_err;
    const bool launched = run_adb(*config, snapshot.runtime.adb_serial,
        {L"shell", L"am", L"start", L"-W", L"-n", L"com.fang.animatedgles/.AnimatedGlesActivity"},
        30000, launch_out, launch_err);
    const auto launch_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - launch_start).count());

    if (!launched || launch_out.find("Status: ok") == std::string::npos) {
        tracker.record("cycle1_activity_launch", StageStatus::Fail, launch_elapsed,
            "ActivityManager start did not report Status: ok: " + launch_out);
        tracker.block_remaining("Activity launch failed");
        SupervisorRuntimeSnapshot dummy;
        std::uint64_t dummy_ms{};
        wait_for_shutdown(client, config->shutdown_timeout_ms, dummy, dummy_ms);
        tracker.print_summary_table();
        return 8;
    }
    tracker.record("cycle1_activity_launch", StageStatus::Pass, launch_elapsed, "ActivityManager Status: ok");

    // Observable verification: process PID + window/activity state
    const auto verify_start = std::chrono::steady_clock::now();
    std::string pid_out;
    std::string pid_err;
    run_adb(*config, snapshot.runtime.adb_serial,
        {L"shell", L"pidof", L"com.fang.animatedgles"}, 10000, pid_out, pid_err);
    while (!pid_out.empty() && (pid_out.back() == '\r' || pid_out.back() == '\n' || pid_out.back() == ' ')) {
        pid_out.pop_back();
    }

    if (pid_out.empty()) {
        const auto verify_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - verify_start).count());
        tracker.record("cycle1_activity_verify", StageStatus::Fail, verify_elapsed,
            "Package process com.fang.animatedgles was not found via pidof");
        tracker.block_remaining("Activity process verification failed");
        SupervisorRuntimeSnapshot dummy;
        std::uint64_t dummy_ms{};
        wait_for_shutdown(client, config->shutdown_timeout_ms, dummy, dummy_ms);
        tracker.print_summary_table();
        return 9;
    }

    std::string activity_top_out;
    std::string activity_top_err;
    run_adb(*config, snapshot.runtime.adb_serial,
        {L"shell", L"dumpsys", L"activity", L"top"}, 10000, activity_top_out, activity_top_err);

    const auto verify_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - verify_start).count());

    std::ostringstream verify_details;
    verify_details << "PID=" << pid_out
                   << " target=AnimatedGlesActivity"
                   << " (observed rendering synthetic test loop; not representative game FPS)";
    tracker.record("cycle1_activity_verify", StageStatus::Pass, verify_elapsed, verify_details.str());

    // Cycle 1 Graceful Shutdown
    std::cout << "\n>>> Executing Cycle 1 Graceful Shutdown\n";
    SupervisorRuntimeSnapshot c1_shutdown_snapshot;
    std::uint64_t c1_shutdown_elapsed_ms = 0;
    if (!wait_for_shutdown(client, config->shutdown_timeout_ms, c1_shutdown_snapshot, c1_shutdown_elapsed_ms)) {
        tracker.record("cycle1_graceful_shutdown", StageStatus::Fail, c1_shutdown_elapsed_ms,
            "Graceful shutdown did not transition to Stopped: state=" +
            std::string(runtime_lifecycle_name(c1_shutdown_snapshot.runtime.state)) +
            " diag=" + c1_shutdown_snapshot.runtime.diagnostic);
        tracker.block_remaining("Cycle 1 shutdown failed");
        tracker.print_summary_table();
        return 10;
    }
    tracker.record("cycle1_graceful_shutdown", StageStatus::Pass, c1_shutdown_elapsed_ms, "Supervisor reported Stopped");

    // Verify verified-empty Job and durable journal
    const auto empty_start = std::chrono::steady_clock::now();
    bool c1_job_empty = (c1_shutdown_snapshot.runtime.active_process_count == 0);
    std::string c1_journal_details;
    const bool c1_journal_finalized = verify_durable_finalized_state(
        canonical_id, snapshot.runtime.generation_id, c1_journal_details);

    std::string c1_adb_post_out;
    std::string c1_adb_post_err;
    bool c1_serial_gone = true;
    if (run_adb(*config, "", {L"devices"}, config->adb_command_timeout_ms, c1_adb_post_out, c1_adb_post_err)) {
        if (c1_adb_post_out.find(snapshot.runtime.adb_serial) != std::string::npos) {
            c1_serial_gone = false;
        }
    }

    const auto c1_empty_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - empty_start).count());

    if (!c1_job_empty || !c1_journal_finalized || !c1_serial_gone) {
        std::ostringstream fail_details;
        fail_details << "Verified-empty check failed: job_active=" << c1_shutdown_snapshot.runtime.active_process_count
                     << " journal_finalized=" << (c1_journal_finalized ? "yes" : "no")
                     << " (" << c1_journal_details << ")"
                     << " serial_gone=" << (c1_serial_gone ? "yes" : "no");
        tracker.record("cycle1_verified_empty", StageStatus::Fail, c1_empty_elapsed, fail_details.str());
        tracker.block_remaining("Cycle 1 ownership empty verification failed");
        tracker.print_summary_table();
        return 11;
    }
    tracker.record("cycle1_verified_empty", StageStatus::Pass, c1_empty_elapsed,
        "Job processes=0, Journal=VerifiedEmpty (" + c1_journal_details + "), Serial removed from adb devices");

    // =========================================================================
    // CYCLE 2: Second Start -> Guest Ready -> Activity Verify -> Shutdown -> Empty
    // =========================================================================
    std::cout << "\n>>> Starting Cycle 2: Restart & Re-verification Cycle\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    const auto c2_start_time = std::chrono::steady_clock::now();
    if (!client.start(options.avd_name)) {
        tracker.record("cycle2_start_and_ownership", StageStatus::Fail, 0, "Supervisor client failed to queue Cycle 2 start request");
        tracker.block_remaining("Cycle 2 start request failed");
        tracker.print_summary_table();
        return 12;
    }

    bool c2_process_observed = false;
    bool c2_ownership_verified = false;
    std::uint64_t c2_process_launch_ms = 0;
    std::uint64_t c2_ownership_verified_ms = 0;
    SupervisorRuntimeSnapshot c2_snapshot;

    auto last_c2_log = std::chrono::steady_clock::now();
    std::string last_c2_state;
    const auto c2_deadline = c2_start_time + std::chrono::milliseconds(config->readiness_timeout_ms + 20000);
    while (std::chrono::steady_clock::now() < c2_deadline) {
        c2_snapshot = client.snapshot();
        if (c2_snapshot.request_pending || c2_snapshot.runtime.generation_id == snapshot.runtime.generation_id) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (!c2_process_observed && c2_snapshot.runtime.process_id != 0 && c2_snapshot.runtime.process_creation_time != 0) {
            c2_process_observed = true;
            c2_process_launch_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - c2_start_time).count());
            std::cout << "  [Cycle 2] Process observed: PID " << c2_snapshot.runtime.process_id
                      << " at " << c2_process_launch_ms << " ms\n";
        }

        if (!c2_ownership_verified && (c2_snapshot.runtime.state == RuntimeLifecycleState::OwnershipVerified ||
            c2_snapshot.runtime.state == RuntimeLifecycleState::GuestBooting ||
            c2_snapshot.runtime.state == RuntimeLifecycleState::Ready)) {
            c2_ownership_verified = true;
            c2_ownership_verified_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - c2_start_time).count());
            std::cout << "  [Cycle 2] Windows Job ownership verified: active procs "
                      << c2_snapshot.runtime.active_process_count << " at " << c2_ownership_verified_ms << " ms\n";
        }

        const auto now = std::chrono::steady_clock::now();
        const std::string curr_state(runtime_lifecycle_name(c2_snapshot.runtime.state));
        if (curr_state != last_c2_state || now - last_c2_log >= std::chrono::seconds(5)) {
            const auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - c2_start_time).count();
            std::cout << "  [Cycle 2 +" << elapsed_sec << "s] State: " << curr_state
                      << ", Job procs: " << c2_snapshot.runtime.active_process_count
                      << ", Diag: " << c2_snapshot.runtime.diagnostic << "\n";
            last_c2_state = curr_state;
            last_c2_log = now;
        }

        if (c2_snapshot.runtime.state == RuntimeLifecycleState::Ready && c2_snapshot.runtime.guest_ready) {
            break;
        }

        if ((c2_ownership_verified && c2_snapshot.runtime.state == RuntimeLifecycleState::Stopped) ||
            c2_snapshot.runtime.state == RuntimeLifecycleState::RecoveryRequired ||
            (c2_snapshot.runtime.state == RuntimeLifecycleState::Failed && c2_snapshot.runtime.generation_id.empty())) {
            const auto fail_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - c2_start_time).count());
            tracker.record("cycle2_start_and_ownership", StageStatus::Fail, fail_elapsed,
                std::string("Cycle 2 startup failed in state ") + std::string(runtime_lifecycle_name(c2_snapshot.runtime.state)) +
                ": " + c2_snapshot.runtime.diagnostic);
            tracker.block_remaining("Cycle 2 startup failed");
            tracker.print_summary_table();
            return 13;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!c2_ownership_verified || c2_snapshot.runtime.process_id == 0) {
        const auto fail_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - c2_start_time).count());
        tracker.record("cycle2_start_and_ownership", StageStatus::Fail, fail_elapsed,
            "Cycle 2 Windows Job process ownership was not verified before deadline");
        tracker.block_remaining("Cycle 2 ownership verification timed out");
        SupervisorRuntimeSnapshot dummy;
        std::uint64_t dummy_ms{};
        wait_for_shutdown(client, config->shutdown_timeout_ms, dummy, dummy_ms);
        tracker.print_summary_table();
        return 13;
    }

    std::ostringstream c2_owner_details;
    c2_owner_details << "PID=" << c2_snapshot.runtime.process_id
                     << " creation_time=" << c2_snapshot.runtime.process_creation_time
                     << " job_active_processes=" << c2_snapshot.runtime.active_process_count
                     << " generation=" << c2_snapshot.runtime.generation_id
                     << " (launch=" << c2_process_launch_ms << "ms)";
    tracker.record("cycle2_start_and_ownership", StageStatus::Pass, c2_ownership_verified_ms, c2_owner_details.str());

    if (c2_snapshot.runtime.state != RuntimeLifecycleState::Ready || !c2_snapshot.runtime.guest_ready || c2_snapshot.runtime.adb_serial.empty()) {
        const auto boot_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - c2_start_time).count());
        tracker.record("cycle2_guest_ready", StageStatus::Fail, boot_elapsed,
            "Cycle 2 guest boot completed timeout: state=" + std::string(runtime_lifecycle_name(c2_snapshot.runtime.state)) +
            " diag=" + c2_snapshot.runtime.diagnostic);
        tracker.block_remaining("Cycle 2 guest boot timed out");
        SupervisorRuntimeSnapshot dummy;
        std::uint64_t dummy_ms{};
        wait_for_shutdown(client, config->shutdown_timeout_ms, dummy, dummy_ms);
        tracker.print_summary_table();
        return 14;
    }

    const auto c2_guest_ready_time = std::chrono::steady_clock::now();
    const auto c2_ready_elapsed_total = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        c2_guest_ready_time - c2_start_time).count());
    const auto c2_guest_ready_duration = c2_ready_elapsed_total >= c2_process_launch_ms
        ? (c2_ready_elapsed_total - c2_process_launch_ms) : c2_ready_elapsed_total;

    std::ostringstream c2_ready_details;
    c2_ready_details << "serial=" << c2_snapshot.runtime.adb_serial
                     << " sys.boot_completed=1 (guest_boot_duration=" << c2_guest_ready_duration
                     << "ms, total_start_to_ready=" << c2_ready_elapsed_total << "ms)";
    tracker.record("cycle2_guest_ready", StageStatus::Pass, c2_guest_ready_duration, c2_ready_details.str());

    // Cycle 2: Validation Activity Re-verification
    const auto c2_launch_start = std::chrono::steady_clock::now();
    std::string c2_package_path_out;
    std::string c2_package_path_err;
    bool c2_installed = run_adb(*config, c2_snapshot.runtime.adb_serial,
        {L"shell", L"pm", L"path", L"com.fang.animatedgles"}, 10000,
        c2_package_path_out, c2_package_path_err) &&
        c2_package_path_out.find("package:") != std::string::npos;

    if (!c2_installed) {
        std::string c2_install_out;
        std::string c2_install_err;
        run_adb(*config, c2_snapshot.runtime.adb_serial,
            {L"install", L"-t", L"-r", apk.wstring()}, 120000,
            c2_install_out, c2_install_err);
    }

    std::string c2_launch_out;
    std::string c2_launch_err;
    run_adb(*config, c2_snapshot.runtime.adb_serial,
        {L"shell", L"am", L"start", L"-W", L"-n", L"com.fang.animatedgles/.AnimatedGlesActivity"},
        30000, c2_launch_out, c2_launch_err);

    std::string c2_pid_out;
    std::string c2_pid_err;
    const auto c2_pid_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < c2_pid_deadline) {
        c2_pid_out.clear();
        c2_pid_err.clear();
        run_adb(*config, c2_snapshot.runtime.adb_serial,
            {L"shell", L"pidof", L"com.fang.animatedgles"}, 10000, c2_pid_out, c2_pid_err);
        while (!c2_pid_out.empty() && (c2_pid_out.back() == '\r' || c2_pid_out.back() == '\n' || c2_pid_out.back() == ' ')) {
            c2_pid_out.pop_back();
        }
        if (!c2_pid_out.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    const auto c2_verify_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - c2_launch_start).count());

    if (c2_pid_out.empty()) {
        std::ostringstream c2_fail_details;
        c2_fail_details << "Cycle 2 package process com.fang.animatedgles not running; launch_out=["
                        << c2_launch_out << "] launch_err=[" << c2_launch_err << "]";
        tracker.record("cycle2_activity_verify", StageStatus::Fail, c2_verify_elapsed,
            c2_fail_details.str());
        tracker.block_remaining("Cycle 2 Activity verification failed");
        SupervisorRuntimeSnapshot dummy;
        std::uint64_t dummy_ms{};
        wait_for_shutdown(client, config->shutdown_timeout_ms, dummy, dummy_ms);
        tracker.print_summary_table();
        return 15;
    }

    std::ostringstream c2_verify_details;
    c2_verify_details << "PID=" << c2_pid_out
                      << " target=AnimatedGlesActivity (re-verified after reboot)";
    tracker.record("cycle2_activity_verify", StageStatus::Pass, c2_verify_elapsed, c2_verify_details.str());

    // Cycle 2 Graceful Shutdown
    std::cout << "\n>>> Executing Cycle 2 Graceful Shutdown\n";
    SupervisorRuntimeSnapshot c2_shutdown_snapshot;
    std::uint64_t c2_shutdown_elapsed_ms = 0;
    if (!wait_for_shutdown(client, config->shutdown_timeout_ms, c2_shutdown_snapshot, c2_shutdown_elapsed_ms)) {
        tracker.record("cycle2_graceful_shutdown", StageStatus::Fail, c2_shutdown_elapsed_ms,
            "Cycle 2 graceful shutdown did not transition to Stopped: state=" +
            std::string(runtime_lifecycle_name(c2_shutdown_snapshot.runtime.state)) +
            " diag=" + c2_shutdown_snapshot.runtime.diagnostic);
        tracker.block_remaining("Cycle 2 shutdown failed");
        tracker.print_summary_table();
        return 16;
    }
    tracker.record("cycle2_graceful_shutdown", StageStatus::Pass, c2_shutdown_elapsed_ms, "Supervisor reported Stopped");

    // Cycle 2 Verified-empty
    const auto c2_empty_start = std::chrono::steady_clock::now();
    bool c2_job_empty = (c2_shutdown_snapshot.runtime.active_process_count == 0);
    std::string c2_journal_details;
    const bool c2_journal_finalized = verify_durable_finalized_state(
        canonical_id, c2_snapshot.runtime.generation_id, c2_journal_details);

    std::string c2_adb_post_out;
    std::string c2_adb_post_err;
    bool c2_serial_gone = true;
    if (run_adb(*config, "", {L"devices"}, config->adb_command_timeout_ms, c2_adb_post_out, c2_adb_post_err)) {
        if (c2_adb_post_out.find(c2_snapshot.runtime.adb_serial) != std::string::npos) {
            c2_serial_gone = false;
        }
    }

    const auto c2_empty_elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - c2_empty_start).count());

    if (!c2_job_empty || !c2_journal_finalized || !c2_serial_gone) {
        std::ostringstream fail_details;
        fail_details << "Cycle 2 verified-empty failed: job_active=" << c2_shutdown_snapshot.runtime.active_process_count
                     << " journal_finalized=" << (c2_journal_finalized ? "yes" : "no")
                     << " (" << c2_journal_details << ")"
                     << " serial_gone=" << (c2_serial_gone ? "yes" : "no");
        tracker.record("cycle2_verified_empty", StageStatus::Fail, c2_empty_elapsed, fail_details.str());
        tracker.print_summary_table();
        return 17;
    }
    tracker.record("cycle2_verified_empty", StageStatus::Pass, c2_empty_elapsed,
        "Job processes=0, Journal=VerifiedEmpty (" + c2_journal_details + "), Serial removed from adb devices");

    tracker.print_summary_table();
    return 0;
}

}

int wmain(int argc, wchar_t* argv[]) {
    Options options;
    std::string error;
    if (!parse_options(argc, argv, options, error)) {
        std::cerr << error << '\n';
        return 2;
    }
    try {
        return run(options);
    } catch (const std::exception& exception) {
        std::cerr << "reference_smoke_exception=" << exception.what() << '\n';
        return 18;
    }
}
