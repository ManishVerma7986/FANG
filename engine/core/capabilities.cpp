#include "capabilities.hpp"

#include <algorithm>
#include <utility>

namespace fang::core {

std::string_view capability_state_name(CapabilityState state) noexcept {
    switch (state) {
    case CapabilityState::Unavailable: return "unavailable";
    case CapabilityState::Detected: return "detected";
    case CapabilityState::Initialized: return "initialized";
    case CapabilityState::Verified: return "verified";
    }
    return "unknown";
}

namespace {

int capability_rank(CapabilityState state) noexcept {
    switch (state) {
    case CapabilityState::Unavailable: return 0;
    case CapabilityState::Detected: return 1;
    case CapabilityState::Initialized: return 2;
    case CapabilityState::Verified: return 3;
    }
    return -1;
}

}

Result<void> CapabilityRegistry::define(std::string name) {
    if (name.empty()) {
        return Result<void>::failure({ErrorCode::InvalidCapabilityReport, "capability name must not be empty", {}});
    }

    std::lock_guard lock(mutex_);
    const auto existing = std::find_if(reports_.begin(), reports_.end(), [&name](const auto& report) {
        return report.name == name;
    });
    if (existing != reports_.end()) {
        return Result<void>::failure({ErrorCode::InvalidCapabilityReport, "capability is already defined", name});
    }
    reports_.push_back({std::move(name), CapabilityState::Unavailable, {}});
    return Result<void>::success();
}

Result<void> CapabilityRegistry::report(std::string_view name, CapabilityState state, std::string evidence) {
    if (capability_state_name(state) == "unknown") {
        return Result<void>::failure({ErrorCode::InvalidCapabilityReport, "capability state is invalid", std::string(name)});
    }

    std::lock_guard lock(mutex_);
    const auto existing = std::find_if(reports_.begin(), reports_.end(), [name](const auto& report) {
        return report.name == name;
    });
    if (existing == reports_.end()) {
        return Result<void>::failure({ErrorCode::InvalidCapabilityReport, "capability must be defined before it is reported", std::string(name)});
    }
    if (capability_rank(state) > capability_rank(existing->state) + 1) {
        return Result<void>::failure({
            ErrorCode::InvalidCapabilityReport,
            "capability evidence stages cannot be skipped",
            std::string(name)
        });
    }
    existing->state = state;
    existing->evidence = std::move(evidence);
    return Result<void>::success();
}

std::vector<CapabilityReport> CapabilityRegistry::snapshot() const {
    std::lock_guard lock(mutex_);
    return reports_;
}

}