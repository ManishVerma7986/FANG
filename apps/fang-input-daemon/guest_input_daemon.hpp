#pragma once

#include "guest_input_backend.hpp"
#include "input_ipc_protocol.hpp"
#include "input_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fang::guest {

using studio::IInputTransport;
using studio::InputMessageType;
using studio::InputProtocolError;
using studio::WireHeader;
using studio::WireTouchContact;

class GuestInputDaemon final {
public:
    enum class DaemonState : uint8_t {
        Uninitialized,
        AwaitingHello,
        Active,
        Disconnected,
        Error
    };

    struct Config {
        std::string expected_session_id{"default_session"};
        std::string expected_generation_id{"gen-1"};
        uint32_t display_width{1080};
        uint32_t display_height{1920};
        uint32_t max_contacts{10};
        uint64_t heartbeat_timeout_ms{5000};
        std::size_t max_buffer_bytes{65536};
    };

    GuestInputDaemon(
        std::shared_ptr<IInputTransport> transport,
        std::shared_ptr<IGuestInputBackend> backend,
        Config config);
    ~GuestInputDaemon();

    // Lifecycle
    bool start();
    void shutdown();
    bool is_active() const noexcept;
    DaemonState state() const noexcept;

    // Single-tick message processing (returns number of messages processed)
    std::size_t poll_and_process(uint64_t current_time_ms = 0);

    // Diagnostics & Metrics
    uint64_t total_frames_processed() const noexcept;
    uint64_t total_keys_processed() const noexcept;
    uint64_t total_cancels_processed() const noexcept;
    uint64_t total_acks_sent() const noexcept;
    uint64_t last_received_sequence() const noexcept;
    uint64_t last_acked_sequence() const noexcept;
    uint64_t duplicate_seq_count() const noexcept;
    uint64_t gap_seq_count() const noexcept;
    uint32_t active_contact_count() const noexcept;
    uint32_t active_contact_mask() const noexcept;
    bool is_contact_active(uint32_t contact_id) const noexcept;
    std::size_t held_key_count() const noexcept;
    uint32_t negotiated_max_contacts() const noexcept;
    InputProtocolError last_error() const noexcept;
    std::string status_diagnostic() const;

    // Test hooks
    void set_expected_generation(std::string gen_id);
    void set_expected_session(std::string sess_id);

private:
    void handle_hello(const WireHeader& header, std::span<const std::byte> payload);
    void handle_touch_frame(const WireHeader& header, std::span<const std::byte> payload);
    void handle_key_event(const WireHeader& header, std::span<const std::byte> payload);
    void handle_cancel_all(const WireHeader& header, std::span<const std::byte> payload);
    void handle_ping(const WireHeader& header, std::span<const std::byte> payload);
    void handle_disconnect(const WireHeader& header, std::span<const std::byte> payload);

    void send_error(uint64_t seq, InputProtocolError error, std::string message);

    uint32_t active_contact_count_locked() const noexcept;
    uint32_t active_contact_mask_locked() const noexcept;
    bool is_contact_active_locked(uint32_t contact_id) const noexcept;

    std::shared_ptr<IInputTransport> transport_;
    std::shared_ptr<IGuestInputBackend> backend_;
    Config config_;

    mutable std::recursive_mutex mutex_;
    DaemonState state_{DaemonState::Uninitialized};
    uint32_t negotiated_max_contacts_{0};

    uint64_t last_received_seq_{0};
    uint64_t last_acked_seq_{0};
    uint64_t last_packet_time_ms_{0};
    uint64_t duplicate_seq_count_{0};
    uint64_t gap_seq_count_{0};

    uint64_t total_frames_processed_{0};
    uint64_t total_keys_processed_{0};
    uint64_t total_cancels_processed_{0};
    uint64_t total_acks_sent_{0};

    InputProtocolError last_error_{InputProtocolError::None};
    std::string diagnostic_msg_;

    std::array<bool, 10> active_contacts_{};
    std::vector<int32_t> held_keys_;
    std::vector<std::byte> receive_buffer_;
};

} // namespace fang::guest
