#include "guest_endpoint_forwarder.hpp"
#include "adb_input_dispatcher.hpp"
#include "input_ipc_protocol.hpp"
#include "socket_input_transport.hpp"

#include <sstream>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstdlib>
#include <array>
#include <memory>
#include <span>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fang::studio {
// ============================================================================
// Simple process executor for ADB commands
// ============================================================================

namespace {

constexpr std::size_t max_captured_process_output = 64 * 1024;

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }
    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE handle = nullptr) noexcept {
        if (*this) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }
private:
    HANDLE handle_{};
};

struct ProcessResult {
    bool succeeded{false};
    bool timed_out{false};
    std::string error_code;
    std::string error;
    std::optional<std::uint32_t> exit_code;
    std::string stdout_output;
    std::string stderr_output;
    bool stdout_truncated{false};
    bool stderr_truncated{false};
};

struct CapturedPipe {
    std::string output;
    bool truncated{false};
    DWORD read_error{ERROR_SUCCESS};
};

std::wstring quote_windows_argument(std::wstring_view argument) {
    std::wstring quoted(1, L'"');
    std::size_t backslashes{};
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

bool utf8_to_wide(const std::string& value, std::wstring& output) {
    if (value.find('\0') != std::string::npos || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (value.empty()) {
        output.clear();
        return true;
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), count) == count;
}

std::wstring build_windows_command_line(const std::filesystem::path& executable,
                                        const std::vector<std::string>& args) {
    std::wstring command_line = quote_windows_argument(executable.wstring());
    for (const auto& arg : args) {
        std::wstring wide_arg;
        if (!utf8_to_wide(arg, wide_arg)) {
            throw std::invalid_argument("ADB argument is not valid UTF-8 or contains NUL.");
        }
        command_line.push_back(L' ');
        command_line += quote_windows_argument(wide_arg);
        if (command_line.size() >= 32767) {
            throw std::length_error("ADB command line exceeds the Windows process limit.");
        }
    }
    return command_line;
}

void drain_available_pipe(HANDLE pipe, CapturedPipe& capture, bool& closed) noexcept {
    if (closed) {
        return;
    }
    std::array<char, 8192> buffer{};
    DWORD available{};
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA) {
            capture.read_error = error;
        }
        closed = true;
        return;
    }
    if (available == 0) {
        return;
    }
    DWORD bytes_read{};
    const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
    if (!ReadFile(pipe, buffer.data(), requested, &bytes_read, nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_BROKEN_PIPE && error != ERROR_OPERATION_ABORTED) {
            capture.read_error = error;
        }
        closed = true;
        return;
    }
    if (bytes_read == 0) {
        closed = true;
        return;
    }
    const std::size_t remaining = max_captured_process_output - capture.output.size();
    const std::size_t copy_count = std::min<std::size_t>(remaining, bytes_read);
    if (copy_count != 0) {
        try {
            capture.output.append(buffer.data(), copy_count);
        } catch (...) {
            capture.read_error = ERROR_NOT_ENOUGH_MEMORY;
        }
    }
    if (copy_count < bytes_read) {
        capture.truncated = true;
    }
}

class AttributeList {
public:
    AttributeList() {
        SIZE_T size{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage_.resize(size);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        initialized_ = InitializeProcThreadAttributeList(list_, 1, 0, &size) != 0;
    }
    ~AttributeList() {
        if (initialized_) {
            DeleteProcThreadAttributeList(list_);
        }
    }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
    bool valid() const noexcept { return initialized_; }
    LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }
private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_{};
    bool initialized_{false};
};

class AdbProcessExecutor {
public:
    ProcessResult execute(const std::filesystem::path& executable,
                          const std::vector<std::string>& args,
                          std::chrono::milliseconds timeout) const {
        ProcessResult result;
        std::error_code path_error;
        if (executable.empty() || !std::filesystem::is_regular_file(executable, path_error)) {
            result.error_code = "ADB_NOT_FOUND";
            result.error = "ADB executable not found: " + executable.string();
            return result;
        }

        std::wstring command_line;
        try {
            command_line = build_windows_command_line(std::filesystem::absolute(executable), args);
        } catch (const std::exception& error) {
            result.error_code = "ADB_ARGUMENT_INVALID";
            result.error = error.what();
            return result;
        }

        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE stdout_read_raw{}, stdout_write_raw{}, stderr_read_raw{}, stderr_write_raw{};
        if (!CreatePipe(&stdout_read_raw, &stdout_write_raw, &security, 0) ||
            !CreatePipe(&stderr_read_raw, &stderr_write_raw, &security, 0)) {
            result.error_code = "ADB_PIPE_FAILED";
            result.error = "CreatePipe failed with error " + std::to_string(GetLastError());
            if (stdout_read_raw) CloseHandle(stdout_read_raw);
            if (stdout_write_raw) CloseHandle(stdout_write_raw);
            if (stderr_read_raw) CloseHandle(stderr_read_raw);
            if (stderr_write_raw) CloseHandle(stderr_write_raw);
            return result;
        }
        UniqueHandle stdout_read(stdout_read_raw);
        UniqueHandle stdout_write(stdout_write_raw);
        UniqueHandle stderr_read(stderr_read_raw);
        UniqueHandle stderr_write(stderr_write_raw);
        if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
            result.error_code = "ADB_PIPE_FAILED";
            result.error = "SetHandleInformation failed with error " + std::to_string(GetLastError());
            return result;
        }

        UniqueHandle stdin_handle(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!stdin_handle) {
            result.error_code = "ADB_PIPE_FAILED";
            result.error = "Opening NUL for child stdin failed with error " + std::to_string(GetLastError());
            return result;
        }

        AttributeList attributes;
        if (!attributes.valid()) {
            result.error_code = "ADB_PIPE_FAILED";
            result.error = "InitializeProcThreadAttributeList failed with error " + std::to_string(GetLastError());
            return result;
        }
        HANDLE inherited_handles[]{stdin_handle.get(), stdout_write.get(), stderr_write.get()};
        if (!UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited_handles, sizeof(inherited_handles), nullptr, nullptr)) {
            result.error_code = "ADB_PIPE_FAILED";
            result.error = "UpdateProcThreadAttribute failed with error " + std::to_string(GetLastError());
            return result;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = stdin_handle.get();
        startup.StartupInfo.hStdOutput = stdout_write.get();
        startup.StartupInfo.hStdError = stderr_write.get();
        startup.lpAttributeList = attributes.get();

        PROCESS_INFORMATION process_info{};
        const std::wstring executable_path = std::filesystem::absolute(executable).wstring();
        if (!CreateProcessW(executable_path.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                &startup.StartupInfo, &process_info)) {
            result.error_code = "ADB_LAUNCH_FAILED";
            result.error = "CreateProcessW failed with error " + std::to_string(GetLastError());
            return result;
        }
        UniqueHandle process(process_info.hProcess);
        UniqueHandle thread_handle(process_info.hThread);
        stdout_write.reset();
        stderr_write.reset();
        stdin_handle.reset();

        CapturedPipe stdout_capture;
        CapturedPipe stderr_capture;
        const auto timeout_count = std::max<std::int64_t>(0, timeout.count());
        const DWORD timeout_ms = timeout_count >= MAXDWORD ? MAXDWORD - 1 : static_cast<DWORD>(timeout_count);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        auto drain_remaining = [](HANDLE pipe, CapturedPipe& capture, bool& closed) noexcept {
            while (!closed) {
                DWORD available = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
                    break;
                }
                drain_available_pipe(pipe, capture, closed);
            }
        };

        bool stdout_closed = false;
        bool stderr_closed = false;
        for (;;) {
            drain_available_pipe(stdout_read.get(), stdout_capture, stdout_closed);
            drain_available_pipe(stderr_read.get(), stderr_capture, stderr_closed);

            const DWORD wait_result = WaitForSingleObject(process.get(), 0);
            if (wait_result == WAIT_OBJECT_0) {
                drain_remaining(stdout_read.get(), stdout_capture, stdout_closed);
                drain_remaining(stderr_read.get(), stderr_capture, stderr_closed);
                break;
            }
            if (wait_result == WAIT_FAILED) {
                const DWORD error = GetLastError();
                result.error_code = "ADB_WAIT_FAILED";
                result.error = "WaitForSingleObject failed with error " + std::to_string(error);
                TerminateProcess(process.get(), error);
                WaitForSingleObject(process.get(), INFINITE);
                drain_remaining(stdout_read.get(), stdout_capture, stdout_closed);
                drain_remaining(stderr_read.get(), stderr_capture, stderr_closed);
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result.timed_out = true;
                result.error_code = "ADB_TIMEOUT";
                result.error = "ADB command timed out after " + std::to_string(timeout.count()) + " ms.";
                if (!TerminateProcess(process.get(), ERROR_TIMEOUT)) {
                    result.error += " TerminateProcess failed with error " + std::to_string(GetLastError());
                }
                WaitForSingleObject(process.get(), INFINITE);
                drain_remaining(stdout_read.get(), stdout_capture, stdout_closed);
                drain_remaining(stderr_read.get(), stderr_capture, stderr_closed);
                break;
            }
            Sleep(1);
        }
        result.stdout_output = std::move(stdout_capture.output);
        result.stderr_output = std::move(stderr_capture.output);
        result.stdout_truncated = stdout_capture.truncated;
        result.stderr_truncated = stderr_capture.truncated;

        DWORD exit_code{};
        if (GetExitCodeProcess(process.get(), &exit_code)) {
            result.exit_code = exit_code;
        }
        if (stdout_capture.read_error != ERROR_SUCCESS || stderr_capture.read_error != ERROR_SUCCESS) {
            result.error_code = "ADB_READ_FAILED";
            const DWORD error = stdout_capture.read_error != ERROR_SUCCESS
                ? stdout_capture.read_error : stderr_capture.read_error;
            result.error = "Reading ADB output failed with error " + std::to_string(error);
        }
        if (!result.timed_out && result.error_code.empty()) {
            if (!result.exit_code) {
                result.error_code = "ADB_EXIT_CODE_FAILED";
                result.error = "GetExitCodeProcess failed with error " + std::to_string(GetLastError());
            } else if (*result.exit_code != 0) {
                result.error_code = "ADB_COMMAND_FAILED";
                result.error = "ADB command exited with code " + std::to_string(*result.exit_code);
            } else {
                result.succeeded = true;
            }
        }
        return result;
    }
};

void copy_process_result(TransportResult& target, const ProcessResult& source) {
    target.exit_code = source.exit_code;
    target.stdout_output = source.stdout_output;
    target.stderr_output = source.stderr_output;
    target.stdout_truncated = source.stdout_truncated;
    target.stderr_truncated = source.stderr_truncated;
}

void copy_process_result(TransportResult& target, const TransportResult& source) {
    target.exit_code = source.exit_code;
    target.stdout_output = source.stdout_output;
    target.stderr_output = source.stderr_output;
    target.stdout_truncated = source.stdout_truncated;
    target.stderr_truncated = source.stderr_truncated;
}

} // namespace

// ============================================================================
// LoopbackTestForwarder
// ============================================================================

LoopbackTestForwarder::LoopbackTestForwarder(uint16_t local_test_port)
    : local_test_port_(local_test_port) {}

bool LoopbackTestForwarder::setup_forwarding(
    const std::string& serial_or_avd,
    uint16_t guest_port,
    GuestEndpointMapping& out_mapping) {
    if (simulate_failure_) {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.mechanism = ForwardingMechanism::StubLoopback;
        out_mapping.diagnostic = "Simulated port forwarding failure.";
        out_mapping.is_live_forwarding_active = false;
        return false;
    }

    out_mapping.host_address = "127.0.0.1";
    out_mapping.host_port = local_test_port_;
    out_mapping.guest_port = guest_port;
    out_mapping.state = ForwardingState::Active;
    out_mapping.mechanism = ForwardingMechanism::StubLoopback;
    out_mapping.diagnostic = "Loopback test forwarder active for target " + serial_or_avd;
    out_mapping.is_live_forwarding_active = true;
    return true;
}

void LoopbackTestForwarder::teardown_forwarding(GuestEndpointMapping& mapping) {
    mapping.state = ForwardingState::NotConfigured;
    mapping.is_live_forwarding_active = false;
    mapping.diagnostic = "Loopback test forwarder torn down.";
}

bool LoopbackTestForwarder::is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept {
    return mapping.state == ForwardingState::Active && mapping.is_live_forwarding_active;
}

// ============================================================================
// AdbTransport
// ============================================================================

AdbTransport::AdbTransport(const AdbTransportConfig& config)
    : config_(config), live_authorized_(!config.require_explicit_authorization) {
    // Discover ADB executable if not provided
    if (config_.adb_executable.empty()) {
        config_.adb_executable = discover_adb_executable();
    }
}

std::filesystem::path AdbTransport::discover_adb_executable() {
    // Check common locations
    const char* android_home = std::getenv("ANDROID_HOME");
    if (android_home) {
        std::filesystem::path adb = std::filesystem::path(android_home) / "platform-tools" / "adb.exe";
        if (std::filesystem::exists(adb)) return adb;
    }
    
    const char* local_appdata = std::getenv("LOCALAPPDATA");
    if (local_appdata) {
        std::filesystem::path adb = std::filesystem::path(local_appdata) / "Android" / "Sdk" / "platform-tools" / "adb.exe";
        if (std::filesystem::exists(adb)) return adb;
    }
    
    // Check PATH
    return "adb.exe"; // Will be found via PATH
}

bool AdbTransport::verify_adb_available() const {
    if (!std::filesystem::exists(config_.adb_executable)) {
        return false;
    }
    return true;
}

std::string AdbTransport::format_adb_forward_cmd(const std::string& serial, uint16_t host_port, uint16_t guest_port) {
    std::ostringstream ss;
    ss << "adb -s " << serial << " forward tcp:" << host_port << " tcp:" << guest_port;
    return ss.str();
}

std::string AdbTransport::format_adb_push_cmd(const std::string& serial, const std::string& local, const std::string& remote) {
    std::ostringstream ss;
    ss << "adb -s " << serial << " push \"" << local << "\" \"" << remote << "\"";
    return ss.str();
}

std::string AdbTransport::format_adb_shell_cmd(const std::string& serial, const std::string& command) {
    std::ostringstream ss;
    ss << "adb -s " << serial << " shell " << command;
    return ss.str();
}

TransportResult AdbTransport::execute_adb_command(const std::vector<std::string>& args, std::chrono::milliseconds timeout, std::string* output) const {
    if (!std::filesystem::exists(config_.adb_executable)) {
        return TransportResult::fail("ADB_NOT_FOUND", "ADB executable not found or not functional", config_.adb_executable.string());
    }

    std::vector<std::string> full_args;
    if (!config_.target_serial.empty()) {
        full_args.push_back("-s");
        full_args.push_back(config_.target_serial);
    }
    full_args.insert(full_args.end(), args.begin(), args.end());

    AdbProcessExecutor executor;
    auto result = executor.execute(config_.adb_executable, full_args, timeout);
    
    if (output) {
        *output = result.stdout_output;
    }
    const std::string detail = !result.stderr_output.empty() ? result.stderr_output
        : (!result.stdout_output.empty() ? result.stdout_output : result.error);
    TransportResult transport_result = result.succeeded
        ? TransportResult::ok("Command succeeded")
        : TransportResult::fail(result.error_code,
            result.error.empty() ? "ADB command failed." : result.error,
            detail);
    copy_process_result(transport_result, result);
    return transport_result;
}

bool AdbTransport::setup_forwarding(
    const std::string& serial_or_avd,
    uint16_t guest_port,
    GuestEndpointMapping& out_mapping) {
    
    // Validate target serial syntax
    if (serial_or_avd.empty() || !AdbInputDispatcher::is_valid_adb_serial(serial_or_avd)) {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.mechanism = ForwardingMechanism::AdbPortForward;
        out_mapping.diagnostic = "Invalid target ADB serial: '" + serial_or_avd + "'";
        out_mapping.is_live_forwarding_active = false;
        return false;
    }

    // Allocate host port
    uint16_t host_p = config_.guest_port;
    if (config_.target_serial.empty()) {
        host_p = guest_port;
    }

    out_mapping.host_address = "127.0.0.1";
    out_mapping.host_port = host_p;
    out_mapping.guest_port = guest_port;
    out_mapping.mechanism = ForwardingMechanism::AdbPortForward;

    if (!live_authorized_) {
        out_mapping.state = ForwardingState::Blocked;
        out_mapping.is_live_forwarding_active = false;
        out_mapping.diagnostic = "ADB port forwarding contract formulated (" +
            format_adb_forward_cmd(serial_or_avd, host_p, guest_port) +
            "); live ADB server execution requires explicit authorization.";
        return false;
    }

    // Execute live ADB forward
    auto result = execute_adb_command({"forward", "tcp:" + std::to_string(host_p), "tcp:" + std::to_string(guest_port)}, config_.command_timeout);
    if (!result.success) {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.is_live_forwarding_active = false;
        out_mapping.diagnostic = "ADB forward failed: " + result.diagnostic + " (" + result.detail + ")";
        return false;
    }

    out_mapping.state = ForwardingState::Active;
    out_mapping.is_live_forwarding_active = true;
    out_mapping.diagnostic = "ADB port forwarding active: " + format_adb_forward_cmd(serial_or_avd, host_p, guest_port);
    return true;
}

void AdbTransport::teardown_forwarding(GuestEndpointMapping& mapping) {
    if (live_authorized_ && mapping.is_live_forwarding_active && !config_.target_serial.empty()) {
        execute_adb_command({"forward", "--remove", "tcp:" + std::to_string(mapping.host_port)}, config_.command_timeout);
    }
    mapping.state = ForwardingState::NotConfigured;
    mapping.is_live_forwarding_active = false;
    mapping.diagnostic = "ADB port forwarding released.";
}

bool AdbTransport::is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept {
    return mapping.state == ForwardingState::Active && mapping.is_live_forwarding_active;
}

TransportResult AdbTransport::push_binary(
    const std::string& serial_or_avd,
    const std::filesystem::path& local_artifact_path,
    const std::string& guest_staging_path,
    uint64_t expected_size,
    const std::string& expected_sha256) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Binary push requires explicit authorization", "Set authorization token to enable live operations");
    }
    if (!std::filesystem::exists(local_artifact_path)) {
        return TransportResult::fail("ARTIFACT_NOT_FOUND", "Local artifact not found", local_artifact_path.string());
    }
    if (std::filesystem::file_size(local_artifact_path) != expected_size) {
        return TransportResult::fail("SIZE_MISMATCH", "Local artifact size mismatch", 
            "Expected: " + std::to_string(expected_size) + ", Actual: " + std::to_string(std::filesystem::file_size(local_artifact_path)));
    }

    // Push binary
    auto result = execute_adb_command({"push", local_artifact_path.string(), guest_staging_path}, config_.push_timeout);
    if (!result.success) {
        TransportResult failure = TransportResult::fail(
            result.error_code.empty() ? "PUSH_FAILED" : result.error_code,
            "ADB push failed: " + result.diagnostic,
            result.detail);
        copy_process_result(failure, result);
        return failure;
    }

    // Verify remote integrity
    return verify_remote_artifact(serial_or_avd, guest_staging_path, expected_size, expected_sha256);
}

TransportResult AdbTransport::verify_remote_artifact(
    const std::string& serial_or_avd,
    const std::string& guest_staging_path,
    uint64_t expected_size,
    const std::string& expected_sha256) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Remote verification requires explicit authorization", "");
    }

    // Check file size
    std::string output;
    auto size_result = execute_adb_command({"shell", "stat", "-c", "%s", guest_staging_path}, config_.command_timeout, &output);
    if (!size_result.success) {
        size_result.diagnostic = "Failed to stat remote file: " + size_result.diagnostic;
        return size_result;
    }
    
    uint64_t remote_size = 0;
    try {
        remote_size = std::stoull(output);
    } catch (...) {
        return TransportResult::fail("PARSE_FAILED", "Failed to parse remote file size", output);
    }
    
    if (remote_size != expected_size) {
        TransportResult mismatch = TransportResult::fail("SIZE_MISMATCH", "Remote artifact size mismatch",
            "Expected: " + std::to_string(expected_size) + ", Actual: " + std::to_string(remote_size));
        copy_process_result(mismatch, size_result);
        return mismatch;
    }

    // Check SHA-256
    auto sha_result = execute_adb_command({"shell", "sha256sum", guest_staging_path}, config_.command_timeout, &output);
    if (!sha_result.success) {
        sha_result.diagnostic = "Failed to compute remote SHA-256: " + sha_result.diagnostic;
        return sha_result;
    }
    
    // Parse sha256sum output: "hash  filepath"
    std::istringstream iss(output);
    std::string remote_sha;
    iss >> remote_sha;
    
    if (remote_sha != expected_sha256) {
        TransportResult mismatch = TransportResult::fail("SHA_MISMATCH", "Remote artifact SHA-256 mismatch",
            "Expected: " + expected_sha256 + ", Actual: " + remote_sha);
        copy_process_result(mismatch, sha_result);
        return mismatch;
    }

    TransportResult verified = TransportResult::ok("Remote artifact verified: size=" + std::to_string(remote_size) + ", sha256=" + remote_sha);
    copy_process_result(verified, sha_result);
    return verified;
}

TransportResult AdbTransport::start_daemon(
    const std::string& serial_or_avd,
    const std::string& guest_staging_path,
    const std::string& guest_port,
    const std::string& session_id,
    const std::string& generation_id) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Daemon startup requires explicit authorization", "");
    }

    // Make executable
    auto chmod_result = execute_adb_command({"shell", "chmod", "755", guest_staging_path}, config_.command_timeout);
    if (!chmod_result.success) {
        chmod_result.diagnostic = "Failed to make daemon executable: " + chmod_result.diagnostic;
        return chmod_result;
    }

    // Start daemon - the artifact is a native ELF binary, not a Java class
    // Execute directly with --port argument and required configuration
    // Use session_id and generation_id from deployment context for protocol handshake
    std::string cmd = guest_staging_path + " --port " + guest_port + 
        " --session-id " + session_id + 
        " --generation-id " + generation_id + 
        " --backend recording";
    auto exec_result = execute_adb_command({"shell", cmd + " &"}, config_.startup_timeout);
    if (!exec_result.success) {
        exec_result.diagnostic = "Failed to start guest daemon: " + exec_result.diagnostic;
        return exec_result;
    }

    // Give the daemon a moment to bind to the port
    execute_adb_command({"shell", "sleep", "1"}, std::chrono::milliseconds(2000));

    // Verify the process is actually running by checking for the exact command line
    // Use pgrep with -f to match the full command line including session/generation IDs
    // NO FALLBACK to binary-name-only matching - that can match stale or other instances
    std::string pgrep_cmd = "pgrep -f \"" + guest_staging_path + ".*--session-id " + session_id + ".*--generation-id " + generation_id + "\"";
    auto ps_result = execute_adb_command({"shell", pgrep_cmd}, config_.command_timeout);
    
    if (!ps_result.success || ps_result.detail.empty()) {
        // Process with matching session/generation IDs not found
        // Do NOT fall back to binary-name-only matching - that would match stale or other instances
        return TransportResult::fail("DAEMON_PROCESS_NOT_FOUND", 
            "Daemon process with matching session/generation IDs not found after startup",
            "Command: " + cmd + "; pgrep: " + pgrep_cmd);
    }

    TransportResult result;
    result.success = true;
    result.lifecycle_state = TransportResult::LifecycleState::ProcessObserved;
    result.diagnostic = "Daemon process observed: " + cmd + " (PID: " + ps_result.detail + ")";
    result.detail = ps_result.detail;
    return result;
}

TransportResult AdbTransport::verify_connection(
    const std::string& serial_or_avd,
    uint16_t host_port,
    std::chrono::milliseconds timeout,
    const std::string& session_id,
    const std::string& generation_id) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Connection verification requires explicit authorization", "");
    }

    // Connect to the forwarded port and perform Hello/HelloAck handshake
    // This proves the daemon is listening, initialized, and protocol-compatible

    SocketTransportConfig transport_config;
    transport_config.host = "127.0.0.1";
    transport_config.port = host_port;
    transport_config.connect_timeout_ms = static_cast<uint32_t>(timeout.count());
    transport_config.send_timeout_ms = 1000;
    transport_config.tcp_nodelay = true;

    SocketInputTransport transport(transport_config);

    // Connect with timeout
    if (!transport.connect()) {
        return TransportResult::fail("CONNECTION_FAILED", 
            "Failed to connect to guest daemon on forwarded port " + std::to_string(host_port),
            transport.error_diagnostic());
    }

    // Build Hello message with expected session/generation IDs
    // These should match what the deployment coordinator configured
    HelloPayload hello_payload;
    hello_payload.client_version = input_protocol_version;
    hello_payload.display_width = 1080;
    hello_payload.display_height = 1920;
    hello_payload.session_id = session_id;
    hello_payload.generation_id = generation_id;

    uint64_t seq = 1;
    uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    auto hello_packet = encode_hello_message(seq, ts, hello_payload);

    // Send Hello
    std::span<const std::byte> hello_span(hello_packet.data(), hello_packet.data() + hello_packet.size());
    if (!transport.send_bytes(hello_span)) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_SEND_FAILED", 
            "Failed to send Hello message to guest daemon",
            transport.error_diagnostic());
    }

    // Receive HelloAck with timeout
    auto response = transport.receive_bytes(4096);
    if (response.empty()) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_TIMEOUT", 
            "No response from guest daemon within timeout",
            "Waited " + std::to_string(timeout.count()) + "ms");
    }

    // Decode response header
    if (response.size() < input_wire_header_size) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_MALFORMED", 
            "Response too small for wire header",
            "Got " + std::to_string(response.size()) + " bytes, need at least " + std::to_string(input_wire_header_size));
    }

    WireHeader resp_header;
    std::span<const std::byte> header_span(response.data(), response.data() + input_wire_header_size);
    auto decode_result = decode_wire_header(header_span, resp_header);
    if (decode_result != InputProtocolError::None) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_INVALID_HEADER", 
            "Failed to decode response header: " + std::string(input_protocol_error_name(decode_result)),
            "Magic: " + std::to_string(resp_header.magic) + ", Version: " + std::to_string(resp_header.version));
    }

    if (resp_header.type != InputMessageType::HelloAck) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_UNEXPECTED_MESSAGE", 
            "Expected HelloAck, got " + std::string(input_message_type_name(resp_header.type)),
            "Sequence: " + std::to_string(resp_header.sequence_number));
    }

    // Decode HelloAck payload
    std::span<const std::byte> payload_span(response.data() + input_wire_header_size, response.data() + input_wire_header_size + resp_header.payload_size);
    InputProtocolError payload_error;
    auto hello_ack = decode_hello_ack_message(payload_span, payload_error);
    if (!hello_ack.has_value()) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_DECODE_FAILED", 
            "Failed to decode HelloAck payload: " + std::string(input_protocol_error_name(payload_error)),
            "");
    }

    // Validate handshake response
    if (hello_ack->status != InputProtocolError::None) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_REJECTED", 
            "Guest daemon rejected handshake: " + std::string(input_protocol_error_name(hello_ack->status)),
            "Session: " + hello_ack->session_id + ", Generation: " + hello_ack->generation_id);
    }

    if (hello_ack->session_id != session_id) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_SESSION_MISMATCH", 
            "Session ID mismatch in HelloAck",
            "Expected: " + session_id + ", Got: " + hello_ack->session_id);
    }

    if (hello_ack->generation_id != generation_id) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_GENERATION_MISMATCH", 
            "Generation ID mismatch in HelloAck",
            "Expected: " + generation_id + ", Got: " + hello_ack->generation_id);
    }

    if (hello_ack->guest_version != input_protocol_version) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_VERSION_MISMATCH", 
            "Protocol version mismatch",
            "Expected: " + std::to_string(input_protocol_version) + ", Got: " + std::to_string(hello_ack->guest_version));
    }

    // Handshake successful - daemon is ready
    transport.disconnect();
    TransportResult result = TransportResult::ok("Readiness handshake successful: Hello/HelloAck exchanged, session=" + hello_ack->session_id + ", generation=" + hello_ack->generation_id + ", max_contacts=" + std::to_string(hello_ack->max_contacts));
    result.lifecycle_state = TransportResult::LifecycleState::ProtocolReady;
    return result;
}

TransportResult AdbTransport::validate_target(
    const std::string& serial_or_avd,
    fang::guest::GuestTargetAbi expected_abi) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Target validation requires explicit authorization", "");
    }

    // Check device is online
    std::string output;
    auto devices_result = execute_adb_command({"devices"}, config_.command_timeout, &output);
    if (!devices_result.success) {
        devices_result.diagnostic = "Failed to list ADB devices: " + devices_result.diagnostic;
        return devices_result;
    }

    // Check our target serial is in the list and online
    bool found = false;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find(serial_or_avd) == 0 && line.find("\tdevice") != std::string::npos) {
            found = true;
            break;
        }
    }
    if (!found) {
        return TransportResult::fail("DEVICE_OFFLINE", "Target device not found or offline", "Serial: " + serial_or_avd);
    }

    // Check ABI via ro.product.cpu.abi
    auto abi_result = execute_adb_command({"shell", "getprop", "ro.product.cpu.abi"}, config_.command_timeout, &output);
    if (!abi_result.success) {
        abi_result.diagnostic = "Failed to get device ABI: " + abi_result.diagnostic;
        return abi_result;
    }

    std::string device_abi = output;
    device_abi.erase(std::remove_if(device_abi.begin(), device_abi.end(), ::isspace), device_abi.end());

    std::string expected_abi_str;
    switch (expected_abi) {
    case fang::guest::GuestTargetAbi::X86_64: expected_abi_str = "x86_64"; break;
    case fang::guest::GuestTargetAbi::Arm64_v8a: expected_abi_str = "arm64-v8a"; break;
    case fang::guest::GuestTargetAbi::X86: expected_abi_str = "x86"; break;
    case fang::guest::GuestTargetAbi::Armeabi_v7a: expected_abi_str = "armeabi-v7a"; break;
    default: expected_abi_str = "unknown"; break;
    }

    if (device_abi != expected_abi_str) {
        TransportResult mismatch = TransportResult::fail("ABI_MISMATCH", "Device ABI does not match expected artifact ABI",
            "Expected: " + expected_abi_str + ", Device: " + device_abi);
        mismatch.stdout_output = abi_result.stdout_output;
        mismatch.stderr_output = abi_result.stderr_output;
        mismatch.exit_code = abi_result.exit_code;
        mismatch.stdout_truncated = abi_result.stdout_truncated;
        mismatch.stderr_truncated = abi_result.stderr_truncated;
        return mismatch;
    }

    TransportResult validated = TransportResult::ok("Target validated: " + device_abi);
    copy_process_result(validated, abi_result);
    return validated;
}

// ============================================================================
// AdbTransport::set_authorization
// ============================================================================

void AdbTransport::set_authorization(std::string token) {
    if (config_.require_explicit_authorization && !config_.authorization_token.empty()) {
        // Use constant-time comparison to prevent timing attacks
        live_authorized_ = constant_time_equals(token, config_.authorization_token);
        // Clear the token from memory immediately
        std::fill(token.begin(), token.end(), '\0');
    } else if (!config_.require_explicit_authorization) {
        live_authorized_ = true;
    }
}

// Constant-time string comparison to prevent timing attacks
bool AdbTransport::constant_time_equals(const std::string& a, const std::string& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    std::uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// ============================================================================
// FakeAdbTransport
// ============================================================================

bool FakeAdbTransport::setup_forwarding(
    const std::string& serial_or_avd,
    uint16_t guest_port,
    GuestEndpointMapping& out_mapping) {

    log_call("setup_forwarding", serial_or_avd, "guest_port=" + std::to_string(guest_port));

    // Basic serial validation (match AdbTransport behavior)
    if (serial_or_avd.empty() || serial_or_avd.find(';') != std::string::npos || 
        serial_or_avd.find('&') != std::string::npos) {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.mechanism = ForwardingMechanism::AdbPortForward;
        out_mapping.diagnostic = "Invalid target ADB serial: '" + serial_or_avd + "'";
        out_mapping.is_live_forwarding_active = false;
        return false;
    }

    if (setup_success_) {
        out_mapping.host_address = "127.0.0.1";
        out_mapping.host_port = guest_port;
        out_mapping.guest_port = guest_port;
        out_mapping.state = ForwardingState::Active;
        out_mapping.mechanism = ForwardingMechanism::AdbPortForward;
        out_mapping.diagnostic = setup_diag_;
        out_mapping.is_live_forwarding_active = true;
        return true;
    } else {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.mechanism = ForwardingMechanism::AdbPortForward;
        out_mapping.diagnostic = setup_diag_;
        out_mapping.is_live_forwarding_active = false;
        return false;
    }
}

void FakeAdbTransport::teardown_forwarding(GuestEndpointMapping& mapping) {
    log_call("teardown_forwarding", "", "host_port=" + std::to_string(mapping.host_port));
    mapping.state = ForwardingState::NotConfigured;
    mapping.is_live_forwarding_active = false;
    mapping.diagnostic = "Fake ADB forwarding released.";
}

bool FakeAdbTransport::is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept {
    return mapping.state == ForwardingState::Active && mapping.is_live_forwarding_active;
}

TransportResult FakeAdbTransport::push_binary(
    const std::string& serial_or_avd,
    const std::filesystem::path& local_artifact_path,
    const std::string& guest_staging_path,
    uint64_t expected_size,
    const std::string& expected_sha256) {

    log_call("push_binary", serial_or_avd, local_artifact_path.string() + " -> " + guest_staging_path);
    return push_result_;
}

TransportResult FakeAdbTransport::verify_remote_artifact(
    const std::string& serial_or_avd,
    const std::string& guest_staging_path,
    uint64_t expected_size,
    const std::string& expected_sha256) {

    log_call("verify_remote_artifact", serial_or_avd, guest_staging_path);
    return verify_result_;
}

TransportResult FakeAdbTransport::start_daemon(
    const std::string& serial_or_avd,
    const std::string& guest_staging_path,
    const std::string& guest_port,
    const std::string& session_id,
    const std::string& generation_id) {

    log_call("start_daemon", serial_or_avd, guest_staging_path + " port=" + guest_port + " session=" + session_id + " gen=" + generation_id);
    
    // Store expected IDs for handshake validation
    expected_session_id_ = session_id;
    expected_generation_id_ = generation_id;
    has_expected_ids_ = true;
    
    // Set lifecycle state to ProcessObserved for fake transport
    TransportResult result = start_daemon_result_;
    result.lifecycle_state = TransportResult::LifecycleState::ProcessObserved;
    return result;
}

TransportResult FakeAdbTransport::verify_connection(
    const std::string& serial_or_avd,
    uint16_t host_port,
    std::chrono::milliseconds timeout,
    const std::string& session_id,
    const std::string& generation_id) {

    log_call("verify_connection", serial_or_avd, "host_port=" + std::to_string(host_port) + " timeout=" + std::to_string(timeout.count()) + " session=" + session_id + " gen=" + generation_id);
    
    // Validate session/generation IDs match what was set in start_daemon
    if (has_expected_ids_) {
        if (session_id != expected_session_id_) {
            return TransportResult::fail("HANDSHAKE_SESSION_MISMATCH", 
                "Session ID mismatch in Hello message",
                "Expected: " + expected_session_id_ + ", Got: " + session_id);
        }
        if (generation_id != expected_generation_id_) {
            return TransportResult::fail("HANDSHAKE_GENERATION_MISMATCH", 
                "Generation ID mismatch in Hello message",
                "Expected: " + expected_generation_id_ + ", Got: " + generation_id);
        }
    }
    
    if (!handshake_success_) {
        return TransportResult::fail(handshake_error_code_, handshake_diagnostic_, "");
    }
    
    TransportResult result = verify_connection_result_;
    result.lifecycle_state = TransportResult::LifecycleState::ProtocolReady;
    return result;
}

TransportResult FakeAdbTransport::validate_target(
    const std::string& serial_or_avd,
    fang::guest::GuestTargetAbi expected_abi) {

    log_call("validate_target", serial_or_avd, "expected_abi=" + std::to_string(static_cast<int>(expected_abi)));
    
    if (!device_online_) {
        return TransportResult::fail("DEVICE_OFFLINE", "Fake device is offline", "");
    }
    
    if (serial_or_avd != device_serial_) {
        return TransportResult::fail("DEVICE_NOT_FOUND", "Fake device serial mismatch", "Expected: " + device_serial_ + ", Got: " + serial_or_avd);
    }
    
    std::string expected_abi_str;
    switch (expected_abi) {
    case fang::guest::GuestTargetAbi::X86_64: expected_abi_str = "x86_64"; break;
    case fang::guest::GuestTargetAbi::Arm64_v8a: expected_abi_str = "arm64-v8a"; break;
    case fang::guest::GuestTargetAbi::X86: expected_abi_str = "x86"; break;
    case fang::guest::GuestTargetAbi::Armeabi_v7a: expected_abi_str = "armeabi-v7a"; break;
    default: expected_abi_str = "unknown"; break;
    }
    
    std::string device_abi_str;
    switch (device_abi_) {
    case fang::guest::GuestTargetAbi::X86_64: device_abi_str = "x86_64"; break;
    case fang::guest::GuestTargetAbi::Arm64_v8a: device_abi_str = "arm64-v8a"; break;
    case fang::guest::GuestTargetAbi::X86: device_abi_str = "x86"; break;
    case fang::guest::GuestTargetAbi::Armeabi_v7a: device_abi_str = "armeabi-v7a"; break;
    default: device_abi_str = "unknown"; break;
    }
    
    if (device_abi_str != expected_abi_str) {
        return TransportResult::fail("ABI_MISMATCH", "Fake device ABI mismatch", "Expected: " + expected_abi_str + ", Device: " + device_abi_str);
    }
    
    return validate_target_result_;
}

void FakeAdbTransport::log_call(const std::string& op, const std::string& serial, const std::string& detail) {
    CallLog log;
    log.operation = op;
    log.serial = serial;
    log.detail = detail;
    log.timestamp = std::chrono::system_clock::now();
    call_log_.push_back(log);
}

// ============================================================================
// BlockedGuestForwarder
// ============================================================================

BlockedGuestForwarder::BlockedGuestForwarder(std::string blocker_reason)
    : blocker_reason_(std::move(blocker_reason)) {}

bool BlockedGuestForwarder::setup_forwarding(
    const std::string& /*serial_or_avd*/,
    uint16_t guest_port,
    GuestEndpointMapping& out_mapping) {
    out_mapping.host_address = "127.0.0.1";
    out_mapping.host_port = 0;
    out_mapping.guest_port = guest_port;
    out_mapping.state = ForwardingState::Blocked;
    out_mapping.mechanism = ForwardingMechanism::Blocked;
    out_mapping.is_live_forwarding_active = false;
    out_mapping.diagnostic = blocker_reason_;
    return false;
}

void BlockedGuestForwarder::teardown_forwarding(GuestEndpointMapping& mapping) {
    mapping.state = ForwardingState::NotConfigured;
    mapping.is_live_forwarding_active = false;
}

bool BlockedGuestForwarder::is_forwarding_active(const GuestEndpointMapping& /*mapping*/) const noexcept {
    return false;
}

} // namespace fang::studio