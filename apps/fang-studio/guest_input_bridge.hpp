#pragma once

#include "input_ipc_protocol.hpp"
#include "input_mapping_types.hpp"
#include "input_transport.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
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
        return false;
    }
    bool send_key_event(int32_t /*android_keycode*/, bool /*is_down*/) override {
        return false;
    }
    std::string status_diagnostic() const override {
        return "ADB shell input interface cannot represent simultaneous multi-touch contacts or low-latency move streams. Multi-touch backend remains unconfigured pending a dedicated guest input bridge.";
    }
};

// Concrete host-side input bridge connecting InputMappingEngine to a guest input service
class GuestInputBridgeClient final : public IGuestInputBridge {
public:
    enum class ConnectionState : uint8_t {
        Disconnected,
        Connecting,
        Negotiating,
        Connected,
        Error
    };

    struct Config {
        std::string session_id{"default_session"};
        std::string generation_id{"gen-1"};
        uint32_t display_width{1080};
        uint32_t display_height{1920};
        std::size_t max_queue_depth{64};
    };

    explicit GuestInputBridgeClient(
        std::shared_ptr<IInputTransport> transport,
        Config config = {});
    ~GuestInputBridgeClient() override;

    // Connection lifecycle
    bool start();
    void stop();
    void reset_generation(std::string new_generation_id);

    // IGuestInputBridge implementation
    MultiTouchBackendCapability capability() const noexcept override;
    bool is_connected() const noexcept override;
    bool send_touch_frame(const std::vector<TouchContactState>& contacts) override;
    bool send_key_event(int32_t android_keycode, bool is_down) override;
    std::string status_diagnostic() const override;

    // Contact management
    bool cancel_all_contacts(uint32_t reason = 0);

    // Acknowledgement processing
    void poll_incoming_acks();

    // Diagnostics & Metrics
    ConnectionState connection_state() const noexcept;
    uint64_t total_sent_count() const noexcept;
    uint64_t total_acked_count() const noexcept;
    uint64_t total_dropped_count() const noexcept;
    uint64_t last_acked_sequence() const noexcept;
    uint32_t negotiated_max_contacts() const noexcept;
    const std::string& session_id() const noexcept;
    const std::string& generation_id() const noexcept;
    InputProtocolError last_error() const noexcept;

private:
    std::shared_ptr<IInputTransport> transport_;
    Config config_;

    mutable std::mutex mutex_;
    ConnectionState state_{ConnectionState::Disconnected};
    MultiTouchBackendCapability capability_{MultiTouchBackendCapability::NotConfigured};
    uint32_t negotiated_max_contacts_{0};
    uint64_t next_sequence_{1};
    uint64_t total_sent_count_{0};
    uint64_t total_acked_count_{0};
    uint64_t total_dropped_count_{0};
    uint64_t last_acked_seq_{0};
    InputProtocolError last_error_{InputProtocolError::None};
    std::string diagnostic_override_;

    std::vector<std::byte> receive_buffer_;
    void poll_incoming_acks_locked();
};

} // namespace fang::studio
