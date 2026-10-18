#include "guest_input_bridge.hpp"

#include <chrono>
#include <sstream>

namespace fang::studio {

GuestInputBridgeClient::GuestInputBridgeClient(
    std::shared_ptr<IInputTransport> transport,
    Config config)
    : transport_(std::move(transport))
    , config_(std::move(config)) {}

GuestInputBridgeClient::~GuestInputBridgeClient() {
    stop();
}

bool GuestInputBridgeClient::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ConnectionState::Connected) {
        return true;
    }

    if (!transport_) {
        state_ = ConnectionState::Error;
        last_error_ = InputProtocolError::ConnectionClosed;
        diagnostic_override_ = "No input transport configured.";
        return false;
    }

    if (!transport_->is_connected()) {
        if (!transport_->connect()) {
            state_ = ConnectionState::Error;
            last_error_ = InputProtocolError::ConnectionClosed;
            diagnostic_override_ = "Transport connection failed.";
            return false;
        }
    }

    state_ = ConnectionState::Negotiating;
    diagnostic_override_ = "Negotiating capabilities with guest input service...";

    // Build and send Hello message
    HelloPayload hello;
    hello.client_version = input_protocol_version;
    hello.display_width = config_.display_width;
    hello.display_height = config_.display_height;
    hello.session_id = config_.session_id;
    hello.generation_id = config_.generation_id;

    const uint64_t seq = next_sequence_++;
    const uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    auto hello_pkt = encode_hello_message(seq, ts, hello);
    if (!transport_->send_bytes(hello_pkt)) {
        state_ = ConnectionState::Error;
        last_error_ = InputProtocolError::QueueOverflow;
        diagnostic_override_ = "Failed to send Hello message to guest.";
        return false;
    }

    // Try to process immediate ack if transport or mock is synchronous
    poll_incoming_acks_locked();

    return state_ != ConnectionState::Error;
}

void GuestInputBridgeClient::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ConnectionState::Connected && transport_ && transport_->is_connected()) {
        // Emit CancelAll to ensure guest clears contacts
        const uint64_t seq = next_sequence_++;
        const uint64_t ts = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        auto cancel_pkt = encode_cancel_all_message(seq, ts, CancelAllPayload{0});
        transport_->send_bytes(cancel_pkt);
    }

    if (transport_) {
        transport_->disconnect();
    }

    state_ = ConnectionState::Disconnected;
    capability_ = MultiTouchBackendCapability::NotConfigured;
    diagnostic_override_ = "Disconnected from guest input service.";
}

void GuestInputBridgeClient::reset_generation(std::string new_generation_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.generation_id == new_generation_id) {
        return;
    }

    // Cancel contacts on current generation before switching
    if (state_ == ConnectionState::Connected && transport_ && transport_->is_connected()) {
        const uint64_t seq = next_sequence_++;
        const uint64_t ts = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        auto cancel_pkt = encode_cancel_all_message(seq, ts, CancelAllPayload{0});
        transport_->send_bytes(cancel_pkt);
    }

    config_.generation_id = std::move(new_generation_id);
    state_ = ConnectionState::Disconnected;
    capability_ = MultiTouchBackendCapability::NotConfigured;
    diagnostic_override_ = "Generation changed to " + config_.generation_id + "; re-negotiation required.";
}

MultiTouchBackendCapability GuestInputBridgeClient::capability() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return capability_;
}

bool GuestInputBridgeClient::is_connected() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ConnectionState::Connected;
}

bool GuestInputBridgeClient::send_touch_frame(const std::vector<TouchContactState>& contacts) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ConnectionState::Connected || !transport_ || !transport_->is_connected()) {
        ++total_dropped_count_;
        last_error_ = InputProtocolError::ConnectionClosed;
        return false;
    }

    if (contacts.size() > negotiated_max_contacts_) {
        ++total_dropped_count_;
        last_error_ = InputProtocolError::ExceededContactLimit;
        return false;
    }

    TouchFramePayload payload;
    payload.contacts.reserve(contacts.size());
    for (const auto& cs : contacts) {
        WireTouchContact wc;
        wc.contact_id = cs.contact_id;
        wc.transition = cs.is_active ? 1 /*Move*/ : 2 /*Up*/;
        // If last_move_time == start_time and active, it is Down
        if (cs.is_active && cs.start_time_ms == cs.last_move_time_ms) {
            wc.transition = 0 /*Down*/;
        }
        wc.norm_u = cs.norm_pos.u;
        wc.norm_v = cs.norm_pos.v;
        wc.guest_x = cs.guest_x;
        wc.guest_y = cs.guest_y;
        payload.contacts.push_back(wc);
    }

    const uint64_t seq = next_sequence_++;
    const uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    auto pkt = encode_touch_frame_message(seq, ts, payload);
    if (!transport_->send_bytes(pkt)) {
        ++total_dropped_count_;
        last_error_ = InputProtocolError::QueueOverflow;
        return false;
    }

    ++total_sent_count_;
    return true;
}

bool GuestInputBridgeClient::send_key_event(int32_t android_keycode, bool is_down) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ConnectionState::Connected || !transport_ || !transport_->is_connected()) {
        ++total_dropped_count_;
        last_error_ = InputProtocolError::ConnectionClosed;
        return false;
    }

    KeyEventPayload payload{android_keycode, is_down};
    const uint64_t seq = next_sequence_++;
    const uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    auto pkt = encode_key_event_message(seq, ts, payload);
    if (!transport_->send_bytes(pkt)) {
        ++total_dropped_count_;
        last_error_ = InputProtocolError::QueueOverflow;
        return false;
    }

    ++total_sent_count_;
    return true;
}

bool GuestInputBridgeClient::cancel_all_contacts(uint32_t reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ConnectionState::Connected || !transport_ || !transport_->is_connected()) {
        return false;
    }

    const uint64_t seq = next_sequence_++;
    const uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    auto pkt = encode_cancel_all_message(seq, ts, CancelAllPayload{reason});
    if (!transport_->send_bytes(pkt)) {
        ++total_dropped_count_;
        last_error_ = InputProtocolError::QueueOverflow;
        return false;
    }

    ++total_sent_count_;
    return true;
}

void GuestInputBridgeClient::poll_incoming_acks() {
    std::lock_guard<std::mutex> lock(mutex_);
    poll_incoming_acks_locked();
}

void GuestInputBridgeClient::poll_incoming_acks_locked() {
    if (!transport_ || !transport_->is_connected()) {
        return;
    }

    auto bytes = transport_->receive_bytes(8192);
    if (!bytes.empty()) {
        receive_buffer_.insert(receive_buffer_.end(), bytes.begin(), bytes.end());
    }

    while (receive_buffer_.size() >= input_wire_header_size) {
        WireHeader header;
        const auto err = decode_wire_header(
            std::span<const std::byte>(receive_buffer_.data(), input_wire_header_size), header);
        if (err != InputProtocolError::None) {
            receive_buffer_.erase(receive_buffer_.begin());
            continue;
        }

        if (header.payload_size > input_maximum_payload_size) {
            receive_buffer_.erase(receive_buffer_.begin());
            continue;
        }

        const std::size_t total_msg_size = input_wire_header_size + header.payload_size;
        if (receive_buffer_.size() < total_msg_size) {
            break; // Awaiting full packet
        }

        std::span<const std::byte> payload(
            receive_buffer_.data() + input_wire_header_size, header.payload_size);

        if (header.type == InputMessageType::HelloAck) {
            InputProtocolError p_err = InputProtocolError::None;
            auto ack_opt = decode_hello_ack_message(payload, p_err);
            if (!ack_opt.has_value()) {
                state_ = ConnectionState::Error;
                last_error_ = p_err;
                capability_ = MultiTouchBackendCapability::Unsupported;
                diagnostic_override_ = "Failed to decode HelloAck from guest.";
            } else if (ack_opt->status != InputProtocolError::None) {
                state_ = ConnectionState::Error;
                last_error_ = ack_opt->status;
                capability_ = MultiTouchBackendCapability::Unsupported;
                diagnostic_override_ = "Guest rejected negotiation: " + std::string(input_protocol_error_name(ack_opt->status));
            } else if (ack_opt->generation_id != config_.generation_id) {
                state_ = ConnectionState::Error;
                last_error_ = InputProtocolError::StaleGeneration;
                capability_ = MultiTouchBackendCapability::Unsupported;
                diagnostic_override_ = "Guest generation ID mismatch: expected " + config_.generation_id + ", got " + ack_opt->generation_id;
            } else {
                negotiated_max_contacts_ = ack_opt->max_contacts;
                if (negotiated_max_contacts_ >= 2) {
                    capability_ = MultiTouchBackendCapability::Supported;
                } else {
                    capability_ = MultiTouchBackendCapability::Unsupported;
                }
                state_ = ConnectionState::Connected;
                last_error_ = InputProtocolError::None;
                diagnostic_override_ = "Connected. Multi-touch enabled (max " + std::to_string(negotiated_max_contacts_) + " contacts).";
            }
        } else if (header.type == InputMessageType::TouchAck) {
            InputProtocolError p_err = InputProtocolError::None;
            auto ack = decode_touch_ack_message(payload, p_err);
            if (ack.has_value()) {
                last_acked_seq_ = ack->acked_sequence;
                ++total_acked_count_;
                if (ack->status != InputProtocolError::None) {
                    last_error_ = ack->status;
                }
            }
        } else if (header.type == InputMessageType::KeyAck) {
            InputProtocolError p_err = InputProtocolError::None;
            auto ack = decode_key_ack_message(payload, p_err);
            if (ack.has_value()) {
                last_acked_seq_ = ack->acked_sequence;
                ++total_acked_count_;
            }
        } else if (header.type == InputMessageType::CancelAllAck) {
            InputProtocolError p_err = InputProtocolError::None;
            auto ack = decode_cancel_all_ack_message(payload, p_err);
            if (ack.has_value()) {
                last_acked_seq_ = ack->acked_sequence;
                ++total_acked_count_;
            }
        } else if (header.type == InputMessageType::Error) {
            InputProtocolError p_err = InputProtocolError::None;
            auto err_p = decode_error_message(payload, p_err);
            if (err_p.has_value()) {
                last_error_ = err_p->error_code;
            }
        }

        receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + total_msg_size);
    }
}

GuestInputBridgeClient::ConnectionState GuestInputBridgeClient::connection_state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

uint64_t GuestInputBridgeClient::total_sent_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_sent_count_;
}

uint64_t GuestInputBridgeClient::total_acked_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_acked_count_;
}

uint64_t GuestInputBridgeClient::total_dropped_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_dropped_count_;
}

uint64_t GuestInputBridgeClient::last_acked_sequence() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_acked_seq_;
}

uint32_t GuestInputBridgeClient::negotiated_max_contacts() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return negotiated_max_contacts_;
}

const std::string& GuestInputBridgeClient::session_id() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.session_id;
}

const std::string& GuestInputBridgeClient::generation_id() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.generation_id;
}

InputProtocolError GuestInputBridgeClient::last_error() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::string GuestInputBridgeClient::status_diagnostic() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream ss;
    switch (state_) {
    case ConnectionState::Disconnected: ss << "[Disconnected] "; break;
    case ConnectionState::Connecting:   ss << "[Connecting] "; break;
    case ConnectionState::Negotiating:  ss << "[Negotiating] "; break;
    case ConnectionState::Connected:    ss << "[Connected] "; break;
    case ConnectionState::Error:        ss << "[Error: " << input_protocol_error_name(last_error_) << "] "; break;
    }

    if (!diagnostic_override_.empty()) {
        ss << diagnostic_override_ << " ";
    }

    ss << "(Sent: " << total_sent_count_
       << ", Acked: " << total_acked_count_
       << ", Dropped: " << total_dropped_count_
       << ", Gen: " << config_.generation_id << ")";
    return ss.str();
}

} // namespace fang::studio
