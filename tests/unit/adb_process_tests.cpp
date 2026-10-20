#include "guest_endpoint_forwarder.hpp"
#include "adb_input_dispatcher.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fang::studio {

bool AdbInputDispatcher::is_valid_adb_serial(std::string_view serial) noexcept {
    if (serial.empty() || serial.size() > 64) {
        return false;
    }
    for (const char value : serial) {
        const bool alphanumeric = (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
        const bool punctuation = value == '-' || value == '_' || value == '.' || value == ':';
        if (!alphanumeric && !punctuation) {
            return false;
        }
    }
    return true;
}

} // namespace fang::studio

namespace {

using namespace fang::studio;

int failures = 0;

void check(bool condition, std::string_view label) {
    std::cout << (condition ? "PASS " : "FAIL ") << label << '\n';
    if (!condition) {
        ++failures;
    }
}

void set_mode(std::string_view mode, const std::filesystem::path& log_path) {
    _putenv_s("FANG_FAKE_ADB_MODE", std::string(mode).c_str());
    _putenv_s("FANG_FAKE_ADB_LOG", log_path.string().c_str());
}

bool log_contains(const std::filesystem::path& log_path, std::string_view value) {
    std::ifstream input(log_path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return contents.find(value) != std::string::npos;
}

DWORD last_pid_for_mode(const std::filesystem::path& log_path, std::string_view mode) {
    std::ifstream input(log_path, std::ios::binary);
    std::string line;
    DWORD pid{};
    while (std::getline(input, line)) {
        if (line.rfind("PID ", 0) == 0 && line.find(" MODE " + std::string(mode)) != std::string::npos) {
            pid = static_cast<DWORD>(std::stoul(line.substr(4)));
        }
    }
    return pid;
}

bool process_has_exited(DWORD process_id) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) {
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    DWORD exit_code{};
    const bool exited = GetExitCodeProcess(process, &exit_code) != 0 && exit_code != STILL_ACTIVE;
    CloseHandle(process);
    return exited;
}

bool start_sentinel(const std::filesystem::path& executable, PROCESS_INFORMATION& process_info) {
    std::wstring command_line = L"\"" + executable.wstring() + L"\" --sentinel";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    return CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process_info) != 0;
}

AdbTransport make_transport(const std::filesystem::path& executable,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
    AdbTransportConfig config;
    config.adb_executable = executable;
    config.target_serial = "emulator-5554";
    config.require_explicit_authorization = false;
    config.command_timeout = timeout;
    config.push_timeout = timeout;
    return AdbTransport(config);
}

void run_case(std::string_view label, const std::function<void()>& test) {
    try {
        test();
    } catch (const std::exception& error) {
        check(false, std::string(label) + ": " + error.what());
    }
}

void test_paths_and_arguments(const std::filesystem::path& fake_adb,
                              const std::filesystem::path& root,
                              const std::filesystem::path& log_path) {
    check(fake_adb.string().find(' ') != std::string::npos, "fake executable path contains spaces");
    auto transport = make_transport(fake_adb);
    set_mode("normal", log_path);
    auto validation = transport.validate_target("emulator-5554", fang::guest::GuestTargetAbi::X86_64);
    check(validation.success, "space-containing executable launches and validates target");

    const auto artifact = root / "artifact &^%() with spaces.bin";
    {
        std::ofstream output(artifact, std::ios::binary | std::ios::trunc);
        output << "payload\n";
    }
    const std::string expected_sha(64, 'a');
    auto pushed = transport.push_binary("emulator-5554", artifact,
        "/data/local/tmp/fang &|^%() daemon", 8, expected_sha);
    if (!pushed.success) {
        std::cout << "OBSERVATION push_error=" << pushed.error_code << " diagnostic="
                  << pushed.diagnostic << " detail=" << pushed.detail << '\n';
    }
    std::error_code copy_error;
    std::filesystem::copy_file(log_path, root / "push-argv.log",
        std::filesystem::copy_options::overwrite_existing, copy_error);
    check(pushed.success, "push and remote integrity verify with metacharacters in paths");
    check(log_contains(log_path, artifact.string()), "local path passed literally as a child argument");
    check(log_contains(log_path, "/data/local/tmp/fang &|^%() daemon"), "remote path passed literally as a child argument");
    check(log_contains(log_path, "ARG 4 " + std::to_string(artifact.string().size()) + " " + artifact.string()),
        "artifact argv entry preserves shell metacharacters literally");
    const std::string remote_path = "/data/local/tmp/fang &|^%() daemon";
    check(log_contains(log_path, "ARG 5 " + std::to_string(remote_path.size()) + " " + remote_path),
        "remote argv entry preserves shell metacharacters literally");
}

void test_exit_and_missing_executable(const std::filesystem::path& fake_adb,
                                      const std::filesystem::path& root,
                                      const std::filesystem::path& log_path) {
    auto transport = make_transport(fake_adb);
    set_mode("nonzero", log_path);
    auto nonzero = transport.validate_target("emulator-5554", fang::guest::GuestTargetAbi::X86_64);
    check(!nonzero.success && nonzero.error_code == "ADB_COMMAND_FAILED", "nonzero child exit retains command failure code");
        check(nonzero.exit_code.has_value() && nonzero.exit_code.value() == 23,
            "nonzero child exit code is structured");
    check(nonzero.stderr_output.find("FAKE_NONZERO_STDERR") != std::string::npos, "nonzero stderr is structured");

    auto missing = make_transport(root / "missing-adb.exe");
    auto missing_result = missing.validate_target("emulator-5554", fang::guest::GuestTargetAbi::X86_64);
    check(!missing_result.success && missing_result.error_code == "ADB_NOT_FOUND", "missing executable retains actionable error code");

    const auto invalid_executable = root / "invalid-adb.exe";
    { std::ofstream invalid(invalid_executable, std::ios::binary | std::ios::trunc); invalid << "not an executable"; }
    auto invalid = make_transport(invalid_executable);
    auto launch_failure = invalid.validate_target("emulator-5554", fang::guest::GuestTargetAbi::X86_64);
    check(!launch_failure.success && launch_failure.error_code == "ADB_LAUNCH_FAILED", "invalid executable returns launch error");
    check(!launch_failure.detail.empty(), "launch error includes native diagnostic");
}

void test_timeout(const std::filesystem::path& fake_adb, const std::filesystem::path& log_path) {
    auto transport = make_transport(fake_adb, std::chrono::milliseconds(100));
    set_mode("timeout", log_path);
    const auto start = std::chrono::steady_clock::now();
    auto result = transport.validate_target("emulator-5554", fang::guest::GuestTargetAbi::X86_64);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    check(!result.success && result.error_code == "ADB_TIMEOUT", "timed out child maps to ADB_TIMEOUT");
    check(elapsed < std::chrono::milliseconds(700), "timeout is enforced before fake child finishes");
    check(process_has_exited(last_pid_for_mode(log_path, "timeout")), "timed-out child is terminated and reaped");
}

void test_timeout_does_not_terminate_unrelated_process(const std::filesystem::path& fake_adb,
                                                      const std::filesystem::path& log_path) {
    _putenv_s("FANG_FAKE_ADB_MODE", "sentinel");
    _putenv_s("FANG_FAKE_ADB_LOG", log_path.string().c_str());
    PROCESS_INFORMATION sentinel{};
    check(start_sentinel(fake_adb, sentinel), "unrelated sentinel process starts");
    if (!sentinel.hProcess) {
        return;
    }
    const DWORD sentinel_pid = sentinel.dwProcessId;
    auto transport = make_transport(fake_adb, std::chrono::milliseconds(100));
    set_mode("timeout", log_path);
    auto result = transport.validate_target("emulator-5554", fang::guest::GuestTargetAbi::X86_64);
    DWORD sentinel_exit{};
    const bool sentinel_running = GetExitCodeProcess(sentinel.hProcess, &sentinel_exit) != 0 && sentinel_exit == STILL_ACTIVE;
    check(!result.success && result.error_code == "ADB_TIMEOUT", "timeout result returned while sentinel is active");
    check(sentinel_running, "timeout does not terminate unrelated process");
    TerminateProcess(sentinel.hProcess, 0);
    WaitForSingleObject(sentinel.hProcess, 2000);
    CloseHandle(sentinel.hThread);
    CloseHandle(sentinel.hProcess);
    std::cout << "OBSERVATION sentinel_pid=" << sentinel_pid << '\n';
}

void test_output_capture_limits(const std::filesystem::path& fake_adb,
                                const std::filesystem::path& log_path) {
    auto transport = make_transport(fake_adb, std::chrono::milliseconds(5000));
    set_mode("streams", log_path);
    auto result = transport.validate_target("emulator-5554", fang::guest::GuestTargetAbi::X86_64);
    check(!result.success && result.error_code == "ABI_MISMATCH", "simultaneous stdout/stderr target query completes");
    check(result.stdout_output.size() == 64 * 1024, "stdout capture is bounded to 64 KiB");
    check(result.stderr_output.size() == 64 * 1024, "stderr capture is bounded to 64 KiB");
    check(result.stdout_truncated && result.stderr_truncated, "both truncation flags are set");
    check(result.stderr_output.find("FAKE_STDERR_MARKER") != std::string::npos, "stderr is independently captured");
}

void test_repeated_execution(const std::filesystem::path& fake_adb, const std::filesystem::path& log_path) {
    auto transport = make_transport(fake_adb);
    set_mode("normal", log_path);
    bool succeeded = true;
    for (int iteration = 0; iteration < 12; ++iteration) {
        succeeded = succeeded && transport.validate_target("emulator-5554", fang::guest::GuestTargetAbi::X86_64).success;
    }
    check(succeeded, "12 repeated subprocess invocations succeed");
}

void test_unauthorized_no_process(const std::filesystem::path& fake_adb,
                                  const std::filesystem::path& root,
                                  const std::filesystem::path& log_path) {
    std::filesystem::remove(log_path);
    AdbTransportConfig config;
    config.adb_executable = fake_adb;
    config.target_serial = "emulator-5554";
    config.require_explicit_authorization = true;
    config.authorization_token = "test-only-token";
    AdbTransport transport(config);
    auto result = transport.validate_target("emulator-5554", fang::guest::GuestTargetAbi::X86_64);
    check(!result.success && result.error_code == "LIVE_OP_UNAUTHORIZED", "unauthorized target validation is rejected");
    auto push = transport.push_binary("emulator-5554", root / "absent", "/remote/file", 1, "hash");
    auto startup = transport.start_daemon("emulator-5554", "/remote/file", "24800", "session", "generation");
    GuestEndpointMapping mapping;
    check(!transport.setup_forwarding("emulator-5554", 24800, mapping), "unauthorized forwarding is rejected");
    check(!push.success && push.error_code == "LIVE_OP_UNAUTHORIZED", "unauthorized push is rejected");
    check(!startup.success && startup.error_code == "LIVE_OP_UNAUTHORIZED", "unauthorized startup is rejected");
    check(!std::filesystem::exists(log_path), "unauthorized operation launches no subprocess");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: fang-adb-process-tests <fake-adb-executable>\n";
        return 2;
    }
    const std::filesystem::path fake_adb = argv[1];
    const auto root = std::filesystem::temp_directory_path() / "fang_adb_process_unit";
    std::filesystem::create_directories(root);
    const auto log_path = root / "fake-adb.log";
    std::ofstream(log_path, std::ios::binary | std::ios::trunc).close();

    run_case("paths and arguments", [&] { test_paths_and_arguments(fake_adb, root, log_path); });
    run_case("exit and missing executable", [&] { test_exit_and_missing_executable(fake_adb, root, log_path); });
    run_case("timeout", [&] { test_timeout(fake_adb, log_path); });
    run_case("unrelated process", [&] { test_timeout_does_not_terminate_unrelated_process(fake_adb, log_path); });
    run_case("output capture limits", [&] { test_output_capture_limits(fake_adb, log_path); });
    run_case("repeated execution", [&] { test_repeated_execution(fake_adb, log_path); });
    run_case("unauthorized no process", [&] { test_unauthorized_no_process(fake_adb, root, log_path); });

    std::cout << "RESULT failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}