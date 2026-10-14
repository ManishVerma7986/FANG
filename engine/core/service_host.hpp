#pragma once

#include "error.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace fang::core {

enum class LifecycleState {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed
};

class IService {
public:
    virtual ~IService() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
};

class ServiceHost {
public:
    ServiceHost() = default;
    ~ServiceHost();

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;

    Result<void> add_service(std::unique_ptr<IService> service);
    Result<void> start();
    Result<void> stop();
    LifecycleState state() const noexcept;

private:
    struct Entry {
        std::string name;
        std::unique_ptr<IService> service;
    };

    std::optional<CoreError> stop_service(Entry& entry);
    std::optional<CoreError> rollback_started(std::optional<CoreError> first_error = std::nullopt);
    void set_state(LifecycleState state) noexcept;

    mutable std::mutex state_mutex_;
    std::mutex operation_mutex_;
    LifecycleState state_{LifecycleState::Created};
    std::vector<Entry> services_;
    std::size_t started_count_{};
};

}