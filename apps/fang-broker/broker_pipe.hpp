#pragma once

#include "broker_core.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace fang::broker {

struct UserIdentity {
    std::string sid;
    std::uint32_t session_id{};
};

struct IdentityResult {
    bool succeeded{};
    UserIdentity identity;
    BrokerError error;
};

struct ClientIdentity {
    std::uint32_t process_id{};
    std::string sid;
    std::uint32_t token_session_id{};
    std::uint32_t process_session_id{};
};

IdentityResult current_user_identity();
std::wstring pipe_name_for_user(std::string_view user_sid);
std::wstring supervisor_pipe_name_for_user(std::string_view user_sid);
BrokerError authorize_client(std::string_view expected_user_sid, const ClientIdentity& client);

enum class PipeEndpointKind : std::uint8_t {
    Broker,
    Supervisor
};

class BrokerPipeServer {
public:
    BrokerPipeServer(std::wstring pipe_name, std::string expected_user_sid,
        BrokerLifecycle& lifecycle, IRequestDispatcher& dispatcher,
        std::uint32_t request_timeout_ms = 5000,
        PipeEndpointKind endpoint = PipeEndpointKind::Broker,
        std::optional<std::uint32_t> expected_session_id = std::nullopt);
    BrokerPipeServer(const BrokerPipeServer&) = delete;
    BrokerPipeServer& operator=(const BrokerPipeServer&) = delete;
    ~BrokerPipeServer();

    BrokerError run();
    bool request_stop() noexcept;

private:
    BrokerError serve_client(void* pipe);

    std::wstring pipe_name_;
    std::string expected_user_sid_;
    BrokerLifecycle& lifecycle_;
    IRequestDispatcher& dispatcher_;
    const std::uint32_t request_timeout_ms_;
    const PipeEndpointKind endpoint_;
    const std::optional<std::uint32_t> expected_session_id_;
    std::atomic_flag run_started_ = ATOMIC_FLAG_INIT;
    void* stop_event_{};
    std::uint32_t stop_event_error_{};
};

}
