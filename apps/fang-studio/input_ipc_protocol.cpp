#include "input_ipc_protocol.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace fang::studio {

namespace {

// htonll/ntohll for 64-bit network byte order conversion
static inline uint64_t htonll_impl(uint64_t val) {
    static const uint16_t test = 0x0102;
    if (*reinterpret_cast<const uint8_t*>(&test) == 0x01) {
        // Big-endian host - no conversion needed
        return val;
    }
    // Little-endian host - swap bytes
    return ((uint64_t)htonl(static_cast<uint32_t>(val)) << 32) | htonl(static_cast<uint32_t>(val >> 32));
}

static inline uint64_t ntohll_impl(uint64_t val) {
    return htonll_impl(val); // Symmetric operation
}

template <typename T>
void write_val(std::vector<std::byte>& buf, const T& val) {
    const auto* src = reinterpret_cast<const std::byte*>(&val);
    buf.insert(buf.end(), src, src + sizeof(T));
}

template <typename T>
bool read_val(std::span<const std::byte>& span, T& val) noexcept {
    if (span.size() < sizeof(T)) {
        return false;
    }
    std::memcpy(&val, span.data(), sizeof(T));
    span = span.subspan(sizeof(T));
    return true;
}

void write_string(std::vector<std::byte>& buf, std::string_view str, std::size_t max_len) {
    const uint32_t len = static_cast<uint32_t>((std::min)(str.size(), max_len));
    write_val(buf, len);
    for (size_t i = 0; i < len; ++i) {
        buf.push_back(static_cast<std::byte>(str[i]));
    }
}

bool read_string(std::span<const std::byte>& span, std::string& out_str, std::size_t max_len) noexcept {
    uint32_t len = 0;
    if (!read_val(span, len)) {
        return false;
    }
    if (len > max_len || span.size() < len) {
        return false;
    }
    out_str.assign(reinterpret_cast<const char*>(span.data()), len);
    span = span.subspan(len);
    return true;
}

std::vector<std::byte> build_wire_packet(
    InputMessageType type, uint64_t seq, uint64_t ts, uint32_t flags,
    const std::vector<std::byte>& payload) {
    WireHeader header;
    header.magic = input_protocol_magic;
    header.version = input_protocol_version;
    header.type = type;
    header.sequence_number = seq;
    header.timestamp_ms = ts;
    header.payload_size = static_cast<uint32_t>(payload.size());
    header.status_or_flags = flags;

    const auto header_bytes = encode_wire_header(header);
    std::vector<std::byte> packet;
    packet.reserve(input_wire_header_size + payload.size());
    packet.insert(packet.end(), header_bytes.begin(), header_bytes.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

} // namespace
std::array<std::byte, input_wire_header_size> encode_wire_header(const WireHeader& header) noexcept {
      std::array<std::byte, input_wire_header_size> bytes{};
      auto* dst = bytes.data();

      // Write in network byte order (big-endian)
      uint32_t magic_be = htonl(header.magic);
      std::memcpy(dst + 0, &magic_be, 4);

      uint16_t version_be = htons(header.version);
      std::memcpy(dst + 4, &version_be, 2);

      uint16_t type_be = htons(static_cast<uint16_t>(header.type));
      std::memcpy(dst + 6, &type_be, 2);

      uint64_t seq_be = htonll_impl(header.sequence_number);
      std::memcpy(dst + 8, &seq_be, 8);

      uint64_t timestamp_be = htonll_impl(header.timestamp_ms);
      std::memcpy(dst + 16, &timestamp_be, 8);

      uint32_t payload_size_be = htonl(header.payload_size);
      std::memcpy(dst + 24, &payload_size_be, 4);

      uint32_t status_be = htonl(header.status_or_flags);
      std::memcpy(dst + 28, &status_be, 4);

      return bytes;
  }

  InputProtocolError decode_wire_header(std::span<const std::byte> bytes, WireHeader& out_header) noexcept {
      if (bytes.size() < input_wire_header_size) {
          return InputProtocolError::TruncatedHeader;
      }

      const auto* src = bytes.data();

      uint32_t magic_be;
      std::memcpy(&magic_be, src + 0, 4);
      out_header.magic = ntohl(magic_be);
      if (out_header.magic != input_protocol_magic) {
          return InputProtocolError::InvalidMagic;
      }

      uint16_t version_be;
      std::memcpy(&version_be, src + 4, 2);
      out_header.version = ntohs(version_be);
      if (out_header.version != input_protocol_version) {
          return InputProtocolError::UnsupportedVersion;
      }

      uint16_t raw_type_be;
      std::memcpy(&raw_type_be, src + 6, 2);
      uint16_t raw_type = ntohs(raw_type_be);
      if (raw_type < static_cast<uint16_t>(InputMessageType::Hello) ||
          raw_type > static_cast<uint16_t>(InputMessageType::Error)) {
          return InputProtocolError::UnknownMessageType;
      }
      out_header.type = static_cast<InputMessageType>(raw_type);

      uint64_t seq_be;
      std::memcpy(&seq_be, src + 8, 8);
      out_header.sequence_number = ntohll_impl(seq_be);

      uint64_t timestamp_be;
      std::memcpy(&timestamp_be, src + 16, 8);
      out_header.timestamp_ms = ntohll_impl(timestamp_be);

      uint32_t payload_size_be;
      std::memcpy(&payload_size_be, src + 24, 4);
      out_header.payload_size = ntohl(payload_size_be);
      if (out_header.payload_size > input_maximum_payload_size) {
          return InputProtocolError::OversizedPayload;
      }

      uint32_t status_be;
      std::memcpy(&status_be, src + 28, 4);
      out_header.status_or_flags = ntohl(status_be);

      return InputProtocolError::None;
  }

std::vector<std::byte> encode_hello_message(uint64_t seq, uint64_t ts, const HelloPayload& payload) {
    std::vector<std::byte> p;
    write_val(p, payload.client_version);
    write_val(p, payload.display_width);
    write_val(p, payload.display_height);
    write_string(p, payload.session_id, input_max_string_length);
    write_string(p, payload.generation_id, input_max_string_length);
    return build_wire_packet(InputMessageType::Hello, seq, ts, 0, p);
}

std::optional<HelloPayload> decode_hello_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
      auto span = payload;
      HelloPayload out;
      if (!read_val(span, out.client_version) ||
          !read_val(span, out.display_width) ||
          !read_val(span, out.display_height)) {
          out_error = InputProtocolError::TruncatedPayload;
          return std::nullopt;
      }

      // DEBUG
      std::cerr << "[DEBUG decode_hello] client_version=" << out.client_version 
                << " input_protocol_version=" << input_protocol_version
                << " display_width=" << out.display_width
                << " display_height=" << out.display_height << std::endl;

      if (out.client_version != input_protocol_version) {
          out_error = InputProtocolError::UnsupportedVersion;
          return std::nullopt;
      }

    if (out.display_width == 0 || out.display_width > 8192 ||
        out.display_height == 0 || out.display_height > 8192) {
        out_error = InputProtocolError::InvalidCoordinates;
        return std::nullopt;
    }

    if (!read_string(span, out.session_id, input_max_string_length) ||
        !read_string(span, out.generation_id, input_max_string_length)) {
        out_error = InputProtocolError::MalformedPayload;
        return std::nullopt;
    }

    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_hello_ack_message(uint64_t seq, uint64_t ts, const HelloAckPayload& payload) {
    std::vector<std::byte> p;
    write_val(p, payload.guest_version);
    const uint32_t status = static_cast<uint32_t>(payload.status);
    write_val(p, status);
    write_val(p, payload.max_contacts);
    write_val(p, payload.supported_caps);
    write_string(p, payload.session_id, input_max_string_length);
    write_string(p, payload.generation_id, input_max_string_length);
    return build_wire_packet(InputMessageType::HelloAck, seq, ts, status, p);
}

std::optional<HelloAckPayload> decode_hello_ack_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    HelloAckPayload out;
    uint32_t status_raw = 0;
    if (!read_val(span, out.guest_version) ||
        !read_val(span, status_raw) ||
        !read_val(span, out.max_contacts) ||
        !read_val(span, out.supported_caps)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }
    out.status = static_cast<InputProtocolError>(status_raw);

    if (out.guest_version != input_protocol_version) {
        out_error = InputProtocolError::UnsupportedVersion;
        return std::nullopt;
    }

    if (out.max_contacts > input_max_simultaneous_contacts) {
        out_error = InputProtocolError::ExceededContactLimit;
        return std::nullopt;
    }

    if (!read_string(span, out.session_id, input_max_string_length) ||
        !read_string(span, out.generation_id, input_max_string_length)) {
        out_error = InputProtocolError::MalformedPayload;
        return std::nullopt;
    }

    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_touch_frame_message(uint64_t seq, uint64_t ts, const TouchFramePayload& payload) {
    std::vector<std::byte> p;
    const uint32_t count = static_cast<uint32_t>(payload.contacts.size());
    write_val(p, count);
    for (const auto& contact : payload.contacts) {
        write_val(p, contact);
    }
    return build_wire_packet(InputMessageType::TouchFrame, seq, ts, 0, p);
}

std::optional<TouchFramePayload> decode_touch_frame_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    uint32_t count = 0;
    if (!read_val(span, count)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }

    if (count > input_max_simultaneous_contacts) {
        out_error = InputProtocolError::ExceededContactLimit;
        return std::nullopt;
    }

    if (span.size() != count * sizeof(WireTouchContact)) {
        out_error = InputProtocolError::MalformedPayload;
        return std::nullopt;
    }

    TouchFramePayload out;
    out.contacts.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!read_val(span, out.contacts[i])) {
            out_error = InputProtocolError::TruncatedPayload;
            return std::nullopt;
        }

        const auto& c = out.contacts[i];
        if (c.contact_id >= input_max_simultaneous_contacts) {
            out_error = InputProtocolError::InvalidContactId;
            return std::nullopt;
        }

        if (c.transition > 3) {
            out_error = InputProtocolError::InvalidTransition;
            return std::nullopt;
        }

        if (std::isnan(c.norm_u) || std::isnan(c.norm_v) ||
            c.norm_u < -0.01f || c.norm_u > 1.01f ||
            c.norm_v < -0.01f || c.norm_v > 1.01f) {
            out_error = InputProtocolError::InvalidCoordinates;
            return std::nullopt;
        }
    }

    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_touch_ack_message(uint64_t seq, uint64_t ts, const TouchAckPayload& payload) {
    std::vector<std::byte> p;
    write_val(p, payload.acked_sequence);
    write_val(p, payload.active_contact_mask);
    const uint32_t status = static_cast<uint32_t>(payload.status);
    write_val(p, status);
    return build_wire_packet(InputMessageType::TouchAck, seq, ts, status, p);
}

std::optional<TouchAckPayload> decode_touch_ack_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    TouchAckPayload out;
    uint32_t status_raw = 0;
    if (!read_val(span, out.acked_sequence) ||
        !read_val(span, out.active_contact_mask) ||
        !read_val(span, status_raw)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }
    out.status = static_cast<InputProtocolError>(status_raw);
    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_key_event_message(uint64_t seq, uint64_t ts, const KeyEventPayload& payload) {
    std::vector<std::byte> p;
    write_val(p, payload.android_keycode);
    const uint8_t down = payload.is_down ? 1 : 0;
    write_val(p, down);
    const uint8_t pad[3] = {0, 0, 0};
    p.insert(p.end(), reinterpret_cast<const std::byte*>(pad), reinterpret_cast<const std::byte*>(pad) + 3);
    return build_wire_packet(InputMessageType::KeyEvent, seq, ts, 0, p);
}

std::optional<KeyEventPayload> decode_key_event_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    KeyEventPayload out;
    uint8_t down = 0;
    if (!read_val(span, out.android_keycode) || !read_val(span, down)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }
    out.is_down = (down != 0);
    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_key_ack_message(uint64_t seq, uint64_t ts, const KeyAckPayload& payload) {
    std::vector<std::byte> p;
    write_val(p, payload.acked_sequence);
    const uint32_t status = static_cast<uint32_t>(payload.status);
    write_val(p, status);
    return build_wire_packet(InputMessageType::KeyAck, seq, ts, status, p);
}

std::optional<KeyAckPayload> decode_key_ack_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    KeyAckPayload out;
    uint32_t status_raw = 0;
    if (!read_val(span, out.acked_sequence) || !read_val(span, status_raw)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }
    out.status = static_cast<InputProtocolError>(status_raw);
    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_cancel_all_message(uint64_t seq, uint64_t ts, const CancelAllPayload& payload) {
    std::vector<std::byte> p;
    write_val(p, payload.reason_code);
    return build_wire_packet(InputMessageType::CancelAll, seq, ts, 0, p);
}

std::optional<CancelAllPayload> decode_cancel_all_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    CancelAllPayload out;
    if (!read_val(span, out.reason_code)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }
    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_cancel_all_ack_message(uint64_t seq, uint64_t ts, const CancelAllAckPayload& payload) {
    std::vector<std::byte> p;
    write_val(p, payload.acked_sequence);
    const uint32_t status = static_cast<uint32_t>(payload.status);
    write_val(p, status);
    return build_wire_packet(InputMessageType::CancelAllAck, seq, ts, status, p);
}

std::optional<CancelAllAckPayload> decode_cancel_all_ack_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    CancelAllAckPayload out;
    uint32_t status_raw = 0;
    if (!read_val(span, out.acked_sequence) || !read_val(span, status_raw)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }
    out.status = static_cast<InputProtocolError>(status_raw);
    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_ping_message(uint64_t seq, uint64_t ts, const PingPayload& payload) {
    std::vector<std::byte> p;
    write_val(p, payload.cookie);
    return build_wire_packet(InputMessageType::Ping, seq, ts, 0, p);
}

std::optional<PingPayload> decode_ping_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    PingPayload out;
    if (!read_val(span, out.cookie)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }
    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_pong_message(uint64_t seq, uint64_t ts, const PongPayload& payload) {
    std::vector<std::byte> p;
    write_val(p, payload.cookie);
    return build_wire_packet(InputMessageType::Pong, seq, ts, 0, p);
}

std::optional<PongPayload> decode_pong_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    PongPayload out;
    if (!read_val(span, out.cookie)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }
    out_error = InputProtocolError::None;
    return out;
}

std::vector<std::byte> encode_error_message(uint64_t seq, uint64_t ts, const ErrorPayload& payload) {
    std::vector<std::byte> p;
    const uint32_t err = static_cast<uint32_t>(payload.error_code);
    write_val(p, err);
    write_string(p, payload.message, input_max_diagnostic_length);
    return build_wire_packet(InputMessageType::Error, seq, ts, err, p);
}

std::optional<ErrorPayload> decode_error_message(std::span<const std::byte> payload, InputProtocolError& out_error) {
    auto span = payload;
    ErrorPayload out;
    uint32_t raw_err = 0;
    if (!read_val(span, raw_err)) {
        out_error = InputProtocolError::TruncatedPayload;
        return std::nullopt;
    }
    out.error_code = static_cast<InputProtocolError>(raw_err);
    if (!read_string(span, out.message, input_max_diagnostic_length)) {
        out_error = InputProtocolError::MalformedPayload;
        return std::nullopt;
    }
    out_error = InputProtocolError::None;
    return out;
}

} // namespace fang::studio
