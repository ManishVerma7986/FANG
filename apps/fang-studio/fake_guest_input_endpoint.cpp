#include "fake_guest_input_endpoint.hpp"

#include <cstring>

namespace fang::studio {

FakeGuestInputEndpoint::FakeGuestInputEndpoint(
    std::shared_ptr<LoopbackInputTransport> transport,
    std::string expected_session_id,
    std::string expected_generation_id,
    uint32_t display_w,
    uint32_t display_h)
    : transport_(std::move(transport))
    , expected_session_id_(std::move(expected_session_id))
    , expected_generation_id_(std::move(expected_generation_id))
    , display_w_(display_w)
    , display_h_(display_h) {}

void FakeGuestInputEndpoint::reset() {
    active_contacts_.fill(false);
    last_acked_seq_ = 0;
    total_frames_received_ = 0;
    total_keys_received_ = 0;
    total_cancels_received_ = 0;
    last_reported_error_ = InputProtocolError::None;
    incoming_buffer_.clear();
}

bool FakeGuestInputEndpoint::is_contact_active(uint32_t contact_id) const noexcept {
    if (contact_id >= input_max_simultaneous_contacts) return false;
    return active_contacts_[contact_id];
}

uint32_t FakeGuestInputEndpoint::active_contact_count() const noexcept {
    uint32_t count = 0;
    for (bool active : active_contacts_) {
        if (active) ++count;
    }
    return count;
}

uint32_t FakeGuestInputEndpoint::active_contact_mask() const noexcept {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < input_max_simultaneous_contacts; ++i) {
        if (active_contacts_[i]) {
            mask |= (1u << i);
        }
    }
    return mask;
}

std::size_t FakeGuestInputEndpoint::poll_and_process() {
    if (!transport_) return 0;

    auto new_bytes = transport_->guest_receive_bytes(8192);
    if (!new_bytes.empty()) {
        incoming_buffer_.insert(incoming_buffer_.end(), new_bytes.begin(), new_bytes.end());
    }

    std::size_t messages_processed = 0;
    while (incoming_buffer_.size() >= input_wire_header_size) {
        WireHeader header;
        const auto err = decode_wire_header(
            std::span<const std::byte>(incoming_buffer_.data(), input_wire_header_size), header);
        if (err != InputProtocolError::None) {
            last_reported_error_ = err;
            // Corrupt or invalid magic: drop 1 byte to seek next sync point
            incoming_buffer_.erase(incoming_buffer_.begin());
            continue;
        }

        const std::size_t total_msg_size = input_wire_header_size + header.payload_size;
        if (incoming_buffer_.size() < total_msg_size) {
            // Incomplete message; wait for more data
            break;
        }

        std::span<const std::byte> payload_span(
            incoming_buffer_.data() + input_wire_header_size, header.payload_size);

        switch (header.type) {
        case InputMessageType::Hello:
            handle_hello(header, payload_span);
            break;
        case InputMessageType::TouchFrame:
            handle_touch_frame(header, payload_span);
            break;
        case InputMessageType::KeyEvent:
            handle_key_event(header, payload_span);
            break;
        case InputMessageType::CancelAll:
            handle_cancel_all(header, payload_span);
            break;
        case InputMessageType::Ping:
            handle_ping(header, payload_span);
            break;
        default:
            last_reported_error_ = InputProtocolError::UnknownMessageType;
            break;
        }

        incoming_buffer_.erase(incoming_buffer_.begin(), incoming_buffer_.begin() + total_msg_size);
        ++messages_processed;
    }

    return messages_processed;
}

void FakeGuestInputEndpoint::handle_hello(
    const WireHeader& header, std::span<const std::byte> payload) {
    InputProtocolError parse_err = InputProtocolError::None;
    auto hello_opt = decode_hello_message(payload, parse_err);
    if (!hello_opt.has_value()) {
        last_reported_error_ = parse_err;
        ErrorPayload err_p{parse_err, "Malformed Hello payload."};
        auto pkt = encode_error_message(header.sequence_number, header.timestamp_ms, err_p);
        transport_->guest_send_bytes(pkt);
        return;
    }

    HelloAckPayload ack;
    ack.guest_version = simulated_guest_version_;
    ack.session_id = hello_opt->session_id;
    ack.generation_id = hello_opt->generation_id;
    ack.max_contacts = simulated_max_contacts_;
    ack.supported_caps = (simulated_max_contacts_ > 1) ? 0x07 : 0x03; // Touch + Key + (PointerAim if > 1)

    if (hello_opt->generation_id != expected_generation_id_) {
        ack.status = InputProtocolError::StaleGeneration;
        last_reported_error_ = InputProtocolError::StaleGeneration;
    } else if (simulated_guest_version_ != input_protocol_version) {
        ack.status = InputProtocolError::UnsupportedVersion;
        last_reported_error_ = InputProtocolError::UnsupportedVersion;
    } else {
        ack.status = InputProtocolError::None;
        last_reported_error_ = InputProtocolError::None;
    }

    auto pkt = encode_hello_ack_message(header.sequence_number, header.timestamp_ms, ack);
    transport_->guest_send_bytes(pkt);
}

void FakeGuestInputEndpoint::handle_touch_frame(
    const WireHeader& header, std::span<const std::byte> payload) {
    InputProtocolError parse_err = InputProtocolError::None;
    auto frame_opt = decode_touch_frame_message(payload, parse_err);
    if (!frame_opt.has_value()) {
        last_reported_error_ = parse_err;
        TouchAckPayload ack{header.sequence_number, active_contact_mask(), parse_err};
        auto pkt = encode_touch_ack_message(header.sequence_number, header.timestamp_ms, ack);
        transport_->guest_send_bytes(pkt);
        return;
    }

    InputProtocolError frame_err = InputProtocolError::None;
    for (const auto& contact : frame_opt->contacts) {
        if (contact.contact_id >= simulated_max_contacts_) {
            frame_err = InputProtocolError::ExceededContactLimit;
            break;
        }

        switch (contact.transition) {
        case 0: // Down
            active_contacts_[contact.contact_id] = true;
            contact_details_[contact.contact_id] = contact;
            break;
        case 1: // Move
            if (!active_contacts_[contact.contact_id]) {
                frame_err = InputProtocolError::InvalidTransition;
            } else {
                contact_details_[contact.contact_id] = contact;
            }
            break;
        case 2: // Up
            if (!active_contacts_[contact.contact_id]) {
                frame_err = InputProtocolError::InvalidTransition;
            }
            active_contacts_[contact.contact_id] = false;
            break;
        case 3: // Cancel
            active_contacts_[contact.contact_id] = false;
            break;
        default:
            frame_err = InputProtocolError::InvalidTransition;
            break;
        }
    }

    last_acked_seq_ = header.sequence_number;
    ++total_frames_received_;
    last_reported_error_ = frame_err;

    TouchAckPayload ack{header.sequence_number, active_contact_mask(), frame_err};
    auto pkt = encode_touch_ack_message(header.sequence_number, header.timestamp_ms, ack);
    transport_->guest_send_bytes(pkt);
}

void FakeGuestInputEndpoint::handle_key_event(
    const WireHeader& header, std::span<const std::byte> payload) {
    InputProtocolError parse_err = InputProtocolError::None;
    auto key_opt = decode_key_event_message(payload, parse_err);
    if (!key_opt.has_value()) {
        last_reported_error_ = parse_err;
        KeyAckPayload ack{header.sequence_number, parse_err};
        auto pkt = encode_key_ack_message(header.sequence_number, header.timestamp_ms, ack);
        transport_->guest_send_bytes(pkt);
        return;
    }

    last_acked_seq_ = header.sequence_number;
    ++total_keys_received_;
    last_reported_error_ = InputProtocolError::None;

    KeyAckPayload ack{header.sequence_number, InputProtocolError::None};
    auto pkt = encode_key_ack_message(header.sequence_number, header.timestamp_ms, ack);
    transport_->guest_send_bytes(pkt);
}

void FakeGuestInputEndpoint::handle_cancel_all(
    const WireHeader& header, std::span<const std::byte> payload) {
    InputProtocolError parse_err = InputProtocolError::None;
    auto cancel_opt = decode_cancel_all_message(payload, parse_err);
    if (!cancel_opt.has_value()) {
        last_reported_error_ = parse_err;
        CancelAllAckPayload ack{header.sequence_number, parse_err};
        auto pkt = encode_cancel_all_ack_message(header.sequence_number, header.timestamp_ms, ack);
        transport_->guest_send_bytes(pkt);
        return;
    }

    active_contacts_.fill(false);
    last_acked_seq_ = header.sequence_number;
    ++total_cancels_received_;
    last_reported_error_ = InputProtocolError::None;

    CancelAllAckPayload ack{header.sequence_number, InputProtocolError::None};
    auto pkt = encode_cancel_all_ack_message(header.sequence_number, header.timestamp_ms, ack);
    transport_->guest_send_bytes(pkt);
}

void FakeGuestInputEndpoint::handle_ping(
    const WireHeader& header, std::span<const std::byte> payload) {
    InputProtocolError parse_err = InputProtocolError::None;
    auto ping_opt = decode_ping_message(payload, parse_err);
    if (!ping_opt.has_value()) {
        return;
    }

    PongPayload pong{ping_opt->cookie};
    auto pkt = encode_pong_message(header.sequence_number, header.timestamp_ms, pong);
    transport_->guest_send_bytes(pkt);
}

} // namespace fang::studio
