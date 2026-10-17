#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "broker_pipe.hpp"
#include "supervisor_runtime_client.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace fang::studio {
namespace {

using namespace fang::broker;

std::uint64_t next_request_id(std::atomic<std::uint64_t>& counter) noexcept {
    auto value = counter.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) value = counter.fetch_add(1, std::memory_order_relaxed);
    return value;
}

std::string transport_message(const BrokerCallResult& result) {
    if (result.transport_error) return result.transport_error.message;
    return "The supervisor returned no reply.";
}

}

SupervisorRuntimeClient::SupervisorRuntimeClient(std::wstring pipe_name,
        std::filesystem::path supervisor_executable)
    : pipe_name_(std::move(pipe_name)), supervisor_executable_(std::move(supervisor_executable)),
      worker_([this] { worker_loop(); }) {}

SupervisorRuntimeClient::~SupervisorRuntimeClient() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool SupervisorRuntimeClient::enqueue(Command command) {
    std::lock_guard lock(mutex_);
    if (stopping_ || busy_ || !commands_.empty()) return false;
    commands_.push_back(std::move(command));
    snapshot_.request_pending = true;
    condition_.notify_one();
    return true;
}

bool SupervisorRuntimeClient::start(std::string avd_name) {
    std::lock_guard lock(mutex_);
    const auto state = snapshot_.runtime.state;
    if (stopping_ || busy_ || !commands_.empty() ||
                (state != fang::broker::RuntimeLifecycleState::Stopped &&
                 state != fang::broker::RuntimeLifecycleState::Failed &&
                 !(state == fang::broker::RuntimeLifecycleState::RecoveryRequired &&
                     snapshot_.runtime.generation_id.empty()))) return false;
    restart_avd_name_.clear();
    commands_.push_back({CommandKind::Start, std::move(avd_name)});
    snapshot_.request_pending = true;
    condition_.notify_one();
    return true;
}

bool SupervisorRuntimeClient::shutdown() {
    std::lock_guard lock(mutex_);
    if (stopping_ || std::any_of(commands_.begin(), commands_.end(), [](const auto& command) {
            return command.kind == CommandKind::Shutdown;
        }) || (commands_.empty() && !busy_ &&
            snapshot_.runtime.state == fang::broker::RuntimeLifecycleState::Stopped)) return false;
    commands_.push_back({CommandKind::Shutdown, {}});
    snapshot_.request_pending = true;
    condition_.notify_one();
    return true;
}

bool SupervisorRuntimeClient::restart(std::string avd_name) {
    std::lock_guard lock(mutex_);
    const auto state = snapshot_.runtime.state;
    if (stopping_ || busy_ || !commands_.empty() ||
        state == fang::broker::RuntimeLifecycleState::Stopping ||
        state == fang::broker::RuntimeLifecycleState::RecoveryRequired) return false;
    if (state == fang::broker::RuntimeLifecycleState::Stopped ||
        (state == fang::broker::RuntimeLifecycleState::Failed &&
         snapshot_.runtime.generation_id.empty())) {
        commands_.push_back({CommandKind::Start, std::move(avd_name)});
    } else {
        restart_avd_name_ = std::move(avd_name);
        commands_.push_back({CommandKind::Shutdown, {}});
    }
    snapshot_.request_pending = true;
    condition_.notify_one();
    return true;
}

SupervisorRuntimeSnapshot SupervisorRuntimeClient::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void SupervisorRuntimeClient::set_failure(BrokerErrorCode code,
        RuntimeLifecycleState state, std::string message) {
    std::lock_guard lock(mutex_);
    snapshot_.error = code;
    snapshot_.runtime.state = state;
    snapshot_.runtime.failure_stage = RuntimeFailureStage::Unknown;
    snapshot_.runtime.diagnostic = message.substr(0, 1024);
    snapshot_.message = std::move(message);
    snapshot_.request_pending = false;
}

bool SupervisorRuntimeClient::ensure_supervisor(std::string& error) {
    const auto ping = [this] {
        BrokerPipeClient client(pipe_name_, 350);
        return client.call({RequestType::Ping,
            next_request_id(next_request_id_), {}});
    };
    auto result = ping();
    if (!result.transport_error && result.reply.has_value() &&
        result.reply->error == BrokerErrorCode::None) return true;

    const ULONGLONG initial_deadline = GetTickCount64() + 1500;
    while (GetTickCount64() < initial_deadline) {
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, std::chrono::milliseconds(50), [&] { return stopping_; });
        if (stopping_) break;
        result = ping();
        if (!result.transport_error && result.reply.has_value() &&
            result.reply->error == BrokerErrorCode::None) return true;
    }

    if (supervisor_executable_.empty() ||
        !std::filesystem::is_regular_file(supervisor_executable_)) {
        error = "The sibling fang-runtime-supervisor.exe is unavailable. Build the supervisor target first.";
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    auto command = L"\"" + supervisor_executable_.wstring() + L"\"";
    if (CreateProcessW(supervisor_executable_.c_str(), command.data(), nullptr,
            nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
            nullptr, supervisor_executable_.parent_path().c_str(), &startup,
            &process) != 0) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else if (GetLastError() != ERROR_ACCESS_DENIED) {
        error = "Could not start the per-user runtime supervisor; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        result = ping();
        if (!result.transport_error && result.reply.has_value() &&
            result.reply->error == BrokerErrorCode::None) return true;
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, std::chrono::milliseconds(100),
            [&] { return stopping_; });
        if (stopping_) break;
    }
    error = "The runtime supervisor did not become reachable within 10 seconds: " +
        transport_message(result);
    return false;
}

void SupervisorRuntimeClient::apply_reply(const BrokerCallResult& result) {
    std::lock_guard lock(mutex_);
    snapshot_.request_pending = false;
    if (result.transport_error || !result.reply.has_value()) {
        snapshot_.error = BrokerErrorCode::RecoveryRequired;
        snapshot_.runtime.state = snapshot_.runtime.generation_id.empty()
            ? RuntimeLifecycleState::Failed : RuntimeLifecycleState::RecoveryRequired;
        snapshot_.runtime.failure_stage = RuntimeFailureStage::VerifyOwnership;
        snapshot_.message = transport_message(result);
        snapshot_.runtime.diagnostic = snapshot_.message.substr(0, 1024);
        return;
    }
    snapshot_.error = result.reply->error;
    snapshot_.message = result.reply->message;
    if (result.reply->data.empty()) {
        if (result.reply->error != BrokerErrorCode::None) {
            snapshot_.runtime.state = RuntimeLifecycleState::Failed;
            snapshot_.runtime.diagnostic = result.reply->message.substr(0, 1024);
        }
        return;
    }
    const auto status = decode_runtime_status_data(result.reply->data);
    if (!status.has_value()) {
        snapshot_.error = BrokerErrorCode::InvalidMessage;
        snapshot_.runtime.state = RuntimeLifecycleState::RecoveryRequired;
        snapshot_.runtime.failure_stage = RuntimeFailureStage::Unknown;
        snapshot_.runtime.diagnostic = "Supervisor runtime status payload was malformed.";
        snapshot_.message = snapshot_.runtime.diagnostic;
        return;
    }
    snapshot_.runtime = *status;
}

bool SupervisorRuntimeClient::send_initialize() {
    BrokerPipeClient client(pipe_name_, 5000);
    const auto result = client.call({RequestType::InitializeOwnerStore,
        next_request_id(next_request_id_), {}});
    apply_reply(result);
    return !result.transport_error && result.reply.has_value() &&
        result.reply->error == BrokerErrorCode::None;
}

bool SupervisorRuntimeClient::send_start(std::string_view avd_name) {
    std::string error;
    if (!ensure_supervisor(error)) {
        set_failure(BrokerErrorCode::RecoveryRequired,
            RuntimeLifecycleState::RecoveryRequired, error);
        return false;
    }
    if (!send_initialize()) return false;
    BrokerCallResult result;
    try {
        BrokerPipeClient client(pipe_name_, 5000);
        result = client.call({RequestType::StartReferenceRuntime,
            next_request_id(next_request_id_),
            encode_start_reference_runtime_request({
                reference_runtime_configuration_version, std::string(avd_name)})});
    } catch (const std::exception& exception) {
        std::lock_guard lock(mutex_);
        snapshot_.error = BrokerErrorCode::InvalidRuntimeConfiguration;
        snapshot_.runtime.state = RuntimeLifecycleState::Failed;
        snapshot_.runtime.failure_stage = RuntimeFailureStage::ValidateConfiguration;
        snapshot_.runtime.diagnostic = std::string(exception.what()).substr(0, 1024);
        snapshot_.message = snapshot_.runtime.diagnostic;
        snapshot_.request_pending = false;
        return false;
    }
    apply_reply(result);
    return !result.transport_error && result.reply.has_value() &&
        result.reply->error == BrokerErrorCode::None;
}

bool SupervisorRuntimeClient::send_shutdown() {
    const auto current = snapshot();
    if (current.runtime.generation_id.empty()) {
        set_failure(BrokerErrorCode::RuntimeNotFound,
            RuntimeLifecycleState::Failed, "There is no active runtime generation to shut down.");
        return false;
    }
    BrokerCallResult result;
    try {
        BrokerPipeClient client(pipe_name_, 5000);
        result = client.call({RequestType::ShutdownRuntime,
            next_request_id(next_request_id_),
            encode_runtime_generation_request({current.runtime.generation_id})});
    } catch (const std::exception& exception) {
        set_failure(BrokerErrorCode::InvalidMessage,
            RuntimeLifecycleState::Failed, exception.what());
        return false;
    }
    apply_reply(result);
    return !result.transport_error && result.reply.has_value() &&
        (result.reply->error == BrokerErrorCode::None ||
         result.reply->error == BrokerErrorCode::RecoveryRequired);
}

bool SupervisorRuntimeClient::query_status() {
    const auto current = snapshot();
    if (current.runtime.generation_id.empty()) return false;
    BrokerCallResult result;
    try {
        BrokerPipeClient client(pipe_name_, 10000);
        result = client.call({RequestType::QueryRuntimeStatus,
            next_request_id(next_request_id_),
            encode_runtime_generation_request({current.runtime.generation_id})});
    } catch (const std::exception& exception) {
        set_failure(BrokerErrorCode::InvalidMessage,
            RuntimeLifecycleState::Failed, exception.what());
        return false;
    }
    apply_reply(result);
    return !result.transport_error && result.reply.has_value() &&
        (result.reply->error == BrokerErrorCode::None ||
         result.reply->error == BrokerErrorCode::RecoveryRequired);
}

void SupervisorRuntimeClient::worker_loop() {
    auto next_poll = std::chrono::steady_clock::now();
    while (true) {
        std::optional<Command> command;
        bool should_poll{};
        {
            std::unique_lock lock(mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return stopping_ || !commands_.empty();
            });
            if (stopping_) return;
            if (!commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
                busy_ = true;
            } else if (!snapshot_.runtime.generation_id.empty() &&
                snapshot_.runtime.state != RuntimeLifecycleState::Stopped &&
                snapshot_.runtime.state != RuntimeLifecycleState::Failed &&
                std::chrono::steady_clock::now() >= next_poll) {
                busy_ = true;
                should_poll = true;
            }
        }

        if (command.has_value()) {
            switch (command->kind) {
            case CommandKind::Start:
                send_start(command->avd_name);
                break;
            case CommandKind::Shutdown:
                send_shutdown();
                break;
            case CommandKind::Restart:
                break;
            }
        } else if (should_poll) {
            query_status();
        }

        std::string restart_avd;
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
            if (!restart_avd_name_.empty() &&
                snapshot_.runtime.state == RuntimeLifecycleState::Stopped) {
                restart_avd = std::move(restart_avd_name_);
                restart_avd_name_.clear();
                commands_.push_back({CommandKind::Start, restart_avd});
                snapshot_.request_pending = true;
            }
            if (should_poll || command.has_value()) {
                const auto state = snapshot_.runtime.state;
                next_poll = std::chrono::steady_clock::now() +
                    (state == RuntimeLifecycleState::RecoveryRequired
                        ? std::chrono::seconds(2) : std::chrono::milliseconds(500));
            }
        }
    }
}

}
