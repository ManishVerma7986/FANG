#pragma once

#include "input_ipc_protocol.hpp"
#include "input_transport.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fang::studio {

class FakeGuestInputEndpoint final {
public:
    explicit FakeGuestInputEndpoint(
        std::shared_ptr<LoopbackInputTransport> transport,
        std::string expected_session_id = "test_session",
        std::string expected_generation_id = "gen-1",
        uint32_t display_w = 1080,
        uint32_t display_h = 1920);

    // Reads and processes incoming packets from transport, responding with appropriate ACKs
    std::size_t poll_and_process();

    // Query guest-side tracking state
    bool is_contact_active(uint32_t contact_id) const noexcept;
    uint32_t active_contact_count() const noexcept;
    uint32_t active_contact_mask() const noexcept;
    uint64_t last_acked_sequence() const noexcept { return last_acked_seq_; }
    uint64_t total_frames_received() const noexcept { return total_frames_received_; }
    uint64_t total_keys_received() const noexcept { return total_keys_received_; }
    uint64_t total_cancels_received() const noexcept { return total_cancels_received_; }
    InputProtocolError last_reported_error() const noexcept { return last_reported_error_; }

    // Configuration for test scenarios
    void set_expected_generation(std::string gen_id) { expected_generation_id_ = std::move(gen_id); }
    void set_simulated_max_contacts(uint32_t max_c) { simulated_max_contacts_ = max_c; }
    void set_simulated_guest_version(uint16_t v) { simulated_guest_version_ = v; }
    void reset();

private:
    void handle_hello(const WireHeader& header, std::span<const std::byte> payload);
    void handle_touch_frame(const WireHeader& header, std::span<const std::byte> payload);
    void handle_key_event(const WireHeader& header, std::span<const std::byte> payload);
    void handle_cancel_all(const WireHeader& header, std::span<const std::byte> payload);
    void handle_ping(const WireHeader& header, std::span<const std::byte> payload);

    std::shared_ptr<LoopbackInputTransport> transport_;
    std::string expected_session_id_;
    std::string expected_generation_id_;
    uint32_t display_w_{1080};
    uint32_t display_h_{1920};

    uint32_t simulated_max_contacts_{input_max_simultaneous_contacts};
    uint16_t simulated_guest_version_{input_protocol_version};

    std::array<bool, input_max_simultaneous_contacts> active_contacts_{};
    std::array<WireTouchContact, input_max_simultaneous_contacts> contact_details_{};
    uint64_t last_acked_seq_{0};
    uint64_t total_frames_received_{0};
    uint64_t total_keys_received_{0};
    uint64_t total_cancels_received_{0};
    InputProtocolError last_reported_error_{InputProtocolError::None};

    std::vector<std::byte> incoming_buffer_;
};

} // namespace fang::studio
