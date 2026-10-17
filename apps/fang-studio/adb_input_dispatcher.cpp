#include "adb_input_dispatcher.hpp"
#include "runtime_backend.hpp"

#include <algorithm>

namespace fang::studio {

namespace {

std::wstring to_wide_string(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), count);
    return result;
}

} // namespace

ProcessExecutionResult DefaultProcessExecutor::execute(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout) {
    ProcessExecutionResult result;
    if (executable.empty()) {
        result.error = "Executable path is empty.";
        return result;
    }

    std::wstring command_line = quote_windows_argument(executable.wstring());
    for (const auto& arg : arguments) {
        command_line.push_back(L' ');
        command_line += quote_windows_argument(to_wide_string(arg));
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    const HANDLE null_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    if (null_handle != INVALID_HANDLE_VALUE) {
        startup.hStdInput = null_handle;
        startup.hStdOutput = null_handle;
        startup.hStdError = null_handle;
    }

    PROCESS_INFORMATION pi{};
    const std::wstring working_dir = executable.parent_path().wstring();
    const BOOL created = CreateProcessW(
        executable.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        null_handle != INVALID_HANDLE_VALUE ? TRUE : FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        &startup,
        &pi);

    if (null_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(null_handle);
    }

    if (!created) {
        result.error = "CreateProcessW failed with error " + std::to_string(GetLastError());
        return result;
    }

    const DWORD timeout_ms = static_cast<DWORD>(std::min<int64_t>(timeout.count(), 60000));
    const DWORD wait_res = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait_res == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        result.error = "ADB command timed out after " + std::to_string(timeout.count()) + " ms.";
    } else if (wait_res == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
            result.succeeded = (exit_code == 0);
            result.exit_code = exit_code;
            if (exit_code != 0) {
                result.error = "Process exited with non-zero code " + std::to_string(exit_code);
            }
        } else {
            result.error = "Could not get exit code: " + std::to_string(GetLastError());
        }
    } else {
        result.error = "WaitForSingleObject failed: " + std::to_string(GetLastError());
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

AdbInputDispatcher::AdbInputDispatcher(
    AdbInputDispatcherConfig config,
    std::shared_ptr<IProcessExecutor> executor)
    : config_(std::move(config)), executor_(std::move(executor)) {
    if (!executor_) {
        executor_ = std::make_shared<DefaultProcessExecutor>();
    }
}

AdbInputDispatcher::~AdbInputDispatcher() {
    stop();
}

void AdbInputDispatcher::bind_session_target(std::string target_serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    bound_serial_ = std::move(target_serial);
}

void AdbInputDispatcher::unbind_session() {
    std::lock_guard<std::mutex> lock(mutex_);
    bound_serial_.clear();
    const auto count = queue_.size();
    queue_.clear();
    dropped_count_ += count;
    if (count > 0) {
        last_error_ = "Pending commands dropped due to session unbind.";
    }
    cv_.notify_all();
}

std::string AdbInputDispatcher::bound_session_target() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bound_serial_;
}

bool AdbInputDispatcher::is_session_bound() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return !bound_serial_.empty();
}

void AdbInputDispatcher::cancel_pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto count = queue_.size();
    queue_.clear();
    dropped_count_ += count;
    if (count > 0) {
        last_error_ = "Pending input commands cancelled.";
    }
    cv_.notify_all();
}

void AdbInputDispatcher::set_adb_executable(std::filesystem::path executable) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.adb_executable = std::move(executable);
}

std::filesystem::path AdbInputDispatcher::adb_executable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.adb_executable;
}

void AdbInputDispatcher::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    stop_requested_ = false;
    running_ = true;
    worker_ = std::thread(&AdbInputDispatcher::worker_loop, this);
}

void AdbInputDispatcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stop_requested_ = true;
        const auto count = queue_.size();
        queue_.clear();
        dropped_count_ += count;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
}

bool AdbInputDispatcher::is_running() const noexcept {
    return running_;
}

std::size_t AdbInputDispatcher::queue_depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t AdbInputDispatcher::dispatched_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dispatched_count_;
}

std::size_t AdbInputDispatcher::failed_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_count_;
}

std::size_t AdbInputDispatcher::dropped_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_count_;
}

std::string AdbInputDispatcher::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void AdbInputDispatcher::clear_metrics() {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatched_count_ = 0;
    failed_count_ = 0;
    dropped_count_ = 0;
    last_error_.clear();
}

bool AdbInputDispatcher::is_valid_adb_serial(std::string_view serial) noexcept {
    if (serial.empty() || serial.size() > 64) {
        return false;
    }
    for (const char c : serial) {
        const bool is_alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        const bool is_allowed_punct = (c == '-' || c == '_' || c == '.' || c == ':');
        if (!is_alnum && !is_allowed_punct) {
            return false;
        }
    }
    return true;
}

std::string AdbInputDispatcher::sanitize_text_argument(std::string_view text) {
    if (text.empty() || text.size() > 256) {
        return {};
    }
    std::string sanitized;
    sanitized.reserve(text.size() * 2);
    for (const char c : text) {
        if (c == ' ') {
            sanitized += "%s";
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            sanitized.push_back(c);
        } else if (c == '.' || c == ',' || c == '-' || c == '_' || c == '!' || c == '?' || c == '@' || c == '/' || c == ':') {
            sanitized.push_back(c);
        }
    }
    return sanitized;
}

std::optional<std::vector<std::string>> AdbInputDispatcher::build_command_arguments(
    const std::string& serial, const InputEvent& event, bool coalesce_touch) {
    if (!is_valid_adb_serial(serial)) {
        return std::nullopt;
    }

    switch (event.type) {
    case InputEventType::TouchDown:
        if (coalesce_touch) {
            return std::vector<std::string>{};
        }
        if (event.x < 0 || event.y < 0) return std::nullopt;
        return std::vector<std::string>{
            "-s", serial, "shell", "input", "motionevent", "DOWN",
            std::to_string(event.x), std::to_string(event.y)
        };
    case InputEventType::TouchMove:
        if (coalesce_touch) {
            return std::vector<std::string>{};
        }
        if (event.x < 0 || event.y < 0) return std::nullopt;
        return std::vector<std::string>{
            "-s", serial, "shell", "input", "motionevent", "MOVE",
            std::to_string(event.x), std::to_string(event.y)
        };
    case InputEventType::TouchUp:
        if (coalesce_touch) {
            return std::vector<std::string>{};
        }
        if (event.x < 0 || event.y < 0) return std::nullopt;
        return std::vector<std::string>{
            "-s", serial, "shell", "input", "motionevent", "UP",
            std::to_string(event.x), std::to_string(event.y)
        };
    case InputEventType::Tap:
        if (event.x < 0 || event.y < 0) return std::nullopt;
        return std::vector<std::string>{
            "-s", serial, "shell", "input", "tap",
            std::to_string(event.x), std::to_string(event.y)
        };
    case InputEventType::Swipe: {
        if (event.x < 0 || event.y < 0 || event.end_x < 0 || event.end_y < 0) return std::nullopt;
        const int duration = std::max(50, event.duration_ms);
        return std::vector<std::string>{
            "-s", serial, "shell", "input", "swipe",
            std::to_string(event.x), std::to_string(event.y),
            std::to_string(event.end_x), std::to_string(event.end_y),
            std::to_string(duration)
        };
    }
    case InputEventType::Key:
        if (event.keycode <= 0) return std::nullopt;
        return std::vector<std::string>{
            "-s", serial, "shell", "input", "keyevent",
            std::to_string(event.keycode)
        };
    case InputEventType::Text: {
        if (event.text.empty()) return std::nullopt;
        const auto sanitized = sanitize_text_argument(event.text);
        if (sanitized.empty()) return std::nullopt;
        return std::vector<std::string>{
            "-s", serial, "shell", "input", "text", sanitized
        };
    }
    default:
        return std::nullopt;
    }
}

bool AdbInputDispatcher::dispatch(const std::string& adb_serial, const InputEvent& event) {
    if (!is_valid_adb_serial(adb_serial)) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++failed_count_;
        last_error_ = "Invalid ADB serial for input dispatch: " + adb_serial;
        return false;
    }

    const auto args = build_command_arguments(adb_serial, event, config_.coalesce_touch_events);
    if (!args.has_value()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++failed_count_;
        last_error_ = "Invalid input event parameters for dispatch.";
        return false;
    }

    if (args->empty()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        ++failed_count_;
        last_error_ = "AdbInputDispatcher is not running.";
        return false;
    }

    if (bound_serial_.empty()) {
        ++failed_count_;
        last_error_ = "Input rejected: No session target bound.";
        return false;
    }

    if (adb_serial != bound_serial_) {
        ++failed_count_;
        last_error_ = "Input rejected: Serial mismatch with bound session target (" + bound_serial_ + " vs " + adb_serial + ").";
        return false;
    }

    if (queue_.size() >= config_.max_queue_depth) {
        ++dropped_count_;
        last_error_ = "Input dispatcher queue full; event dropped.";
        return false;
    }

    queue_.push_back({adb_serial, event});
    cv_.notify_one();
    return true;
}

void AdbInputDispatcher::worker_loop() {
    while (!stop_requested_) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stop_requested_ || !queue_.empty();
            });

            if (stop_requested_) {
                break;
            }
            if (queue_.empty()) {
                continue;
            }

            item = std::move(queue_.front());
            queue_.pop_front();
        }

        const auto args = build_command_arguments(item.serial, item.event, config_.coalesce_touch_events);
        if (!args.has_value() || args->empty()) {
            continue;
        }

        std::filesystem::path adb_path;
        std::shared_ptr<IProcessExecutor> exec;
        std::chrono::milliseconds timeout;
        std::chrono::milliseconds throttle;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            adb_path = config_.adb_executable;
            exec = executor_;
            timeout = config_.command_timeout;
            throttle = config_.event_throttle_ms;
        }

        if (adb_path.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++failed_count_;
            last_error_ = "ADB executable path is not configured.";
            continue;
        }

        if (!exec) {
            exec = std::make_shared<DefaultProcessExecutor>();
        }

        const auto result = exec->execute(adb_path, *args, timeout);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (result.succeeded && result.exit_code == 0) {
                ++dispatched_count_;
            } else {
                ++failed_count_;
                last_error_ = result.error.empty()
                    ? "ADB execution failed with exit code " + std::to_string(result.exit_code)
                    : result.error;
            }
        }

        if (throttle.count() > 0) {
            std::this_thread::sleep_for(throttle);
        }
    }
}

std::filesystem::path discover_adb_executable() {
    const auto emulator = discover_android_emulator();
    if (!emulator.executable.empty()) {
        const auto candidate = emulator.executable.parent_path().parent_path() / L"platform-tools" / L"adb.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    for (const auto* env_name : {L"ANDROID_SDK_ROOT", L"ANDROID_HOME"}) {
        const DWORD len = GetEnvironmentVariableW(env_name, nullptr, 0);
        if (len > 0) {
            std::wstring val(len, L'\0');
            const DWORD copied = GetEnvironmentVariableW(env_name, val.data(), len);
            if (copied > 0 && copied < len) {
                val.resize(copied);
                const auto candidate = std::filesystem::path(val) / L"platform-tools" / L"adb.exe";
                std::error_code ec;
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    return candidate;
                }
            }
        }
    }

    const DWORD local_len = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (local_len > 0) {
        std::wstring local_val(local_len, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", local_val.data(), local_len);
        if (copied > 0 && copied < local_len) {
            local_val.resize(copied);
            const auto candidate = std::filesystem::path(local_val) / L"Android" / L"Sdk" / L"platform-tools" / L"adb.exe";
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }

    return {};
}

} // namespace fang::studio
