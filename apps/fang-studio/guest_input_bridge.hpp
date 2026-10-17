#pragma once

#include "input_mapping_types.hpp"

#include <string>
#include <vector>

namespace fang::studio {

class IGuestInputBridge {
public:
    virtual ~IGuestInputBridge() = default;
    virtual MultiTouchBackendCapability capability() const noexcept = 0;
    virtual bool is_connected() const noexcept = 0;
    virtual bool send_touch_frame(const std::vector<TouchContactState>& contacts) = 0;
    virtual bool send_key_event(int32_t android_keycode, bool is_down) = 0;
    virtual std::string status_diagnostic() const = 0;
};

// Default unconfigured bridge exposing the capability gap of ADB shell input
class UnconfiguredAdbGuestInputBridge final : public IGuestInputBridge {
public:
    MultiTouchBackendCapability capability() const noexcept override {
        return MultiTouchBackendCapability::Unsupported;
    }
    bool is_connected() const noexcept override { return false; }
    bool send_touch_frame(const std::vector<TouchContactState>& /*contacts*/) override {
        // ADB CLI input cannot send simultaneous multi-touch frames
        return false;
    }
    bool send_key_event(int32_t /*android_keycode*/, bool /*is_down*/) override {
        return false;
    }
    std::string status_diagnostic() const override {
        return "ADB shell input interface cannot represent simultaneous multi-touch contacts or low-latency move streams. Multi-touch backend remains unconfigured pending a dedicated guest input bridge.";
    }
};

} // namespace fang::studio
