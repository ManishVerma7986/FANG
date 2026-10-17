#pragma once

#include "broker_pipe.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fang::studio {

struct SupervisorRuntimeSnapshot {
    fang::broker::RuntimeStatusData runtime;
    fang::broker::BrokerErrorCode error{fang::broker::BrokerErrorCode::None};
    std::string message;
    bool request_pending{};
};

class SupervisorRuntimeClient final {
public:
    SupervisorRuntimeClient(std::wstring pipe_name,
        std::filesystem::path supervisor_executable);
    SupervisorRuntimeClient(const SupervisorRuntimeClient&) = delete;
    SupervisorRuntimeClient& operator=(const SupervisorRuntimeClient&) = delete;
    ~SupervisorRuntimeClient();

    bool start(std::string avd_name);
    bool shutdown();
    bool restart(std::string avd_name);
    SupervisorRuntimeSnapshot snapshot() const;

private:
    enum class CommandKind { Start, Shutdown, Restart };
    struct Command {
        CommandKind kind{};
        std::string avd_name;
    };

    void worker_loop();
    bool ensure_supervisor(std::string& error);
    bool send_start(std::string_view avd_name);
    bool send_shutdown();
    bool query_status();
    bool send_initialize();
    void apply_reply(const fang::broker::BrokerCallResult& result);
    void set_failure(fang::broker::BrokerErrorCode code,
        fang::broker::RuntimeLifecycleState state, std::string message);
    bool enqueue(Command command);

    const std::wstring pipe_name_;
    const std::filesystem::path supervisor_executable_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> commands_;
    SupervisorRuntimeSnapshot snapshot_;
    std::string restart_avd_name_;
    bool stopping_{};
    bool busy_{};
    std::atomic<std::uint64_t> next_request_id_{1};
    std::thread worker_;
};

}
