#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include "runtime_backend.hpp"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fang::studio {
namespace {

constexpr DWORD startup_timeout_ms = 90000;
constexpr DWORD stop_timeout_ms = 30000;

std::wstring windows_error_message(DWORD error) {
    wchar_t* buffer{};
    const auto length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::wstring message = length > 0 ? std::wstring(buffer, length) : L"Windows returned an unknown error.";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && std::iswspace(message.back())) {
        message.pop_back();
    }
    return message;
}

std::string narrow(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "The Android Emulator returned text that could not be decoded.";
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring quote_command_line(std::wstring_view executable, const std::vector<std::wstring>& arguments) {
    std::wstring command_line = quote_windows_argument(executable);
    for (const auto& argument : arguments) {
        command_line.push_back(L' ');
        command_line += quote_windows_argument(argument);
    }
    return command_line;
}

std::wstring environment_value(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(length), L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), length);
    if (copied == 0 || copied >= length) {
        return {};
    }
    value.resize(copied);
    return value;
}

std::filesystem::path sdk_root(std::string& error) {
    std::wstring root = environment_value(L"ANDROID_SDK_ROOT");
    if (root.empty()) {
        root = environment_value(L"ANDROID_HOME");
    }
    if (root.empty()) {
        std::wstring local_app_data(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(),
            static_cast<DWORD>(local_app_data.size()));
        if (length == 0 || length >= local_app_data.size()) {
            error = "Set ANDROID_SDK_ROOT or ANDROID_HOME to the installed Android SDK directory.";
            return {};
        }
        local_app_data.resize(length);
        root = (std::filesystem::path(local_app_data) / L"Android" / L"Sdk").wstring();
    }
    return std::filesystem::path(root);
}

bool read_process_output(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
    std::wstring& output, std::string& error) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe{};
    HANDLE write_pipe{};
    if (CreatePipe(&read_pipe, &write_pipe, &security, 0) == 0) {
        error = "Could not create a pipe for Android Emulator discovery: " + narrow(windows_error_message(GetLastError()));
        return false;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    const HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input == INVALID_HANDLE_VALUE) {
        error = "Could not open NUL for Android Emulator discovery: " + narrow(windows_error_message(GetLastError()));
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return false;
    }
    startup.hStdInput = null_input;
    PROCESS_INFORMATION process{};
    auto command_line = quote_command_line(executable.wstring(), arguments);
    const BOOL created = CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process);
    const DWORD create_error = created == 0 ? GetLastError() : ERROR_SUCCESS;
    if (null_input != INVALID_HANDLE_VALUE) {
        CloseHandle(null_input);
    }
    CloseHandle(write_pipe);
    if (created == 0) {
        error = "Could not run Android Emulator discovery: " + narrow(windows_error_message(create_error));
        CloseHandle(read_pipe);
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + 10000;
    std::string output_bytes;
    bool finished{};
    while (GetTickCount64() < deadline) {
        DWORD available{};
        if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) != 0 && available > 0) {
            char buffer[2048]{};
            DWORD bytes_read{};
            if (ReadFile(read_pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &bytes_read, nullptr) != 0) {
                output_bytes.append(buffer, bytes_read);
            }
        }
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            finished = true;
            break;
        }
        Sleep(10);
    }
    if (!finished) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 2000);
        error = "Android Emulator AVD discovery timed out after 10 seconds.";
    } else {
        DWORD available{};
        while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) != 0 && available > 0) {
            char buffer[2048]{};
            DWORD bytes_read{};
            if (ReadFile(read_pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &bytes_read, nullptr) == 0) {
                break;
            }
            output_bytes.append(buffer, bytes_read);
        }
        if (!output_bytes.empty()) {
            const int size = MultiByteToWideChar(CP_UTF8, 0, output_bytes.data(),
                static_cast<int>(output_bytes.size()), nullptr, 0);
            if (size > 0) {
                output.resize(static_cast<std::size_t>(size));
                MultiByteToWideChar(CP_UTF8, 0, output_bytes.data(), static_cast<int>(output_bytes.size()),
                    output.data(), size);
            } else {
                error = "Android Emulator AVD discovery returned invalid UTF-8 output.";
            }
        }
        DWORD exit_code{};
        if (error.empty()) {
            if (GetExitCodeProcess(process.hProcess, &exit_code) == 0) {
                error = "Could not read the Android Emulator AVD discovery exit status.";
            } else if (exit_code != 0) {
                error = "Android Emulator AVD discovery failed with exit code " + std::to_string(exit_code) + ".";
            }
        }
    }
    CloseHandle(read_pipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return finished && error.empty();
}

struct WindowSearch {
    const std::vector<DWORD>* process_ids{};
    HWND window{};
};

BOOL CALLBACK find_process_window(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD process_id{};
    GetWindowThreadProcessId(window, &process_id);
    if (std::find(search->process_ids->begin(), search->process_ids->end(), process_id) != search->process_ids->end() &&
        GetWindow(window, GW_OWNER) == nullptr && IsWindowVisible(window)) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND process_window(DWORD process_id) {
    std::vector<DWORD> process_ids{process_id};
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry) != 0) {
            std::vector<PROCESSENTRY32W> processes;
            do {
                processes.push_back(entry);
            } while (Process32NextW(snapshot, &entry) != 0);
            bool found_child = true;
            while (found_child) {
                found_child = false;
                for (const auto& process : processes) {
                    if (std::find(process_ids.begin(), process_ids.end(), process.th32ParentProcessID) != process_ids.end() &&
                        std::find(process_ids.begin(), process_ids.end(), process.th32ProcessID) == process_ids.end()) {
                        process_ids.push_back(process.th32ProcessID);
                        found_child = true;
                    }
                }
            }
        }
        CloseHandle(snapshot);
    }
    WindowSearch search{&process_ids};
    EnumWindows(find_process_window, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

std::wstring mutex_name(std::wstring_view avd_name) {
    std::wostringstream name;
    name << L"Local\\Fang.AndroidEmulator.";
    for (const wchar_t character : avd_name) {
        name << std::hex << std::setw(4) << std::setfill(L'0') << static_cast<unsigned int>(character);
    }
    return name.str();
}

class WindowsEmulatorProcess final : public IEmulatorProcess {
public:
    ~WindowsEmulatorProcess() override {
        release_handles();
    }

    RuntimeResult launch(const EmulatorConfig& config) override {
        if (process_running()) {
            return RuntimeResult::failure("An Android Emulator process owned by this Fang instance is already active.");
        }
        release_handles();
        if (config.executable.empty() || config.avd_name.empty()) {
            return RuntimeResult::failure("Select an installed AVD and verify the Android Emulator executable before starting.");
        }
        for (const wchar_t character : config.avd_name) {
            if (character == L'\r' || character == L'\n') {
                return RuntimeResult::failure("The selected AVD name contains an invalid line break.");
            }
        }

        SetLastError(ERROR_SUCCESS);
        mutex_ = CreateMutexW(nullptr, TRUE, mutex_name(config.avd_name).c_str());
        const DWORD mutex_error = GetLastError();
        if (mutex_ == nullptr) {
            return RuntimeResult::failure("Fang could not reserve this AVD: " + narrow(windows_error_message(mutex_error)));
        }
        if (mutex_error == ERROR_ALREADY_EXISTS) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
            return RuntimeResult::failure("This AVD is already being managed by another Fang instance.");
        }

        const auto arguments = build_emulator_arguments(config.avd_name);
        auto command_line = quote_command_line(config.executable.wstring(), arguments);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION process{};
        if (CreateProcessW(config.executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                nullptr, config.executable.parent_path().c_str(), &startup, &process) == 0) {
            const auto message = "Could not launch Android Emulator: " + narrow(windows_error_message(GetLastError()));
            ReleaseMutex(mutex_);
            CloseHandle(mutex_);
            mutex_ = nullptr;
            return RuntimeResult::failure(message);
        }
        process_ = process.hProcess;
        process_id_ = process.dwProcessId;
        CloseHandle(process.hThread);
        started_at_ = GetTickCount64();
        stop_requested_at_ = 0;
        stop_requested_ = false;
        startup_timeout_reported_ = false;
        return RuntimeResult::success("Android Emulator process started; waiting for its standalone window.");
    }

    RuntimeResult request_stop() override {
        if (!process_running()) {
            return RuntimeResult::success("Android Emulator process is already stopped.");
        }
        stop_requested_ = true;
        stop_requested_at_ = GetTickCount64();
        stop_message_sent_ = false;
        const auto window = process_window(process_id_);
        if (window != nullptr && PostMessageW(window, WM_CLOSE, 0, 0) == 0) {
            stop_requested_ = false;
            return RuntimeResult::failure("Could not send a graceful close request to the Android Emulator window.");
        }
        stop_message_sent_ = window != nullptr;
        return RuntimeResult::success(window == nullptr
            ? "Stop requested; Fang will send a graceful close when the Emulator window appears."
            : "Graceful close requested for the Android Emulator window.");
    }

    RuntimeResult open_player() override {
        if (!process_running()) {
            return RuntimeResult::failure("The Android Emulator process is not running.");
        }
        const auto window = process_window(process_id_);
        if (window == nullptr) {
            return RuntimeResult::failure("The standalone Emulator window is not available yet; guest boot is not verified.");
        }
        ShowWindow(window, SW_RESTORE);
        if (SetForegroundWindow(window) == 0) {
            return RuntimeResult::failure("Windows did not allow Fang to focus the Emulator window. Use its taskbar entry.");
        }
        return RuntimeResult::success("Focused the standalone Android Emulator window.");
    }

    RuntimeObservation observe() override {
        if (!process_running()) {
            if (process_ != nullptr) {
                DWORD exit_code{};
                const bool exited_unexpectedly = !stop_requested_ &&
                    GetExitCodeProcess(process_, &exit_code) != 0;
                release_handles();
                if (exited_unexpectedly) {
                    return {false, false, "Android Emulator exited unexpectedly with code " +
                        std::to_string(exit_code) + "."};
                }
            }
            return {};
        }
        const auto now = GetTickCount64();
        const auto window = process_window(process_id_);
        if (stop_requested_ && window != nullptr && !stop_message_sent_ && now - stop_requested_at_ < stop_timeout_ms) {
            stop_message_sent_ = PostMessageW(window, WM_CLOSE, 0, 0) != 0;
        }
        if (!stop_requested_ && window == nullptr && now - started_at_ >= startup_timeout_ms &&
            !startup_timeout_reported_) {
            startup_timeout_reported_ = true;
            return {true, false, "Emulator process is still active but its window did not appear within 90 seconds. It was left running; Stop or close it manually."};
        }
        if (stop_requested_ && now - stop_requested_at_ >= stop_timeout_ms) {
            stop_requested_ = false;
            stop_message_sent_ = false;
            return {true, window != nullptr, "Emulator did not exit after a graceful close request. It was left running; close its window manually."};
        }
        return {true, window != nullptr, {}};
    }

private:
    void release_handles() {
        if (process_ != nullptr) {
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (mutex_ != nullptr) {
            ReleaseMutex(mutex_);
            CloseHandle(mutex_);
            mutex_ = nullptr;
        }
        process_id_ = 0;
        stop_message_sent_ = false;
    }

    bool process_running() const {
        return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    HANDLE process_{};
    HANDLE mutex_{};
    DWORD process_id_{};
    ULONGLONG started_at_{};
    ULONGLONG stop_requested_at_{};
    bool stop_requested_{};
    bool stop_message_sent_{};
    bool startup_timeout_reported_{};
};

}

RuntimeResult RuntimeResult::success(std::string message) {
    return {true, std::move(message)};
}

RuntimeResult RuntimeResult::failure(std::string message) {
    return {false, std::move(message)};
}

RuntimeBackend::RuntimeBackend(std::unique_ptr<IEmulatorProcess> process) : process_(std::move(process)) {
    if (!process_) {
        throw std::invalid_argument("A runtime process implementation is required.");
    }
}

RuntimeResult RuntimeBackend::start_after_stopped(const EmulatorConfig& config) {
    const auto result = process_->launch(config);
    if (!result.succeeded) {
        status_ = {RuntimeState::Failed, result.message};
        return result;
    }
    restart_pending_ = false;
    status_ = {RuntimeState::Starting, result.message.empty() ? "Starting Android Emulator." : result.message};
    return result;
}

RuntimeResult RuntimeBackend::start(const EmulatorConfig& config) {
    if (status_.state == RuntimeState::Starting || status_.state == RuntimeState::Running ||
        status_.state == RuntimeState::Stopping) {
        return RuntimeResult::failure("Android Emulator is already active; duplicate start was prevented.");
    }
    const auto observation = process_->observe();
    if (!observation.error.empty()) {
        status_ = {RuntimeState::Failed, observation.error};
        return RuntimeResult::failure(observation.error);
    }
    if (observation.process_running) {
        status_ = {RuntimeState::Failed, "An Android Emulator process is still active. Stop it before starting another."};
        return RuntimeResult::failure(status_.message);
    }
    status_ = {RuntimeState::Stopped, "Stopped"};
    return start_after_stopped(config);
}

RuntimeResult RuntimeBackend::stop() {
    if (status_.state == RuntimeState::Stopped) {
        return RuntimeResult::success("Android Emulator is already stopped.");
    }
    const auto observation = process_->observe();
    if (!observation.error.empty()) {
        status_ = {RuntimeState::Failed, observation.error};
        restart_pending_ = false;
        return RuntimeResult::failure(observation.error);
    }
    if (!observation.process_running) {
        status_ = {RuntimeState::Stopped, "Stopped"};
        restart_pending_ = false;
        return RuntimeResult::success("Android Emulator is already stopped.");
    }
    const auto result = process_->request_stop();
    if (!result.succeeded) {
        status_ = {RuntimeState::Failed, result.message};
        restart_pending_ = false;
        return result;
    }
    status_ = {RuntimeState::Stopping, result.message.empty() ? "Stopping Android Emulator." : result.message};
    return result;
}

RuntimeResult RuntimeBackend::restart(const EmulatorConfig& config) {
    if (status_.state == RuntimeState::Stopping) {
        return RuntimeResult::failure("Android Emulator is already stopping; wait for it to exit before restarting.");
    }
    const auto observation = process_->observe();
    if (!observation.error.empty()) {
        status_ = {RuntimeState::Failed, observation.error};
        return RuntimeResult::failure(observation.error);
    }
    if (!observation.process_running) {
        status_ = {RuntimeState::Stopped, "Stopped"};
        return start_after_stopped(config);
    }
    restart_config_ = config;
    restart_pending_ = true;
    return stop();
}

RuntimeResult RuntimeBackend::open_player() {
    if (status_.state != RuntimeState::Running) {
        return RuntimeResult::failure("Open Player is available after the standalone Emulator window appears.");
    }
    return process_->open_player();
}

void RuntimeBackend::poll() {
    const auto observation = process_->observe();
    if (!observation.error.empty()) {
        status_ = {RuntimeState::Failed, observation.error};
        restart_pending_ = false;
        return;
    }
    if (!observation.process_running) {
        if (status_.state == RuntimeState::Stopping || status_.state == RuntimeState::Starting) {
            status_ = {RuntimeState::Stopped, "Stopped"};
            if (restart_pending_) {
                const auto config = restart_config_;
                restart_pending_ = false;
                static_cast<void>(start_after_stopped(config));
            }
        } else if (status_.state == RuntimeState::Running) {
            status_ = {RuntimeState::Failed, "Android Emulator exited unexpectedly."};
        }
        return;
    }
    if (status_.state == RuntimeState::Starting && observation.player_window_available) {
        status_ = {RuntimeState::Running, "Standalone Emulator window is available; guest boot status is not verified."};
    } else if (status_.state == RuntimeState::Stopping) {
        status_.message = "Stopping Android Emulator; the guest process is not terminated forcibly.";
    }
}

const RuntimeStatus& RuntimeBackend::status() const noexcept {
    return status_;
}

EmulatorInstallation discover_android_emulator() {
    EmulatorInstallation installation;
    std::string sdk_error;
    const auto root = sdk_root(sdk_error);
    if (root.empty()) {
        installation.error = std::move(sdk_error);
        return installation;
    }
    installation.executable = root / L"emulator" / L"emulator.exe";
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(installation.executable, file_error)) {
        installation.error = "Android Emulator was not found at " + narrow(installation.executable.wstring()) +
            ". Set ANDROID_SDK_ROOT or ANDROID_HOME to the installed SDK.";
        if (file_error) {
            installation.error += " File check failed: " + file_error.message();
        }
        return installation;
    }
    std::wstring output;
    if (!read_process_output(installation.executable, {L"-list-avds"}, output, installation.error)) {
        return installation;
    }
    std::wistringstream lines(output);
    std::wstring name;
    while (std::getline(lines, name)) {
        if (!name.empty() && name.back() == L'\r') {
            name.pop_back();
        }
        if (!name.empty()) {
            installation.avd_names.push_back(name);
        }
    }
    if (installation.avd_names.empty()) {
        installation.error = "Android Emulator is installed, but `emulator.exe -list-avds` found no AVDs. Create an AVD with Android Studio's device manager or the installed SDK tools.";
    }
    return installation;
}

std::vector<std::wstring> build_emulator_arguments(std::wstring_view avd_name) {
    if (avd_name.empty() || avd_name.find(L'\r') != std::wstring_view::npos ||
        avd_name.find(L'\n') != std::wstring_view::npos) {
        throw std::invalid_argument("An installed AVD name without line breaks is required.");
    }
    return {L"-avd", std::wstring(avd_name), L"-gpu", L"host", L"-accel", L"on"};
}

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

std::unique_ptr<IEmulatorProcess> create_windows_emulator_process() {
    return std::make_unique<WindowsEmulatorProcess>();
}

std::string runtime_state_name(RuntimeState state) {
    switch (state) {
    case RuntimeState::Stopped: return "Stopped";
    case RuntimeState::Starting: return "Starting";
    case RuntimeState::Running: return "Running";
    case RuntimeState::Stopping: return "Stopping";
    case RuntimeState::Failed: return "Failed";
    }
    return "Unknown";
}

}
