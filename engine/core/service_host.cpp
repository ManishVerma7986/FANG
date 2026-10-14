#include "service_host.hpp"

#include <exception>
#include <string>
#include <utility>

namespace fang::core {

ServiceHost::~ServiceHost() noexcept {
    try {
        static_cast<void>(stop());
    } catch (...) {
    }
}

Result<void> ServiceHost::add_service(std::unique_ptr<IService> service) {
    std::lock_guard operation_lock(operation_mutex_);
    if (!service || service->name().empty()) {
        return Result<void>::failure({ErrorCode::InvalidService, "service and service name must be non-empty", {}});
    }

    std::lock_guard state_lock(state_mutex_);
    if (state_ != LifecycleState::Created) {
        return Result<void>::failure({
            ErrorCode::InvalidLifecycleTransition,
            "services can only be registered before startup",
            std::string(service->name())
        });
    }
    for (const auto& entry : services_) {
        if (entry.name == service->name()) {
            return Result<void>::failure({ErrorCode::DuplicateService, "service name is already registered", entry.name});
        }
    }
    services_.push_back({std::string(service->name()), std::move(service)});
    return Result<void>::success();
}

Result<void> ServiceHost::start() {
    std::lock_guard operation_lock(operation_mutex_);
    {
        std::lock_guard state_lock(state_mutex_);
        if (state_ != LifecycleState::Created) {
            return Result<void>::failure({
                ErrorCode::InvalidLifecycleTransition,
                "service host can only start once from the created state",
                {}
            });
        }
        state_ = LifecycleState::Starting;
    }

    for (auto& entry : services_) {
        try {
            auto result = entry.service->start();
            if (!result) {
                auto error = result.error();
                error.context = error.context.empty()
                    ? entry.name
                    : entry.name + ": " + error.context;
                auto rollback_error = stop_service(entry);
                rollback_error = rollback_started(std::move(rollback_error));
                if (rollback_error) {
                    error.context += (error.context.empty() ? "" : "; ");
                    error.context += "rollback failed in " + rollback_error->context + ": " + rollback_error->message;
                }
                set_state(LifecycleState::Failed);
                return Result<void>::failure(std::move(error));
            }
            ++started_count_;
        } catch (const std::exception& exception) {
            auto rollback_error = stop_service(entry);
            rollback_error = rollback_started(std::move(rollback_error));
            set_state(LifecycleState::Failed);
            return Result<void>::failure({
                ErrorCode::ServiceInitializationFailed,
                exception.what(),
                rollback_error
                    ? entry.name + "; rollback failed in " + rollback_error->context + ": " + rollback_error->message
                    : entry.name
            });
        } catch (...) {
            auto rollback_error = stop_service(entry);
            rollback_error = rollback_started(std::move(rollback_error));
            set_state(LifecycleState::Failed);
            return Result<void>::failure({
                ErrorCode::ServiceInitializationFailed,
                "service threw a non-standard exception",
                rollback_error
                    ? entry.name + "; rollback failed in " + rollback_error->context + ": " + rollback_error->message
                    : entry.name
            });
        }
    }

    set_state(LifecycleState::Running);
    return Result<void>::success();
}

Result<void> ServiceHost::stop() {
    std::lock_guard operation_lock(operation_mutex_);
    {
        std::lock_guard state_lock(state_mutex_);
        if (state_ == LifecycleState::Stopped) {
            return Result<void>::success();
        }
        if (state_ == LifecycleState::Created) {
            state_ = LifecycleState::Stopped;
            return Result<void>::success();
        }
        if (state_ != LifecycleState::Running && state_ != LifecycleState::Failed) {
            return Result<void>::failure({
                ErrorCode::InvalidLifecycleTransition,
                "service host cannot stop from its current state",
                {}
            });
        }
        state_ = LifecycleState::Stopping;
    }

    const auto shutdown_error = rollback_started();
    set_state(LifecycleState::Stopped);
    if (shutdown_error) {
        return Result<void>::failure(*shutdown_error);
    }
    return Result<void>::success();
}

LifecycleState ServiceHost::state() const noexcept {
    std::lock_guard lock(state_mutex_);
    return state_;
}

std::optional<CoreError> ServiceHost::stop_service(Entry& entry) {
    try {
        auto result = entry.service->stop();
        if (!result) {
            auto error = result.error();
            error.context = error.context.empty()
                ? entry.name
                : entry.name + ": " + error.context;
            return error;
        }
    } catch (const std::exception& exception) {
        return CoreError{ErrorCode::ServiceShutdownFailed, exception.what(), entry.name};
    } catch (...) {
        return CoreError{ErrorCode::ServiceShutdownFailed, "service threw a non-standard exception", entry.name};
    }
    return std::nullopt;
}

std::optional<CoreError> ServiceHost::rollback_started(std::optional<CoreError> first_error) {
    while (started_count_ > 0) {
        auto& entry = services_[--started_count_];
        auto error = stop_service(entry);
        if (!first_error && error) {
            first_error = std::move(error);
        }
    }
    return first_error;
}

void ServiceHost::set_state(LifecycleState state) noexcept {
    std::lock_guard lock(state_mutex_);
    state_ = state;
}

}