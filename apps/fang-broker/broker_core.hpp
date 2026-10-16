#pragma once

#include "broker_protocol.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace fang::broker {

struct BrokerError {
    BrokerErrorCode code{BrokerErrorCode::None};
    std::uint32_t native_error{};
    std::string message;

    explicit operator bool() const noexcept {
        return code != BrokerErrorCode::None;
    }
};

class BrokerLifecycle {
public:
    LifecycleState state() const;
    bool mark_ready();
    bool request_shutdown();
    bool mark_stopped();
    void fail(BrokerError error);
    BrokerError last_error() const;

private:
    mutable std::mutex mutex_;
    LifecycleState state_{LifecycleState::Starting};
    BrokerError error_;
};

class BrokerProtocolService final : public IRequestDispatcher {
public:
    explicit BrokerProtocolService(BrokerLifecycle& lifecycle, std::size_t request_history = 4096);

    BrokerReply dispatch(const Request& request) override;

private:
    BrokerLifecycle& lifecycle_;
    RequestIdWindow request_ids_;
};

}
