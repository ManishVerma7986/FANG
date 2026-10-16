#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include "runtime_backend.hpp"
#include "suspended_process_launch.hpp"

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

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
    HANDLE job{};
    const std::vector<DWORD>* process_ids{};
    HWND window{};
    std::string error;
};

bool is_live_job_member(DWORD process_id, HANDLE job, std::string& error) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, process_id);
    if (process == nullptr) {
        const DWORD open_error = GetLastError();
        if (open_error != ERROR_INVALID_PARAMETER) {
            error = "Could not open Job Object member PID " + std::to_string(process_id) +
                " for window verification: " + narrow(windows_error_message(open_error));
        }
        return false;
    }
    BOOL belongs_to_job{};
    const BOOL query_succeeded = IsProcessInJob(process, job, &belongs_to_job);
    const DWORD query_error = query_succeeded == 0 ? GetLastError() : ERROR_SUCCESS;
    const DWORD wait_result = WaitForSingleObject(process, 0);
    const DWORD wait_error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    const BOOL handle_closed = CloseHandle(process);
    const DWORD close_error = handle_closed == 0 ? GetLastError() : ERROR_SUCCESS;
    if (query_succeeded == 0) {
        error = "Could not verify Job Object membership for PID " + std::to_string(process_id) +
            ": " + narrow(windows_error_message(query_error));
        return false;
    }
    if (wait_result == WAIT_FAILED) {
        error = "Could not inspect Job Object member PID " + std::to_string(process_id) +
            ": " + narrow(windows_error_message(wait_error));
        return false;
    }
    if (handle_closed == 0) {
        error = "Could not close the temporary process handle for Job Object member PID " +
            std::to_string(process_id) + ": " + narrow(windows_error_message(close_error));
        return false;
    }
    return belongs_to_job != FALSE && wait_result == WAIT_TIMEOUT;
}

DWORD process_id_for_window(HWND window) {
    DWORD process_id{};
    if (window == nullptr || !IsWindow(window)) {
        return 0;
    }
    GetWindowThreadProcessId(window, &process_id);
    return process_id;
}

BOOL CALLBACK find_process_window(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD process_id{};
    GetWindowThreadProcessId(window, &process_id);
    if (std::find(search->process_ids->begin(), search->process_ids->end(), process_id) != search->process_ids->end() &&
        GetWindow(window, GW_OWNER) == nullptr && IsWindowVisible(window) &&
        is_live_job_member(process_id, search->job, search->error)) {
        std::string verification_error;
        if (process_id_for_window(window) == process_id &&
            is_live_job_member(process_id, search->job, verification_error)) {
            search->window = window;
            return FALSE;
        }
        if (!verification_error.empty()) search->error = std::move(verification_error);
    }
    return TRUE;
}

bool job_process_ids(HANDLE job, std::vector<DWORD>& process_ids, std::string& error) {
    std::size_t capacity = 16;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto header_size = offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList);
        std::vector<std::byte> buffer(header_size + capacity * sizeof(ULONG_PTR));
        auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
        DWORD returned{};
        if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list,
                static_cast<DWORD>(buffer.size()), &returned) != 0) {
            process_ids.clear();
            process_ids.reserve(list->NumberOfProcessIdsInList);
            for (DWORD index = 0; index < list->NumberOfProcessIdsInList; ++index) {
                process_ids.push_back(static_cast<DWORD>(list->ProcessIdList[index]));
            }
            return true;
        }
        const DWORD query_error = GetLastError();
        if (query_error != ERROR_MORE_DATA) {
            error = "Could not enumerate the owned Android Emulator process group: " +
                narrow(windows_error_message(query_error));
            return false;
        }
        capacity = std::max(capacity * 2,
            static_cast<std::size_t>(list->NumberOfAssignedProcesses));
    }
    error = "The Android Emulator process group changed too quickly to enumerate safely.";
    return false;
}

bool job_active_process_count(HANDLE job, DWORD& active_processes, std::string& error) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
            sizeof(accounting), nullptr) == 0) {
        error = "Could not verify Android Emulator process-group shutdown: " +
            narrow(windows_error_message(GetLastError()));
        return false;
    }
    active_processes = accounting.ActiveProcesses;
    return true;
}

HWND process_window(HANDLE job, const std::vector<DWORD>& process_ids, std::string& error) {
    WindowSearch search{job, &process_ids};
    EnumWindows(find_process_window, reinterpret_cast<LPARAM>(&search));
    error = std::move(search.error);
    return search.window;
}

std::vector<std::uint32_t> process_id_values(const std::vector<DWORD>& process_ids) {
    return {process_ids.begin(), process_ids.end()};
}

std::string process_id_list(const std::vector<std::uint32_t>& process_ids) {
    std::ostringstream text;
    text << '[';
    for (std::size_t index = 0; index < process_ids.size(); ++index) {
        if (index != 0) text << ',';
        text << process_ids[index];
    }
    text << ']';
    return text.str();
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
        const auto observation = observe();
        if (observation.process_running || !observation.error.empty()) {
            return;
        }
        std::string cleanup_error;
        static_cast<void>(release_handles(cleanup_error));
    }

    RuntimeResult launch(const EmulatorConfig& config) override {
        const auto current = observe();
        if (!current.error.empty()) {
            return RuntimeResult::failure(current.error);
        }
        if (current.process_running) {
            return RuntimeResult::failure("An Android Emulator process owned by this Fang instance is already active.");
        }
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
            std::string close_error;
            if (!close_handle(mutex_, "existing AVD mutex handle", close_error)) {
                return RuntimeResult::failure("This AVD is already managed by another Fang instance; " + close_error);
            }
            return RuntimeResult::failure("This AVD is already being managed by another Fang instance.");
        }
        mutex_owned_ = true;

        const auto arguments = build_emulator_arguments(config.avd_name);
        const auto command_line = quote_command_line(config.executable.wstring(), arguments);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION created_process{};
        launch_error_.clear();
        const auto launch_result = launch_suspended_process(
            [&] {
                job_ = CreateJobObjectW(nullptr, nullptr);
                if (job_ == nullptr) {
                    launch_error_ = "Could not create the non-killing Android Emulator Job Object: " +
                        narrow(windows_error_message(GetLastError()));
                    return false;
                }
                return true;
            },
            [&] {
                auto mutable_command_line = command_line;
                if (CreateProcessW(config.executable.c_str(), mutable_command_line.data(), nullptr, nullptr,
                        FALSE, CREATE_SUSPENDED, nullptr, config.executable.parent_path().c_str(),
                        &startup, &created_process) == 0) {
                    launch_error_ = "Could not create a suspended Android Emulator process: " +
                        narrow(windows_error_message(GetLastError()));
                    return false;
                }
                process_ = created_process.hProcess;
                thread_ = created_process.hThread;
                process_id_ = created_process.dwProcessId;
                return true;
            },
            [&] {
                if (AssignProcessToJobObject(job_, process_) == 0) {
                    launch_error_ = "Could not assign the suspended Emulator to its Job Object: " +
                        narrow(windows_error_message(GetLastError()));
                    return false;
                }
                return true;
            },
            [&] {
                const DWORD previous_suspend_count = ResumeThread(thread_);
                if (previous_suspend_count == static_cast<DWORD>(-1)) {
                    launch_error_ = "Could not resume the Android Emulator after Job Object assignment: " +
                        narrow(windows_error_message(GetLastError()));
                    return false;
                }
                if (previous_suspend_count != 1) {
                    launch_error_ = "The Android Emulator primary thread had an unexpected suspend count.";
                    return false;
                }
                return true;
            },
            [&] {
                if (process_ == nullptr) {
                    return true;
                }
                if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0 &&
                    TerminateProcess(process_, ERROR_PROCESS_ABORTED) == 0 &&
                    GetLastError() != ERROR_ACCESS_DENIED) {
                    launch_error_ += " The exact Fang-created root could not be aborted.";
                    return false;
                }
                if (WaitForSingleObject(process_, 5000) != WAIT_OBJECT_0) {
                    launch_error_ += " The exact Fang-created root did not exit within 5 seconds.";
                    return false;
                }
                DWORD active_processes{};
                std::string query_error;
                if (job_ != nullptr && !job_active_process_count(job_, active_processes, query_error)) {
                    launch_error_ += " " + query_error;
                    return false;
                }
                if (active_processes != 0) {
                    launch_error_ += " The root exited but owned Job Object processes remain active.";
                    return false;
                }
                return true;
            });
        std::string thread_close_error;
        const bool thread_closed = close_handle(thread_, "Emulator primary thread handle", thread_close_error);
        if (launch_result == SuspendedLaunchResult::Started && !thread_closed) {
            startup_cleanup_unverified_ = true;
            launch_error_ = thread_close_error;
            return RuntimeResult::failure("Emulator is in the owned Job Object, but " + thread_close_error +
                " Fang retains runtime handles and AVD ownership.");
        }
        if (launch_result != SuspendedLaunchResult::Started) {
            if (launch_error_.empty()) {
                launch_error_ = "Android Emulator launch setup failed at stage " +
                    std::to_string(static_cast<int>(launch_result)) + ".";
            }
            if (launch_result == SuspendedLaunchResult::AssignmentCleanupUnverified ||
                launch_result == SuspendedLaunchResult::ResumeCleanupUnverified) {
                startup_cleanup_unverified_ = true;
                suspended_unassigned_ =
                    launch_result == SuspendedLaunchResult::AssignmentCleanupUnverified;
                return RuntimeResult::failure(launch_error_ +
                    " Fang retains the process/job handles and AVD ownership; shutdown is unverified.");
            }
            std::string cleanup_error;
            if (!release_handles(cleanup_error)) {
                startup_cleanup_unverified_ = true;
                return RuntimeResult::failure(launch_error_ + " Resource cleanup is unverified: " + cleanup_error +
                    " Fang retains remaining handles and AVD ownership.");
            }
            return RuntimeResult::failure(launch_error_);
        }
        started_at_ = GetTickCount64();
        stop_requested_at_ = 0;
        stop_requested_ = false;
        startup_timeout_reported_ = false;
        BOOL root_is_in_job{};
        if (IsProcessInJob(process_, job_, &root_is_in_job) == 0) {
            launch_error_ = "Could not verify that the resumed Emulator root belongs to Fang's Job Object: " +
                narrow(windows_error_message(GetLastError()));
            startup_cleanup_unverified_ = true;
            return RuntimeResult::failure(launch_error_ + " Fang retains the process/job handles and AVD ownership.");
        }
        if (root_is_in_job == FALSE) {
            launch_error_ = "Windows reported that the resumed Emulator root is not a member of Fang's Job Object.";
            startup_cleanup_unverified_ = true;
            return RuntimeResult::failure(launch_error_ + " Fang retains the process/job handles and AVD ownership.");
        }
        std::vector<DWORD> process_ids;
        std::string query_error;
        if (!job_process_ids(job_, process_ids, query_error)) {
            startup_cleanup_unverified_ = true;
            launch_error_ = query_error;
            return RuntimeResult::failure(query_error + " Fang retains the process/job handles and AVD ownership.");
        }
        return RuntimeResult::success("Android Emulator root PID=" + std::to_string(process_id_) +
            " started in Fang's non-killing Job Object; owned PIDs=" +
            process_id_list(process_id_values(process_ids)) + ". Waiting for its standalone window.");
    }

    RuntimeResult request_stop() override {
        if (suspended_unassigned_) {
            if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0 &&
                TerminateProcess(process_, ERROR_PROCESS_ABORTED) == 0 &&
                GetLastError() != ERROR_ACCESS_DENIED) {
                return RuntimeResult::failure("Could not retry cleanup of Fang's exact suspended, unassigned Emulator root.");
            }
            if (WaitForSingleObject(process_, 5000) != WAIT_OBJECT_0) {
                return RuntimeResult::failure("Shutdown remains unverified: Fang's suspended, unassigned Emulator root did not exit within 5 seconds.");
            }
            std::string cleanup_error;
            if (!release_handles(cleanup_error)) {
                return RuntimeResult::failure("The suspended, unassigned Emulator root exited, but resource cleanup is unverified: " + cleanup_error);
            }
            return RuntimeResult::success("The suspended, unassigned Emulator root exited; no Job Object member was left active.");
        }
        const auto observation = observe();
        if (!observation.error.empty()) {
            return RuntimeResult::failure(observation.error);
        }
        if (!observation.process_running) {
            return RuntimeResult::success("Android Emulator process is already stopped.");
        }
        stop_requested_ = true;
        stop_requested_at_ = GetTickCount64();
        stop_message_sent_ = false;
        std::vector<DWORD> process_ids;
        std::string enumeration_error;
        if (!job_process_ids(job_, process_ids, enumeration_error)) {
            stop_requested_ = false;
            return RuntimeResult::failure(enumeration_error);
        }
        std::string window_error;
        const auto window = process_window(job_, process_ids, window_error);
        if (!window_error.empty()) {
            stop_requested_ = false;
            return RuntimeResult::failure(window_error);
        }
        const DWORD window_process_id = process_id_for_window(window);
        std::string member_error;
        if (window != nullptr && !is_live_job_member(window_process_id, job_, member_error)) {
            stop_message_sent_ = false;
            if (!member_error.empty()) {
                stop_requested_ = false;
                return RuntimeResult::failure(member_error);
            }
            return RuntimeResult::success("Stop requested; the selected Job Object window exited or changed ownership before close.");
        }
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
        const auto observation = observe();
        if (!observation.error.empty()) {
            return RuntimeResult::failure(observation.error);
        }
        if (!observation.process_running) {
            return RuntimeResult::failure("The Android Emulator process is not running.");
        }
        std::vector<DWORD> process_ids;
        std::string enumeration_error;
        if (!job_process_ids(job_, process_ids, enumeration_error)) {
            return RuntimeResult::failure(enumeration_error);
        }
        std::string window_error;
        const auto window = process_window(job_, process_ids, window_error);
        if (!window_error.empty()) {
            return RuntimeResult::failure(window_error);
        }
        if (window == nullptr) {
            return RuntimeResult::failure("The standalone Emulator window is not available yet; guest boot is not verified.");
        }
        const DWORD window_process_id = process_id_for_window(window);
        std::string member_error;
        if (!is_live_job_member(window_process_id, job_, member_error)) {
            if (!member_error.empty()) return RuntimeResult::failure(member_error);
            return RuntimeResult::failure("The standalone Emulator window could not be revalidated as a live Job Object member.");
        }
        ShowWindow(window, SW_RESTORE);
        if (SetForegroundWindow(window) == 0) {
            return RuntimeResult::failure("Windows did not allow Fang to focus the Emulator window. Use its taskbar entry.");
        }
        return RuntimeResult::success("Focused the standalone Android Emulator window.");
    }

    RuntimeObservation observe() override {
        if (suspended_unassigned_) {
            const DWORD wait = WaitForSingleObject(process_, 0);
            if (wait == WAIT_FAILED) {
                return {true, false, "Could not inspect Fang's suspended, unassigned Emulator root: " +
                    narrow(windows_error_message(GetLastError())), true, 0};
            }
            if (wait == WAIT_OBJECT_0) {
                const DWORD previous_pid = process_id_;
                std::string cleanup_error;
                if (!release_handles(cleanup_error)) {
                    RuntimeObservation failed{false, false,
                        "Suspended Emulator root exited, but resource cleanup is unverified: " + cleanup_error,
                        false, 0};
                    failed.root_process_id = previous_pid;
                    return failed;
                }
                RuntimeObservation stopped;
                stopped.root_process_id = previous_pid;
                return stopped;
            }
            RuntimeObservation active{true, false, {}, true, 0};
            active.root_process_id = process_id_;
            active.owned_process_ids.push_back(process_id_);
            return active;
        }
        const DWORD root_wait = process_ == nullptr ? WAIT_OBJECT_0 : WaitForSingleObject(process_, 0);
        if (root_wait == WAIT_FAILED) {
            RuntimeObservation failed{true, false, "Could not inspect the Android Emulator root process: " +
                narrow(windows_error_message(GetLastError())), true, 0};
            failed.root_process_id = process_id_;
            return failed;
        }
        const bool root_running = root_wait == WAIT_TIMEOUT;
        DWORD active_processes{};
        std::string query_error;
        if (job_ != nullptr && !job_active_process_count(job_, active_processes, query_error)) {
            RuntimeObservation failed{true, false, query_error, root_running, 0};
            failed.root_process_id = process_id_;
            return failed;
        }
        if (root_running && job_ != nullptr && active_processes == 0) {
            RuntimeObservation failed{true, false,
                "The Emulator root is active but the Job Object reports no active members; shutdown ownership is inconsistent.", true, 0};
            failed.root_process_id = process_id_;
            return failed;
        }
        const bool group_running = root_running || active_processes > 0;
        if (!group_running) {
            const DWORD previous_pid = process_id_;
            DWORD exit_code{};
            const bool exited_unexpectedly = process_ != nullptr && !stop_requested_ &&
                GetExitCodeProcess(process_, &exit_code) != 0;
            std::string cleanup_error;
            if (!release_handles(cleanup_error)) {
                RuntimeObservation failed{false, false,
                    "Owned Job Object is empty, but resource/AVD-mutex cleanup is unverified: " + cleanup_error,
                    false, 0};
                failed.root_process_id = previous_pid;
                return failed;
            }
            if (exited_unexpectedly) {
                RuntimeObservation failed{false, false, "Android Emulator root PID " +
                    std::to_string(previous_pid) + " exited unexpectedly with code " +
                    std::to_string(exit_code) + "; the owned Job Object is empty.", false, 0};
                failed.root_process_id = previous_pid;
                return failed;
            }
            RuntimeObservation stopped;
            stopped.root_process_id = previous_pid;
            return stopped;
        }
        const auto now = GetTickCount64();
        std::vector<DWORD> process_ids;
        std::string enumeration_error;
        if (!job_process_ids(job_, process_ids, enumeration_error)) {
            RuntimeObservation failed{true, false, enumeration_error, root_running, active_processes};
            failed.root_process_id = process_id_;
            return failed;
        }
        std::string window_error;
        const auto window = process_window(job_, process_ids, window_error);
        if (!window_error.empty()) {
            RuntimeObservation failed{true, false, window_error, root_running, active_processes};
            failed.root_process_id = process_id_;
            failed.owned_process_ids = process_id_values(process_ids);
            return failed;
        }
        if (stop_requested_ && window != nullptr && !stop_message_sent_ && now - stop_requested_at_ < stop_timeout_ms) {
            stop_message_sent_ = PostMessageW(window, WM_CLOSE, 0, 0) != 0;
        }
        if (!stop_requested_ && root_running && window == nullptr && now - started_at_ >= startup_timeout_ms &&
            !startup_timeout_reported_) {
            startup_timeout_reported_ = true;
            RuntimeObservation timeout{true, false,
                "Emulator root is active but no owned Job Object window appeared within 90 seconds. It was left running; Stop or close it manually.",
                root_running, active_processes};
            timeout.root_process_id = process_id_;
            timeout.owned_process_ids = process_id_values(process_ids);
            return timeout;
        }
        if (stop_requested_ && now - stop_requested_at_ >= stop_timeout_ms) {
            stop_requested_ = false;
            stop_message_sent_ = false;
            return {true, window != nullptr,
                "Shutdown unverified after 30 seconds: root PID=" + std::to_string(process_id_) +
                (root_running ? " active" : " exited") + ", owned Job Object process count=" +
                std::to_string(active_processes) + ", PIDs=" + process_id_list(process_id_values(process_ids)) +
                ". Fang left the group running; retry Stop or close its owned window.",
                root_running, active_processes, process_id_, process_id_values(process_ids)};
        }
        return {true, window != nullptr, {}, root_running, active_processes,
            process_id_, process_id_values(process_ids)};
    }

private:
    bool close_handle(HANDLE& handle, const char* name, std::string& error) {
        if (handle == nullptr) {
            return true;
        }
        if (CloseHandle(handle) == 0) {
            error = "Could not close the " + std::string(name) + ": " +
                narrow(windows_error_message(GetLastError()));
            return false;
        }
        handle = nullptr;
        return true;
    }

    bool release_handles(std::string& error) {
        if (!close_handle(thread_, "Emulator primary thread handle", error) ||
            !close_handle(process_, "Emulator root process handle", error) ||
            !close_handle(job_, "Emulator Job Object handle", error)) {
            return false;
        }
        if (mutex_ != nullptr) {
            if (mutex_owned_) {
                if (ReleaseMutex(mutex_) == 0) {
                    error = "Could not release the AVD ownership mutex: " +
                        narrow(windows_error_message(GetLastError()));
                    return false;
                }
                mutex_owned_ = false;
            }
            if (!close_handle(mutex_, "AVD ownership mutex handle", error)) {
                return false;
            }
        }
        process_id_ = 0;
        stop_message_sent_ = false;
        startup_cleanup_unverified_ = false;
        suspended_unassigned_ = false;
        return true;
    }

    HANDLE job_{};
    HANDLE process_{};
    HANDLE thread_{};
    HANDLE mutex_{};
    DWORD process_id_{};
    bool mutex_owned_{};
    ULONGLONG started_at_{};
    ULONGLONG stop_requested_at_{};
    bool stop_requested_{};
    bool stop_message_sent_{};
    bool startup_timeout_reported_{};
    bool startup_cleanup_unverified_{};
    bool suspended_unassigned_{};
    std::string launch_error_;
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
    if (observation.process_running) {
        if ((status_.state == RuntimeState::Starting || status_.state == RuntimeState::Running) &&
            !observation.root_process_running) {
            status_ = {RuntimeState::Failed,
                "Android Emulator root PID " + std::to_string(observation.root_process_id) +
                " exited while " + std::to_string(observation.active_process_count) +
                " owned process(es) remain in Job Object; PIDs=" + process_id_list(observation.owned_process_ids) + "."};
            restart_pending_ = false;
        } else if (status_.state == RuntimeState::Starting && observation.player_window_available) {
            status_ = {RuntimeState::Running, "Standalone Emulator window is available for root PID " +
                std::to_string(observation.root_process_id) + "; owned Job Object PIDs=" +
                process_id_list(observation.owned_process_ids) + "; guest boot status is not verified."};
        } else if (status_.state == RuntimeState::Stopping) {
            status_.message = "Waiting for root PID " + std::to_string(observation.root_process_id) +
                (observation.root_process_running ? " to exit" : " (exited)") + "; owned Job Object count=" +
                std::to_string(observation.active_process_count) + ", PIDs=" +
                process_id_list(observation.owned_process_ids) + "; no process is terminated forcibly.";
        }
        return;
    }
    {
        if (status_.state == RuntimeState::Stopping || status_.state == RuntimeState::Starting) {
            status_ = {RuntimeState::Stopped, "Stopped; root PID " +
                std::to_string(observation.root_process_id) +
                " exited and owned Job Object process count is 0."};
            if (restart_pending_) {
                const auto config = restart_config_;
                restart_pending_ = false;
                static_cast<void>(start_after_stopped(config));
            }
        } else if (status_.state == RuntimeState::Running) {
            status_ = {RuntimeState::Failed, "Android Emulator exited unexpectedly."};
        }
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
