#include "broker_core.hpp"

#include <utility>

namespace fang::broker {

LifecycleState BrokerLifecycle::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

bool BrokerLifecycle::mark_ready() {
    std::lock_guard lock(mutex_);
    if (state_ != LifecycleState::Starting) {
        return false;
    }
    state_ = LifecycleState::Ready;
    return true;
}

bool BrokerLifecycle::request_shutdown() {
    std::lock_guard lock(mutex_);
    if (state_ != LifecycleState::Ready) {
        return false;
    }
    state_ = LifecycleState::ShuttingDown;
    return true;
}

bool BrokerLifecycle::mark_stopped() {
    std::lock_guard lock(mutex_);
    if (state_ != LifecycleState::ShuttingDown) {
        return false;
    }
    state_ = LifecycleState::Stopped;
    return true;
}

void BrokerLifecycle::fail(BrokerError error) {
    std::lock_guard lock(mutex_);
    if (state_ == LifecycleState::Stopped) {
        return;
    }
    state_ = LifecycleState::Failed;
    error_ = std::move(error);
}

BrokerError BrokerLifecycle::last_error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

BrokerProtocolService::BrokerProtocolService(BrokerLifecycle& lifecycle, std::size_t request_history)
    : lifecycle_(lifecycle), request_ids_(request_history) {}

BrokerReply BrokerProtocolService::dispatch(const Request& request) {
    const auto state = lifecycle_.state();
    if (state != LifecycleState::Ready) {
        return {BrokerErrorCode::ShuttingDown, state,
            "The broker is not accepting requests in its current lifecycle state."};
    }
    if (!request_ids_.observe(request.request_id)) {
        return {BrokerErrorCode::DuplicateRequestId, state,
            "The request identifier was already used recently."};
    }
    switch (request.type) {
    case RequestType::Ping:
        if (!request.payload.empty()) {
            return {BrokerErrorCode::InvalidMessage, state,
                "Ping does not accept a payload."};
        }
        return {BrokerErrorCode::None, state, "pong"};
    case RequestType::GetStatus:
        if (!request.payload.empty()) {
            return {BrokerErrorCode::InvalidMessage, state,
                "GetStatus does not accept a payload."};
        }
        return {BrokerErrorCode::None, state, "Broker is ready; runtime ownership is not integrated in Stage 1."};
    case RequestType::InitializeOwnerStore:
    case RequestType::QueryOwnership:
    case RequestType::ReserveIdentity:
    case RequestType::ReleaseReservation:
    case RequestType::FinalizeEmpty:
    case RequestType::ShutdownOwner:
        return {BrokerErrorCode::UnknownMessageType, state,
            "The requested operation is not implemented by the broker endpoint."};
    }
    return {BrokerErrorCode::UnknownMessageType, state, "The request type is not supported."};
}

}
