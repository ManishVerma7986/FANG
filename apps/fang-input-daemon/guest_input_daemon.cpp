#include "guest_input_daemon.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace fang::guest {

using namespace studio;

GuestInputDaemon::GuestInputDaemon(
    std::shared_ptr<IInputTransport> transport,
    std::shared_ptr<IGuestInputBackend> backend,
    Config config)
    : transport_(std::move(transport))
    , backend_(std::move(backend))
    , config_(std::move(config)) {}

GuestInputDaemon::~GuestInputDaemon() {
    shutdown();
}

bool GuestInputDaemon::start() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (state_ == DaemonState::Active || state_ == DaemonState::AwaitingHello) {
        return true;
    }

    if (!transport_) {
        state_ = DaemonState::Error;
        last_error_ = InputProtocolError::ConnectionClosed;
        diagnostic_msg_ = "No input transport provided to daemon.";
        return false;
    }

    if (!transport_->is_connected()) {
        if (!transport_->connect()) {
            state_ = DaemonState::Error;
            last_error_ = InputProtocolError::ConnectionClosed;
            diagnostic_msg_ = "Failed to connect transport.";
            return false;
        }
    }

    state_ = DaemonState::AwaitingHello;
    diagnostic_msg_ = "Daemon started; awaiting Hello message.";
    return true;
}

void GuestInputDaemon::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (backend_) {
        backend_->cancel_all_contacts_and_keys();
        backend_->shutdown();
    }
    active_contacts_.fill(false);
    held_keys_.clear();

    if (transport_) {
        transport_->disconnect();
    }
    state_ = DaemonState::Disconnected;
    diagnostic_msg_ = "Daemon shut down; all contacts and keys cleared.";
}

bool GuestInputDaemon::is_active() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return state_ == DaemonState::Active;
}

GuestInputDaemon::DaemonState GuestInputDaemon::state() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return state_;
}

std::size_t GuestInputDaemon::poll_and_process(uint64_t current_time_ms) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // DEBUG
    static uint64_t poll_call_count = 0;
    poll_call_count++;
    if (poll_call_count % 1000000 == 0) {
        bool trans_connected = transport_ ? transport_->is_connected() : false;
        std::cerr << "[DEBUG poll] call=" << poll_call_count 
                  << " state=" << static_cast<int>(state_)
                  << " transport_connected=" << trans_connected
                  << std::endl;
    }

    // 1. Heartbeat timeout check
    if (current_time_ms > 0 && last_packet_time_ms_ > 0 && state_ == DaemonState::Active) {
        if (config_.heartbeat_timeout_ms > 0 &&
            (current_time_ms - last_packet_time_ms_) > config_.heartbeat_timeout_ms) {
            if (backend_) {
                backend_->cancel_all_contacts_and_keys();
            }
            active_contacts_.fill(false);
            held_keys_.clear();
            state_ = DaemonState::Disconnected;
            last_error_ = InputProtocolError::ConnectionClosed;
            diagnostic_msg_ = "Peer connection timed out (heartbeat expired).";
            return 0;
        }
    }

if (!transport_ || !transport_->is_connected()) {
          std::cerr << "[DEBUG poll] transport not connected! transport_=" << (transport_ ? "valid" : "null") 
                    << " is_connected=" << (transport_ ? transport_->is_connected() : false) << std::endl;
          if (state_ == DaemonState::Active) {
              if (backend_) {
                  backend_->cancel_all_contacts_and_keys();
              }
              active_contacts_.fill(false);
              held_keys_.clear();
              state_ = DaemonState::Disconnected;
          }
          return 0;
      }

    // 2. Fetch bytes from transport
auto incoming = transport_->receive_bytes(4096);
      if (!incoming.empty()) {
          // DEBUG
          std::cerr << "[DEBUG recv] received " << incoming.size() << " bytes, buffer was " << receive_buffer_.size() << std::endl;
          receive_buffer_.insert(receive_buffer_.end(), incoming.begin(), incoming.end());
      }

    if (receive_buffer_.size() > config_.max_buffer_bytes) {
        receive_buffer_.clear();
        last_error_ = InputProtocolError::QueueOverflow;
        diagnostic_msg_ = "Receive buffer exceeded max capacity; cleared.";
        return 0;
    }

// 3. Process framed packets
      std::size_t processed_count = 0;
      while (receive_buffer_.size() >= input_wire_header_size) {
          WireHeader header;
          const auto err = decode_wire_header(
              std::span<const std::byte>(receive_buffer_.data(), input_wire_header_size), header);

          // DEBUG
          std::cerr << "[DEBUG msg] buffer_size=" << receive_buffer_.size()
                    << " err=" << static_cast<int>(err)
                    << " magic=0x" << std::hex << std::uppercase << header.magic
                    << std::dec << " version=" << header.version
                    << " type=" << static_cast<int>(header.type)
                    << " payload_size=" << header.payload_size
                    << " seq=" << header.sequence_number << std::endl;

          if (err == InputProtocolError::InvalidMagic) {
              std::cerr << "[DEBUG msg] InvalidMagic, erasing 1 byte" << std::endl;
              receive_buffer_.erase(receive_buffer_.begin());
              continue;
          }

          if (err != InputProtocolError::None) {
              std::cerr << "[DEBUG msg] Header decode error: " << static_cast<int>(err) << std::endl;
              if (err == InputProtocolError::UnsupportedVersion ||
                  err == InputProtocolError::UnknownMessageType ||
                  err == InputProtocolError::OversizedPayload) {
                  send_error(0, err, "Header decode error: " + std::string(input_protocol_error_name(err)));
                  state_ = DaemonState::Error;
                  last_error_ = err;
              }
              receive_buffer_.erase(receive_buffer_.begin());
              continue;
          }

        if (header.payload_size > input_maximum_payload_size) {
            send_error(header.sequence_number, InputProtocolError::OversizedPayload, "Payload exceeds 4096 bytes");
            receive_buffer_.erase(receive_buffer_.begin());
            continue;
        }

        const std::size_t total_msg_size = input_wire_header_size + header.payload_size;
        if (receive_buffer_.size() < total_msg_size) {
            break; // Awaiting full packet payload
        }

        std::span<const std::byte> payload(
            receive_buffer_.data() + input_wire_header_size, header.payload_size);

        last_packet_time_ms_ = (current_time_ms > 0) ? current_time_ms : header.timestamp_ms;

        // Monotonic sequence verification
        if (header.sequence_number > 0) {
            if (last_received_seq_ > 0 && header.sequence_number <= last_received_seq_) {
                ++duplicate_seq_count_;
            } else {
                if (last_received_seq_ > 0 && header.sequence_number > last_received_seq_ + 1) {
                    ++gap_seq_count_;
                }
                last_received_seq_ = header.sequence_number;
            }
        }

        switch (header.type) {
        case InputMessageType::Hello:
            handle_hello(header, payload);
            break;
        case InputMessageType::TouchFrame:
            handle_touch_frame(header, payload);
            break;
        case InputMessageType::KeyEvent:
            handle_key_event(header, payload);
            break;
        case InputMessageType::CancelAll:
            handle_cancel_all(header, payload);
            break;
        case InputMessageType::Ping:
            handle_ping(header, payload);
            break;
        case InputMessageType::Disconnect:
            handle_disconnect(header, payload);
            break;
        default:
            send_error(header.sequence_number, InputProtocolError::UnknownMessageType, "Unsupported message type");
            break;
        }

        receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + total_msg_size);
        ++processed_count;
    }

    return processed_count;
}
void GuestInputDaemon::handle_hello(const WireHeader& header, std::span<const std::byte> payload) {
      std::cerr << "[DEBUG handle_hello] called with seq=" << header.sequence_number << std::endl;
      InputProtocolError parse_err = InputProtocolError::None;
      auto hello = decode_hello_message(payload, parse_err);
      if (!hello.has_value()) {
          std::cerr << "[DEBUG handle_hello] decode_hello_message failed: " << static_cast<int>(parse_err) << std::endl;
          send_error(header.sequence_number, parse_err, "Failed to decode Hello message.");
          state_ = DaemonState::Error;
          last_error_ = parse_err;
          return;
      }

      std::cerr << "[DEBUG handle_hello] session_id=" << hello->session_id << " expected=" << config_.expected_session_id << std::endl;
      std::cerr << "[DEBUG handle_hello] generation_id=" << hello->generation_id << " expected=" << config_.expected_generation_id << std::endl;

      if (hello->generation_id != config_.expected_generation_id) {
          std::cerr << "[DEBUG handle_hello] Generation ID mismatch" << std::endl;
          state_ = DaemonState::Error;
          last_error_ = InputProtocolError::StaleGeneration;
          diagnostic_msg_ = "Generation ID mismatch: expected " + config_.expected_generation_id + ", got " + 
hello->generation_id;

          HelloAckPayload ack;
          ack.guest_version = input_protocol_version;
          ack.status = InputProtocolError::StaleGeneration;
          ack.max_contacts = 0;
          ack.supported_caps = 0;
          ack.session_id = hello->session_id;
          ack.generation_id = config_.expected_generation_id;
          auto pkt = encode_hello_ack_message(header.sequence_number, header.timestamp_ms, ack);
          std::cerr << "[DEBUG handle_hello] Sending StaleGeneration HelloAck" << std::endl;
          transport_->send_bytes(pkt);
          return;
      }

      if (hello->session_id != config_.expected_session_id) {
          std::cerr << "[DEBUG handle_hello] Session ID mismatch" << std::endl;
          state_ = DaemonState::Error;
          last_error_ = InputProtocolError::SessionMismatch;
          diagnostic_msg_ = "Session ID mismatch: expected " + config_.expected_session_id + ", got " + 
hello->session_id;

          HelloAckPayload ack;
          ack.guest_version = input_protocol_version;
          ack.status = InputProtocolError::SessionMismatch;
          ack.max_contacts = 0;
          ack.supported_caps = 0;
          ack.session_id = config_.expected_session_id;
          ack.generation_id = config_.expected_generation_id;
          auto pkt = encode_hello_ack_message(header.sequence_number, header.timestamp_ms, ack);
          std::cerr << "[DEBUG handle_hello] Sending SessionMismatch HelloAck" << std::endl;
          transport_->send_bytes(pkt);
          return;
      }

      std::cerr << "[DEBUG handle_hello] IDs match, initializing backend" << std::endl;
      config_.display_width = hello->display_width;
      config_.display_height = hello->display_height;
      negotiated_max_contacts_ = std::min({config_.max_contacts, 10u, input_max_simultaneous_contacts});

      if (backend_) {
          std::cerr << "[DEBUG handle_hello] Calling backend_->initialize" << std::endl;
          backend_->initialize(config_.display_width, config_.display_height, negotiated_max_contacts_);
          std::cerr << "[DEBUG handle_hello] backend_->initialize done" << std::endl;
      }

      HelloAckPayload ack;
      ack.guest_version = input_protocol_version;
      ack.status = InputProtocolError::None;
      ack.max_contacts = negotiated_max_contacts_;
      ack.supported_caps = backend_ ? backend_->supported_capabilities() : 0;
      ack.session_id = config_.expected_session_id;
      ack.generation_id = config_.expected_generation_id;

      std::cerr << "[DEBUG handle_hello] Sending HelloAck, ack.max_contacts=" << ack.max_contacts << " ack.supported_caps=" << ack.supported_caps << std::endl;
      auto pkt = encode_hello_ack_message(header.sequence_number, header.timestamp_ms, ack);
      std::cerr << "[DEBUG handle_hello] Calling transport_->send_bytes" << std::endl;
      transport_->send_bytes(pkt);
      std::cerr << "[DEBUG handle_hello] send_bytes returned" << std::endl;
      state_ = DaemonState::Active;
      last_error_ = InputProtocolError::None;
      last_acked_seq_ = header.sequence_number;
      ++total_acks_sent_;
      std::cerr << "[DEBUG handle_hello] Done, state=Active" << std::endl;
  }
void GuestInputDaemon::handle_touch_frame(const WireHeader& header, std::span<const std::byte> payload) {
    if (state_ != DaemonState::Active) {
        send_error(header.sequence_number, InputProtocolError::ConnectionClosed, "Daemon is not active.");
        return;
    }

    InputProtocolError parse_err = InputProtocolError::None;
    auto frame = decode_touch_frame_message(payload, parse_err);
    if (!frame.has_value()) {
        last_error_ = parse_err;
        TouchAckPayload ack{header.sequence_number, active_contact_mask_locked(), parse_err};
        auto pkt = encode_touch_ack_message(header.sequence_number, header.timestamp_ms, ack);
        transport_->send_bytes(pkt);
        return;
    }

    if (frame->contacts.size() > negotiated_max_contacts_) {
        last_error_ = InputProtocolError::ExceededContactLimit;
        TouchAckPayload ack{header.sequence_number, active_contact_mask_locked(), InputProtocolError::ExceededContactLimit};
        auto pkt = encode_touch_ack_message(header.sequence_number, header.timestamp_ms, ack);
        transport_->send_bytes(pkt);
        return;
    }

    bool backend_failed = false;
    for (const auto& c : frame->contacts) {
        if (c.contact_id >= active_contacts_.size()) {
            continue;
        }

        if (c.transition == 0 /*Down*/ || c.transition == 1 /*Move*/) {
            active_contacts_[c.contact_id] = true;
        } else if (c.transition == 2 /*Up*/ || c.transition == 3 /*Cancel*/) {
            active_contacts_[c.contact_id] = false;
        }

        if (backend_) {
            if (!backend_->emit_touch_contact(c)) {
                backend_failed = true;
            }
        }
    }

    if (backend_) {
        if (!backend_->emit_sync()) {
            backend_failed = true;
        }
    }

    ++total_frames_processed_;
    last_acked_seq_ = header.sequence_number;
    ++total_acks_sent_;

    TouchAckPayload ack;
    ack.acked_sequence = header.sequence_number;
    ack.active_contact_mask = active_contact_mask_locked();
    ack.status = backend_failed ? InputProtocolError::QueueOverflow : InputProtocolError::None;

    auto pkt = encode_touch_ack_message(header.sequence_number, header.timestamp_ms, ack);
    transport_->send_bytes(pkt);
}

void GuestInputDaemon::handle_key_event(const WireHeader& header, std::span<const std::byte> payload) {
    if (state_ != DaemonState::Active) {
        return;
    }

    InputProtocolError parse_err = InputProtocolError::None;
    auto key = decode_key_event_message(payload, parse_err);
    if (!key.has_value()) {
        last_error_ = parse_err;
        KeyAckPayload ack{header.sequence_number, parse_err};
        auto pkt = encode_key_ack_message(header.sequence_number, header.timestamp_ms, ack);
        transport_->send_bytes(pkt);
        return;
    }

    auto it = std::find(held_keys_.begin(), held_keys_.end(), key->android_keycode);
    if (key->is_down) {
        if (it == held_keys_.end()) {
            held_keys_.push_back(key->android_keycode);
        }
    } else {
        if (it != held_keys_.end()) {
            held_keys_.erase(it);
        }
    }

    bool backend_ok = true;
    if (backend_) {
        backend_ok = backend_->emit_key_event(key->android_keycode, key->is_down);
    }

    ++total_keys_processed_;
    last_acked_seq_ = header.sequence_number;
    ++total_acks_sent_;

    KeyAckPayload ack;
    ack.acked_sequence = header.sequence_number;
    ack.status = backend_ok ? InputProtocolError::None : InputProtocolError::QueueOverflow;

    auto pkt = encode_key_ack_message(header.sequence_number, header.timestamp_ms, ack);
    transport_->send_bytes(pkt);
}

void GuestInputDaemon::handle_cancel_all(const WireHeader& header, std::span<const std::byte> payload) {
    InputProtocolError parse_err = InputProtocolError::None;
    auto cancel = decode_cancel_all_message(payload, parse_err);

    if (backend_) {
        backend_->cancel_all_contacts_and_keys();
    }
    active_contacts_.fill(false);
    held_keys_.clear();

    ++total_cancels_processed_;
    last_acked_seq_ = header.sequence_number;
    ++total_acks_sent_;

    CancelAllAckPayload ack;
    ack.acked_sequence = header.sequence_number;
    ack.status = InputProtocolError::None;

    auto pkt = encode_cancel_all_ack_message(header.sequence_number, header.timestamp_ms, ack);
    transport_->send_bytes(pkt);
}

void GuestInputDaemon::handle_ping(const WireHeader& header, std::span<const std::byte> payload) {
    InputProtocolError parse_err = InputProtocolError::None;
    auto ping = decode_ping_message(payload, parse_err);
    if (ping.has_value()) {
        PongPayload pong{ping->cookie};
        auto pkt = encode_pong_message(header.sequence_number, header.timestamp_ms, pong);
        transport_->send_bytes(pkt);
    }
}

void GuestInputDaemon::handle_disconnect(const WireHeader& /*header*/, std::span<const std::byte> /*payload*/) {
    if (backend_) {
        backend_->cancel_all_contacts_and_keys();
    }
    active_contacts_.fill(false);
    held_keys_.clear();
    state_ = DaemonState::Disconnected;
}

void GuestInputDaemon::send_error(uint64_t seq, InputProtocolError error, std::string message) {
    if (!transport_ || !transport_->is_connected()) {
        return;
    }
    ErrorPayload err_p{error, std::move(message)};
    auto pkt = encode_error_message(seq, 0, err_p);
    transport_->send_bytes(pkt);
}

uint64_t GuestInputDaemon::total_frames_processed() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return total_frames_processed_;
}

uint64_t GuestInputDaemon::total_keys_processed() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return total_keys_processed_;
}

uint64_t GuestInputDaemon::total_cancels_processed() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return total_cancels_processed_;
}

uint64_t GuestInputDaemon::total_acks_sent() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return total_acks_sent_;
}

uint64_t GuestInputDaemon::last_received_sequence() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return last_received_seq_;
}

uint64_t GuestInputDaemon::last_acked_sequence() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return last_acked_seq_;
}

uint64_t GuestInputDaemon::duplicate_seq_count() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return duplicate_seq_count_;
}

uint64_t GuestInputDaemon::gap_seq_count() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return gap_seq_count_;
}

uint32_t GuestInputDaemon::active_contact_count_locked() const noexcept {
    uint32_t count = 0;
    for (bool active : active_contacts_) {
        if (active) ++count;
    }
    return count;
}

uint32_t GuestInputDaemon::active_contact_mask_locked() const noexcept {
    uint32_t mask = 0;
    for (std::size_t i = 0; i < active_contacts_.size(); ++i) {
        if (active_contacts_[i]) {
            mask |= (1u << static_cast<uint32_t>(i));
        }
    }
    return mask;
}

bool GuestInputDaemon::is_contact_active_locked(uint32_t contact_id) const noexcept {
    if (contact_id < active_contacts_.size()) {
        return active_contacts_[contact_id];
    }
    return false;
}

uint32_t GuestInputDaemon::active_contact_count() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return active_contact_count_locked();
}

uint32_t GuestInputDaemon::active_contact_mask() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return active_contact_mask_locked();
}

bool GuestInputDaemon::is_contact_active(uint32_t contact_id) const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return is_contact_active_locked(contact_id);
}

std::size_t GuestInputDaemon::held_key_count() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return held_keys_.size();
}

uint32_t GuestInputDaemon::negotiated_max_contacts() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return negotiated_max_contacts_;
}

InputProtocolError GuestInputDaemon::last_error() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return last_error_;
}

std::string GuestInputDaemon::status_diagnostic() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::ostringstream ss;
    switch (state_) {
    case DaemonState::Uninitialized: ss << "[Uninitialized] "; break;
    case DaemonState::AwaitingHello: ss << "[AwaitingHello] "; break;
    case DaemonState::Active:        ss << "[Active] "; break;
    case DaemonState::Disconnected:  ss << "[Disconnected] "; break;
    case DaemonState::Error:         ss << "[Error: " << input_protocol_error_name(last_error_) << "] "; break;
    }
    if (!diagnostic_msg_.empty()) {
        ss << diagnostic_msg_ << " ";
    }
    ss << "(Frames: " << total_frames_processed_
       << ", Keys: " << total_keys_processed_
       << ", Cancels: " << total_cancels_processed_
       << ", Acks: " << total_acks_sent_
       << ", Dups: " << duplicate_seq_count_
       << ", Gaps: " << gap_seq_count_ << ")";
    return ss.str();
}

void GuestInputDaemon::set_expected_generation(std::string gen_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    config_.expected_generation_id = std::move(gen_id);
}

void GuestInputDaemon::set_expected_session(std::string sess_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    config_.expected_session_id = std::move(sess_id);
}

} // namespace fang::guest
