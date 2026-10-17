#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "studio_input_router.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fang::studio {

struct ProcessExecutionResult {
    bool succeeded{};
    DWORD exit_code{};
    std::string error;
};

class IProcessExecutor {
public:
    virtual ~IProcessExecutor() = default;
    virtual ProcessExecutionResult execute(
        const std::filesystem::path& executable,
        const std::vector<std::string>& arguments,
        std::chrono::milliseconds timeout) = 0;
};

class DefaultProcessExecutor final : public IProcessExecutor {
public:
    ProcessExecutionResult execute(
        const std::filesystem::path& executable,
        const std::vector<std::string>& arguments,
        std::chrono::milliseconds timeout) override;
};

struct AdbInputDispatcherConfig {
    std::filesystem::path adb_executable;
    std::size_t max_queue_depth{64};
    std::chrono::milliseconds command_timeout{2000};
    std::chrono::milliseconds event_throttle_ms{0};
    bool coalesce_touch_events{true};
};

class AdbInputDispatcher final : public IInputDispatcher {
public:
    explicit AdbInputDispatcher(
        AdbInputDispatcherConfig config = {},
        std::shared_ptr<IProcessExecutor> executor = nullptr);
    ~AdbInputDispatcher() override;

    AdbInputDispatcher(const AdbInputDispatcher&) = delete;
    AdbInputDispatcher& operator=(const AdbInputDispatcher&) = delete;
    AdbInputDispatcher(AdbInputDispatcher&&) = delete;
    AdbInputDispatcher& operator=(AdbInputDispatcher&&) = delete;

    bool dispatch(const std::string& adb_serial, const InputEvent& event) override;

    void bind_session_target(std::string target_serial);
    void unbind_session();
    std::string bound_session_target() const;
    bool is_session_bound() const noexcept;
    void cancel_pending();

    void set_adb_executable(std::filesystem::path executable);
    std::filesystem::path adb_executable() const;

    void start();
    void stop();
    bool is_running() const noexcept;

    std::size_t queue_depth() const;
    std::size_t dispatched_count() const;
    std::size_t failed_count() const;
    std::size_t dropped_count() const;
    std::string last_error() const;

    void clear_metrics();

    static bool is_valid_adb_serial(std::string_view serial) noexcept;
    static std::optional<std::vector<std::string>> build_command_arguments(
        const std::string& serial, const InputEvent& event, bool coalesce_touch = true);
    static std::string sanitize_text_argument(std::string_view text);

private:
    void worker_loop();

    struct QueueItem {
        std::string serial;
        InputEvent event;
    };

    AdbInputDispatcherConfig config_;
    std::shared_ptr<IProcessExecutor> executor_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueueItem> queue_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::string bound_serial_;

    std::size_t dispatched_count_{0};
    std::size_t failed_count_{0};
    std::size_t dropped_count_{0};
    std::string last_error_;
};

std::filesystem::path discover_adb_executable();

} // namespace fang::studio
