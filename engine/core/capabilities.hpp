#pragma once

#include "error.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fang::core {

enum class CapabilityState {
    Unavailable,
    Detected,
    Initialized,
    Verified
};

std::string_view capability_state_name(CapabilityState state) noexcept;

struct CapabilityReport {
    std::string name;
    CapabilityState state{CapabilityState::Unavailable};
    std::string evidence;
};

class CapabilityRegistry {
public:
    Result<void> define(std::string name);
    Result<void> report(std::string_view name, CapabilityState state, std::string evidence = {});
    std::vector<CapabilityReport> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::vector<CapabilityReport> reports_;
};

}