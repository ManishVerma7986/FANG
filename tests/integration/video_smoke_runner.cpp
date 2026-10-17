#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <rpc.h>

#include "video_smoke_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#pragma comment(lib, "Rpcrt4.lib")

namespace fang::video_smoke {

namespace {

std::uint64_t default_now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void default_sleep_ms(std::uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string escape_json(std::string_view input) {
    std::ostringstream ss;
    for (const char c : input) {
        switch (c) {
        case '\"': ss << "\\\""; break;
        case '\\': ss << "\\\\"; break;
        case '\b': ss << "\\b"; break;
        case '\f': ss << "\\f"; break;
        case '\n': ss << "\\n"; break;
        case '\r': ss << "\\r"; break;
        case '\t': ss << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
            } else {
                ss << c;
            }
            break;
        }
    }
    return ss.str();
}

} // namespace

bool validate_video_smoke_config(const VideoSmokeConfig& config, std::string& error) {
    if (config.avd_name.empty()) {
        error = "AVD name must not be empty.";
        return false;
    }
    for (char ch : config.avd_name) {
        if (static_cast<unsigned char>(ch) <= 0x20 || static_cast<unsigned char>(ch) > 0x7E) {
            error = "AVD name must contain only printable ASCII characters without whitespace.";
            return false;
        }
    }
    if (config.emulator_port < 1024 || config.emulator_port > 65534) {
        error = "Emulator console port must be within range [1024, 65534].";
        return false;
    }
    if (config.emulator_port % 2 != 0) {
        error = "Emulator console port must be an even integer (e.g. 5554).";
        return false;
    }
    if (config.readiness_timeout_ms < 1000 || config.readiness_timeout_ms > 600000) {
        error = "Readiness timeout must be within range [1000, 600000] ms.";
        return false;
    }
    if (config.max_observation_ms < 1000 || config.max_observation_ms > 120000) {
        error = "Observation duration must be within range [1000, 120000] ms.";
        return false;
    }
    if (config.max_accepted_frames == 0 || config.max_accepted_frames > 10000) {
        error = "Maximum accepted frames must be within range [1, 10000].";
        return false;
    }
    if (config.shutdown_timeout_ms < 1000 || config.shutdown_timeout_ms > 120000) {
        error = "Shutdown timeout must be within range [1000, 120000] ms.";
        return false;
    }
    return true;
}

const char* stage_status_string(SmokeStageStatus status) noexcept {
    switch (status) {
    case SmokeStageStatus::Pass:    return "PASS";
    case SmokeStageStatus::Fail:    return "FAIL";
    case SmokeStageStatus::Blocked: return "BLOCKED";
    case SmokeStageStatus::NotRun:  return "NOT RUN";
    default:                        return "UNKNOWN";
    }
}

std::string generate_run_uuid() {
    UUID uuid{};
    if (UuidCreate(&uuid) == RPC_S_OK) {
        RPC_CSTR rpc_str = nullptr;
        if (UuidToStringA(&uuid, &rpc_str) == RPC_S_OK && rpc_str != nullptr) {
            std::string result(reinterpret_cast<char*>(rpc_str));
            RpcStringFreeA(&rpc_str);
            return result;
        }
    }
    // Fallback pseudo-UUID if RPC unavailable
    std::array<std::uint8_t, 16> bytes{};
    BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    bytes[6] = (bytes[6] & 0x0F) | 0x40; // Version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80; // Variant 1
    std::ostringstream ss;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) ss << "-";
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    return ss.str();
}

std::string current_iso8601_timestamp() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buffer);
}

ObservationResult run_video_observation(
    fang::studio::IVideoTransport& transport,
    fang::studio::StudioVideoConsumer& consumer,
    ObservationLimits limits,
    std::function<std::uint64_t()> now_ms_fn,
    std::function<void(std::uint32_t)> sleep_ms_fn) {

    if (!now_ms_fn) now_ms_fn = default_now_ms;
    if (!sleep_ms_fn) sleep_ms_fn = default_sleep_ms;

    ObservationResult result{};
    const auto start_time = now_ms_fn();

    while (true) {
        const auto now = now_ms_fn();
        const auto elapsed = now >= start_time ? now - start_time : 0;
        result.duration_ms = elapsed;

        const auto metrics = consumer.metrics();
        result.accepted_frames = metrics.frames_received;
        result.dropped_frames = metrics.frames_dropped;
        result.torn_frames = metrics.frames_torn;
        result.counter_discontinuities = metrics.counter_discontinuities;
        result.final_sequence = metrics.latest_sequence;

        // Check 1: Accepted frames reached budget limit
        if (result.accepted_frames >= limits.max_frames) {
            result.succeeded = true;
            result.reached_frame_limit = true;
            break;
        }

        // Check 2: Observation duration reached budget deadline
        if (elapsed >= limits.max_duration_ms) {
            result.reached_deadline = true;
            result.succeeded = (result.accepted_frames > 0);
            if (!result.succeeded) {
                result.error = "Observation duration reached deadline without accepting any frames.";
            }
            break;
        }

        // Check 3: Transport error or closed
        const auto transport_state = transport.state();
        if (transport_state == fang::studio::VideoTransportState::Error) {
            result.succeeded = false;
            result.error = "Video transport entered Error state: " + transport.last_error();
            break;
        }
        if (transport_state == fang::studio::VideoTransportState::Closed ||
            transport_state == fang::studio::VideoTransportState::Disconnected) {
            result.succeeded = false;
            result.error = "Video transport closed or disconnected unexpectedly.";
            break;
        }

        sleep_ms_fn(10);
    }

    return result;
}

ReadinessWaitResult wait_for_readiness(
    std::function<fang::studio::SupervisorRuntimeSnapshot()> snapshot_fn,
    std::uint32_t timeout_ms,
    std::function<std::uint64_t()> now_ms_fn,
    std::function<void(std::uint32_t)> sleep_ms_fn) {

    if (!now_ms_fn) now_ms_fn = default_now_ms;
    if (!sleep_ms_fn) sleep_ms_fn = default_sleep_ms;

    ReadinessWaitResult result{};
    const auto start_time = now_ms_fn();
    std::string last_recorded_state;

    while (true) {
        const auto now = now_ms_fn();
        const auto elapsed = now >= start_time ? now - start_time : 0;
        result.elapsed_ms = elapsed;

        if (elapsed >= timeout_ms) {
            result.timed_out = true;
            result.error = "Readiness timed out after " + std::to_string(timeout_ms) + " ms.";
            break;
        }

        const auto snapshot = snapshot_fn();
        const auto current_state = std::string(fang::broker::runtime_lifecycle_name(snapshot.runtime.state));

        if (current_state != last_recorded_state) {
            result.transitions.push_back({current_state, elapsed});
            last_recorded_state = current_state;
        }

        if (!snapshot.runtime.generation_id.empty()) {
            if (result.generation_id.empty()) {
                result.generation_id = snapshot.runtime.generation_id;
            } else if (snapshot.runtime.generation_id != result.generation_id) {
                result.succeeded = false;
                result.error = "Runtime generation discontinuity detected during readiness observation: expected " +
                    result.generation_id + ", observed " + snapshot.runtime.generation_id;
                break;
            }
        }

        if (snapshot.runtime.process_id != 0 && result.process_id == 0) {
            result.process_id = snapshot.runtime.process_id;
            result.process_creation_time = snapshot.runtime.process_creation_time;
        }

        if (snapshot.runtime.active_process_count > 0) {
            result.active_processes = snapshot.runtime.active_process_count;
        }

        if (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::OwnershipVerified ||
            snapshot.runtime.state == fang::broker::RuntimeLifecycleState::GuestBooting ||
            snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Ready) {
            result.containment_verified = true;
        }

        // Target readiness condition: Supervisor reports Ready AND guest_ready is true
        if (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Ready &&
            snapshot.runtime.guest_ready) {
            result.succeeded = true;
            break;
        }

        // Abort early if supervisor reports failed or recovery required
        if (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Failed ||
            snapshot.runtime.state == fang::broker::RuntimeLifecycleState::RecoveryRequired) {
            result.succeeded = false;
            result.error = snapshot.message.empty()
                ? "Supervisor entered failure state: " + current_state
                : snapshot.message;
            break;
        }

        sleep_ms_fn(50);
    }

    return result;
}

ShutdownWaitResult wait_for_shutdown(
    std::function<fang::studio::SupervisorRuntimeSnapshot()> snapshot_fn,
    std::uint32_t timeout_ms,
    std::function<std::uint64_t()> now_ms_fn,
    std::function<void(std::uint32_t)> sleep_ms_fn) {

    if (!now_ms_fn) now_ms_fn = default_now_ms;
    if (!sleep_ms_fn) sleep_ms_fn = default_sleep_ms;

    ShutdownWaitResult result{};
    const auto start_time = now_ms_fn();

    while (true) {
        const auto now = now_ms_fn();
        const auto elapsed = now >= start_time ? now - start_time : 0;
        result.elapsed_ms = elapsed;

        if (elapsed >= timeout_ms) {
            result.timed_out = true;
            result.error = "Shutdown verification timed out after " + std::to_string(timeout_ms) + " ms.";
            break;
        }

        const auto snapshot = snapshot_fn();
        if (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Stopped) {
            result.succeeded = true;
            break;
        }

        if (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Failed) {
            result.succeeded = false;
            result.error = "Supervisor reported failure during shutdown: " + snapshot.message;
            break;
        }

        sleep_ms_fn(50);
    }

    return result;
}

std::string format_json_report(const VideoSmokeReport& report) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schema_version\": \"" << escape_json(report.schema_version) << "\",\n";
    ss << "  \"run_id\": \"" << escape_json(report.run_id) << "\",\n";
    ss << "  \"run_timestamp\": \"" << escape_json(report.run_timestamp) << "\",\n";
    ss << "  \"outcome\": \"" << escape_json(report.outcome) << "\",\n";
    if (!report.failure_reason.empty()) {
        ss << "  \"failure_reason\": \"" << escape_json(report.failure_reason) << "\",\n";
    }

    // Target configuration
    ss << "  \"target_configuration\": {\n";
    ss << "    \"avd_name\": \"" << escape_json(report.avd_name) << "\",\n";
    ss << "    \"emulator_port\": " << report.emulator_port << ",\n";
    ss << "    \"expected_serial\": \"" << escape_json(report.expected_serial) << "\",\n";
    ss << "    \"expected_mapping\": \"" << escape_json(report.expected_mapping) << "\",\n";
    ss << "    \"emulator_version\": \"" << escape_json(report.emulator_version) << "\",\n";
    ss << "    \"emulator_build_id\": \"" << escape_json(report.emulator_build_id) << "\"\n";
    ss << "  },\n";

    // Preflight checks
    ss << "  \"preflight_checks\": {\n";
    ss << "    \"port_free\": " << (report.port_free ? "true" : "false") << ",\n";
    ss << "    \"serial_not_active\": " << (report.serial_not_active ? "true" : "false") << ",\n";
    ss << "    \"mapping_initially_absent\": " << (report.mapping_initially_absent ? "true" : "false") << ",\n";
    ss << "    \"journal_initially_clean\": " << (report.journal_initially_clean ? "true" : "false") << "\n";
    ss << "  },\n";

    // Lifecycle evidence
    ss << "  \"lifecycle_evidence\": {\n";
    ss << "    \"generation_id\": ";
    if (!report.generation_id.empty()) {
        ss << "\"" << escape_json(report.generation_id) << "\",\n";
    } else {
        ss << "null,\n";
    }
    ss << "    \"owned_process_id\": " << report.owned_process_id << ",\n";
    ss << "    \"process_creation_time\": " << report.process_creation_time << ",\n";
    ss << "    \"containment_verified\": " << (report.containment_verified ? "true" : "false") << ",\n";
    ss << "    \"active_processes_in_job\": " << report.active_processes_in_job << ",\n";
    ss << "    \"readiness_elapsed_ms\": " << report.readiness_elapsed_ms << ",\n";
    ss << "    \"guest_ready_observed\": " << (report.guest_ready_observed ? "true" : "false") << ",\n";
    ss << "    \"state_transitions\": [\n";
    for (std::size_t i = 0; i < report.state_transitions.size(); ++i) {
        const auto& t = report.state_transitions[i];
        ss << "      {\"state\": \"" << escape_json(t.state) << "\", \"timestamp_ms\": " << t.timestamp_ms << "}";
        if (i + 1 < report.state_transitions.size()) ss << ",";
        ss << "\n";
    }
    ss << "    ]\n";
    ss << "  },\n";

    // Mapping and header evidence
    ss << "  \"mapping_and_header_evidence\": {\n";
    ss << "    \"mapping_open_succeeded\": " << (report.mapping_open_succeeded ? "true" : "false") << ",\n";
    if (!report.mapping_open_error.empty()) {
        ss << "    \"mapping_open_error\": \"" << escape_json(report.mapping_open_error) << "\",\n";
    }
    ss << "    \"header_width\": ";
    if (report.header_width.has_value()) ss << *report.header_width; else ss << "null";
    ss << ",\n";
    ss << "    \"header_height\": ";
    if (report.header_height.has_value()) ss << *report.header_height; else ss << "null";
    ss << ",\n";
    ss << "    \"header_reported_fps\": ";
    if (report.header_reported_fps.has_value()) ss << *report.header_reported_fps; else ss << "null";
    ss << ",\n";
    ss << "    \"header_reported_fps_note\": \"Header metadata field; not measured FPS\",\n";
    ss << "    \"initial_frame_counter\": ";
    if (report.initial_frame_counter.has_value()) ss << *report.initial_frame_counter; else ss << "null";
    ss << ",\n";
    ss << "    \"final_frame_counter\": ";
    if (report.final_frame_counter.has_value()) ss << *report.final_frame_counter; else ss << "null";
    ss << "\n";
    ss << "  },\n";

    // Video observation evidence
    ss << "  \"video_observation_evidence\": {\n";
    ss << "    \"observation_duration_ms\": " << report.observation_duration_ms << ",\n";
    ss << "    \"accepted_frames\": " << report.accepted_frames << ",\n";
    ss << "    \"dropped_frames\": " << report.dropped_frames << ",\n";
    ss << "    \"torn_frames\": " << report.torn_frames << ",\n";
    ss << "    \"counter_discontinuities\": " << report.counter_discontinuities << ",\n";
    ss << "    \"observation_stopped_by_frame_limit\": " << (report.observation_stopped_by_frame_limit ? "true" : "false") << ",\n";
    ss << "    \"observation_stopped_by_deadline\": " << (report.observation_stopped_by_deadline ? "true" : "false") << ",\n";
    ss << "    \"viewport_rendering_confirmed\": " << (report.viewport_rendering_confirmed ? "true" : "false") << ",\n";
    ss << "    \"viewport_rendering_note\": \"" << escape_json(report.viewport_rendering_note) << "\"\n";
    ss << "  },\n";

    // Shutdown and cleanup evidence
    ss << "  \"shutdown_and_cleanup_evidence\": {\n";
    ss << "    \"shutdown_requested\": " << (report.shutdown_requested ? "true" : "false") << ",\n";
    ss << "    \"shutdown_elapsed_ms\": " << report.shutdown_elapsed_ms << ",\n";
    ss << "    \"process_exit_confirmed\": " << (report.process_exit_confirmed ? "true" : "false") << ",\n";
    if (!report.process_exit_detail.empty()) {
        ss << "    \"process_exit_detail\": \"" << escape_json(report.process_exit_detail) << "\",\n";
    }
    ss << "    \"containment_empty_verified\": " << (report.containment_empty_verified ? "true" : "false") << ",\n";
    ss << "    \"journal_final_state\": \"" << escape_json(report.journal_final_state) << "\",\n";
    ss << "    \"mapping_released_verified\": " << (report.mapping_released_verified ? "true" : "false") << ",\n";
    ss << "    \"cleanup_verified\": " << (report.cleanup_verified ? "true" : "false");
    if (!report.cleanup_failure_reason.empty()) {
        ss << ",\n    \"cleanup_failure_reason\": \"" << escape_json(report.cleanup_failure_reason) << "\"";
    }
    ss << "\n  },\n";

    // Explicit unavailable observations
    ss << "  \"unavailable_fields\": [\n";
    for (std::size_t i = 0; i < report.unavailable_fields.size(); ++i) {
        const auto& u = report.unavailable_fields[i];
        ss << "    {\"field\": \"" << escape_json(u.field_name) << "\", \"reason\": \"" << escape_json(u.reason) << "\"}";
        if (i + 1 < report.unavailable_fields.size()) ss << ",";
        ss << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";

    return ss.str();
}

bool validate_report_schema(std::string_view json_text, std::string& error) {
    if (json_text.empty()) {
        error = "JSON report is empty.";
        return false;
    }
    const std::vector<std::string_view> required_keys = {
        "\"schema_version\"",
        "\"run_id\"",
        "\"run_timestamp\"",
        "\"outcome\"",
        "\"target_configuration\"",
        "\"preflight_checks\"",
        "\"lifecycle_evidence\"",
        "\"mapping_and_header_evidence\"",
        "\"video_observation_evidence\"",
        "\"shutdown_and_cleanup_evidence\"",
        "\"unavailable_fields\""
    };
    for (const auto& key : required_keys) {
        if (json_text.find(key) == std::string_view::npos) {
            error = "JSON report is missing required top-level key: " + std::string(key);
            return false;
        }
    }
    return true;
}

ReportPersistenceResult persist_json_report(
    const VideoSmokeReport& report,
    const ReportPersistenceOptions& options) {

    ReportPersistenceResult result{};
    std::filesystem::path target_path;
    if (options.output_path.empty()) {
        const std::string run_id_safe = report.run_id.empty() ? generate_run_uuid() : report.run_id;
        target_path = std::filesystem::path("out") / "video-smoke" / "runs" / (run_id_safe + ".json");
    } else {
        target_path = std::filesystem::path(options.output_path);
    }
    result.resolved_path = target_path.string();

    std::error_code ec;
    if (target_path.has_parent_path()) {
        std::filesystem::create_directories(target_path.parent_path(), ec);
        if (ec) {
            result.succeeded = false;
            result.error = "Failed to create directory " + target_path.parent_path().string() + ": " + ec.message();
            return result;
        }
    }

    const DWORD creation_disposition = options.allow_overwrite ? CREATE_ALWAYS : CREATE_NEW;
    const std::wstring wide_path = target_path.wstring();
    HANDLE hFile = CreateFileW(
        wide_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        creation_disposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        result.succeeded = false;
        if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
            result.error = "Refusing to overwrite existing destination (collision protection; --overwrite required): " + target_path.string();
        } else {
            result.error = "CreateFileW failed (Win32 error " + std::to_string(err) + "): " + target_path.string();
        }
        return result;
    }

    const std::string content = format_json_report(report);
    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(
        hFile,
        content.data(),
        static_cast<DWORD>(content.size()),
        &bytes_written,
        nullptr);

    if (!write_ok || bytes_written != content.size()) {
        const DWORD err = GetLastError();
        CloseHandle(hFile);
        result.succeeded = false;
        result.error = "WriteFile failed to write full payload (Win32 error " + std::to_string(err) + ")";
        return result;
    }

    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    result.succeeded = true;
    return result;
}

ProcessExitVerificationResult verify_process_exit_by_identity(
    std::uint32_t process_id,
    std::uint64_t expected_creation_time) {

    if (process_id == 0) {
        return {ProcessExitStatus::VerificationUnavailable, "Expected process ID is unavailable or invalid (0)."};
    }
    if (expected_creation_time == 0) {
        return {ProcessExitStatus::VerificationUnavailable, "Expected process creation timestamp is unavailable or invalid (0)."};
    }

    HANDLE hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (hProcess == nullptr) {
        const DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER) {
            return {ProcessExitStatus::ConfirmedExited,
                "Process PID " + std::to_string(process_id) + " no longer exists in system table (ERROR_INVALID_PARAMETER)."};
        }
        if (err == ERROR_ACCESS_DENIED) {
            return {ProcessExitStatus::VerificationUnavailable,
                "OpenProcess on PID " + std::to_string(process_id) + " failed with ERROR_ACCESS_DENIED."};
        }
        return {ProcessExitStatus::VerificationUnavailable,
            "OpenProcess on PID " + std::to_string(process_id) + " failed with Win32 error " + std::to_string(err)};
    }

    FILETIME ft_create{}, ft_exit{}, ft_kernel{}, ft_user{};
    if (!GetProcessTimes(hProcess, &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
        const DWORD err = GetLastError();
        CloseHandle(hProcess);
        return {ProcessExitStatus::VerificationUnavailable,
            "GetProcessTimes on PID " + std::to_string(process_id) + " failed with Win32 error " + std::to_string(err)};
    }

    ULARGE_INTEGER uli{};
    uli.LowPart = ft_create.dwLowDateTime;
    uli.HighPart = ft_create.dwHighDateTime;
    if (uli.QuadPart != expected_creation_time) {
        CloseHandle(hProcess);
        return {ProcessExitStatus::RecycledPidExited,
            "PID " + std::to_string(process_id) + " was recycled by another process (creation time mismatch: expected " +
            std::to_string(expected_creation_time) + ", found " + std::to_string(uli.QuadPart) + "); original process confirmed exited."};
    }

    const DWORD wait_res = WaitForSingleObject(hProcess, 0);
    CloseHandle(hProcess);

    if (wait_res == WAIT_OBJECT_0) {
        return {ProcessExitStatus::ConfirmedExited,
            "Process PID " + std::to_string(process_id) + " with matching creation time is signaled (terminated)."};
    }
    if (wait_res == WAIT_TIMEOUT) {
        return {ProcessExitStatus::StillRunning,
            "Process PID " + std::to_string(process_id) + " with matching creation time is still active."};
    }

    return {ProcessExitStatus::VerificationUnavailable,
        "WaitForSingleObject on PID " + std::to_string(process_id) + " failed with wait status " + std::to_string(wait_res)};
}

PostLaunchCleanupResult execute_post_launch_cleanup(
    std::function<bool()> request_shutdown_fn,
    std::function<fang::studio::SupervisorRuntimeSnapshot()> snapshot_fn,
    const std::function<fang::broker::JournalLoadResult()>& load_journal_fn,
    const std::wstring& wide_mapping,
    std::uint32_t process_id,
    std::uint64_t process_creation_time,
    std::uint32_t shutdown_timeout_ms,
    std::function<ProcessExitVerificationResult(std::uint32_t, std::uint64_t)> verify_proc_exit_fn,
    std::function<bool(const std::wstring&)> check_mapping_released_fn,
    std::function<std::uint64_t()> now_ms_fn,
    std::function<void(std::uint32_t)> sleep_ms_fn) {

    if (!verify_proc_exit_fn) verify_proc_exit_fn = verify_process_exit_by_identity;
    if (!check_mapping_released_fn) {
        check_mapping_released_fn = [](const std::wstring& mapping_name) {
            HANDLE verify_shm = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name.c_str());
            if (verify_shm == nullptr) return true;
            CloseHandle(verify_shm);
            return false;
        };
    }
    if (!now_ms_fn) now_ms_fn = default_now_ms;
    if (!sleep_ms_fn) sleep_ms_fn = default_sleep_ms;

    PostLaunchCleanupResult result{};

    // 1. Request supervisor-managed shutdown
    try {
        result.shutdown_requested = request_shutdown_fn ? request_shutdown_fn() : false;
    } catch (const std::exception&) {
        result.shutdown_requested = false;
    } catch (...) {
        result.shutdown_requested = false;
    }

    // 2. Wait for supervisor shutdown confirmation (bounded timeout)
    std::string shutdown_error;
    try {
        const auto shutdown_res = wait_for_shutdown(snapshot_fn, shutdown_timeout_ms, now_ms_fn, sleep_ms_fn);
        result.shutdown_succeeded = shutdown_res.succeeded;
        result.shutdown_elapsed_ms = shutdown_res.elapsed_ms;
        shutdown_error = shutdown_res.error;
    } catch (const std::exception& e) {
        result.shutdown_succeeded = false;
        shutdown_error = "Exception during shutdown wait: " + std::string(e.what());
    } catch (...) {
        result.shutdown_succeeded = false;
        shutdown_error = "Unknown exception during shutdown wait";
    }

    // 3. Independent process exit verification by identity
    try {
        result.process_exit = verify_proc_exit_fn(process_id, process_creation_time);
    } catch (const std::exception& e) {
        result.process_exit = {ProcessExitStatus::VerificationUnavailable,
            "Exception during process exit verification: " + std::string(e.what())};
    } catch (...) {
        result.process_exit = {ProcessExitStatus::VerificationUnavailable,
            "Unknown exception during process exit verification"};
    }
    const bool proc_exited = (result.process_exit.status == ProcessExitStatus::ConfirmedExited ||
                              result.process_exit.status == ProcessExitStatus::RecycledPidExited);

    // 4. Verify durable ownership journal state (must be EmptyVerified)
    if (load_journal_fn) {
        try {
            const auto journal_res = load_journal_fn();
            if (journal_res.state == fang::broker::JournalLoadState::VerifiedEmpty) {
                result.journal_final_state = "EmptyVerified";
                result.containment_empty_verified = true;
            } else {
                result.journal_final_state = "Unverified (" +
                    (journal_res.error.empty() ? "State " + std::to_string(static_cast<int>(journal_res.state)) : journal_res.error) + ")";
                result.containment_empty_verified = false;
            }
        } catch (const std::exception& e) {
            result.journal_final_state = "Exception loading journal: " + std::string(e.what());
            result.containment_empty_verified = false;
        } catch (...) {
            result.journal_final_state = "Unknown exception loading journal";
            result.containment_empty_verified = false;
        }
    } else {
        result.journal_final_state = "Unavailable (no journal load function)";
        result.containment_empty_verified = false;
    }

    // 5. Verify shared-memory mapping release
    try {
        result.mapping_released_verified = check_mapping_released_fn(wide_mapping);
    } catch (const std::exception&) {
        result.mapping_released_verified = false;
    } catch (...) {
        result.mapping_released_verified = false;
    }

    // 6. Aggregate overall cleanup verification
    result.cleanup_fully_verified = result.shutdown_requested &&
                                   result.shutdown_succeeded &&
                                   result.containment_empty_verified &&
                                   result.mapping_released_verified &&
                                   proc_exited;

    if (!result.cleanup_fully_verified) {
        std::vector<std::string> failures;
        if (!result.shutdown_requested) failures.push_back("supervisor shutdown request failed or was not accepted");
        if (!result.shutdown_succeeded) failures.push_back("supervisor shutdown not confirmed: " + shutdown_error);
        if (!result.containment_empty_verified) failures.push_back("journal not EmptyVerified: " + result.journal_final_state);
        if (!result.mapping_released_verified) failures.push_back("shared memory mapping not released");
        if (!proc_exited) failures.push_back("process exit unconfirmed: " + result.process_exit.detail);

        std::ostringstream ss;
        for (std::size_t i = 0; i < failures.size(); ++i) {
            if (i > 0) ss << "; ";
            ss << failures[i];
        }
        result.failure_reason = ss.str();
    }

    return result;
}

void apply_cleanup_result_to_report(
    const PostLaunchCleanupResult& cleanup,
    VideoSmokeReport& report) {

    report.shutdown_requested = cleanup.shutdown_requested;
    report.shutdown_elapsed_ms = cleanup.shutdown_elapsed_ms;
    report.process_exit_confirmed = (cleanup.process_exit.status == ProcessExitStatus::ConfirmedExited ||
                                     cleanup.process_exit.status == ProcessExitStatus::RecycledPidExited);
    report.process_exit_detail = cleanup.process_exit.detail;
    report.containment_empty_verified = cleanup.containment_empty_verified;
    report.journal_final_state = cleanup.journal_final_state;
    report.mapping_released_verified = cleanup.mapping_released_verified;
    report.cleanup_verified = cleanup.cleanup_fully_verified;
    report.cleanup_failure_reason = cleanup.failure_reason;

    if (report.outcome.empty()) {
        if (!cleanup.cleanup_fully_verified) {
            report.outcome = "FAIL — cleanup verification failed: " + cleanup.failure_reason;
            if (report.failure_reason.empty()) {
                report.failure_reason = cleanup.failure_reason;
            }
        }
    } else {
        if (!cleanup.cleanup_fully_verified) {
            report.outcome += " (cleanup unverified: " + cleanup.failure_reason + ")";
        }
    }
}

bool parse_video_smoke_options(int argc, wchar_t* argv[], VideoSmokeConfig& config,
    std::string& output_json_path, bool& allow_overwrite, std::string& error) {

    allow_overwrite = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--avd" && i + 1 < argc) {
            const std::wstring val(argv[++i]);
            config.avd_name.clear();
            for (const wchar_t c : val) config.avd_name.push_back(static_cast<char>(c));
        } else if (arg == L"--port" && i + 1 < argc) {
            config.emulator_port = static_cast<std::uint16_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--timeout-ms" && i + 1 < argc) {
            config.readiness_timeout_ms = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--observation-ms" && i + 1 < argc) {
            config.max_observation_ms = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--max-frames" && i + 1 < argc) {
            config.max_accepted_frames = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--shutdown-timeout-ms" && i + 1 < argc) {
            config.shutdown_timeout_ms = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--json" && i + 1 < argc) {
            const std::wstring val(argv[++i]);
            output_json_path.clear();
            for (const wchar_t c : val) output_json_path.push_back(static_cast<char>(c));
        } else if (arg == L"--overwrite") {
            allow_overwrite = true;
        } else {
            std::string narrow_arg;
            for (const wchar_t c : arg) narrow_arg.push_back(static_cast<char>(c));
            error = "Unknown or incomplete option: " + narrow_arg;
            return false;
        }
    }

    if (config.avd_name.empty()) {
        error = "Required option --avd <name> is missing.";
        return false;
    }

    return validate_video_smoke_config(config, error);
}

} // namespace fang::video_smoke
