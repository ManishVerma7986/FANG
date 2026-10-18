#pragma once

#include "input_mapping_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fang::studio {

inline constexpr uint32_t input_protocol_magic = 0x46495043; // "FIPC" (Fang Input Protocol Communication)
inline constexpr uint16_t input_protocol_version = 1;
inline constexpr std::size_t input_wire_header_size = 32;
inline constexpr std::size_t input_maximum_payload_size = 4096;
inline constexpr std::size_t input_maximum_wire_size = input_wire_header_size + input_maximum_payload_size;
inline constexpr uint32_t input_max_simultaneous_contacts = 10;
inline constexpr std::size_t input_max_string_length = 64;
inline constexpr std::size_t input_max_diagnostic_length = 128;

enum class InputMessageType : uint16_t {
    Hello = 1,
    HelloAck = 2,
    TouchFrame = 3,
    TouchAck = 4,
    KeyEvent = 5,
    KeyAck = 6,
    CancelAll = 7,
    CancelAllAck = 8,
    Ping = 9,
    Pong = 10,
    Disconnect = 11,
    Error = 12
};

inline constexpr std::string_view input_message_type_name(InputMessageType type) noexcept {
    switch (type) {
    case InputMessageType::Hello:        return "Hello";
    case InputMessageType::HelloAck:     return "HelloAck";
    case InputMessageType::TouchFrame:   return "TouchFrame";
    case InputMessageType::TouchAck:     return "TouchAck";
    case InputMessageType::KeyEvent:     return "KeyEvent";
    case InputMessageType::KeyAck:       return "KeyAck";
    case InputMessageType::CancelAll:    return "CancelAll";
    case InputMessageType::CancelAllAck: return "CancelAllAck";
    case InputMessageType::Ping:         return "Ping";
    case InputMessageType::Pong:         return "Pong";
    case InputMessageType::Disconnect:   return "Disconnect";
    case InputMessageType::Error:        return "Error";
    default:                             return "Unknown";
    }
}

enum class InputProtocolError : uint32_t {
    None = 0,
    InvalidMagic = 1,
    UnsupportedVersion = 2,
    TruncatedHeader = 3,
    TruncatedPayload = 4,
    OversizedPayload = 5,
    MalformedPayload = 6,
    StaleGeneration = 7,
    SessionMismatch = 8,
    InvalidContactId = 9,
    InvalidTransition = 10,
    InvalidCoordinates = 11,
    ExceededContactLimit = 12,
    QueueOverflow = 13,
    ConnectionClosed = 14,
    UnknownMessageType = 15
};

inline constexpr std::string_view input_protocol_error_name(InputProtocolError err) noexcept {
    switch (err) {
    case InputProtocolError::None:                 return "None";
    case InputProtocolError::InvalidMagic:          return "InvalidMagic";
    case InputProtocolError::UnsupportedVersion:    return "UnsupportedVersion";
    case InputProtocolError::TruncatedHeader:       return "TruncatedHeader";
    case InputProtocolError::TruncatedPayload:      return "TruncatedPayload";
    case InputProtocolError::OversizedPayload:      return "OversizedPayload";
    case InputProtocolError::MalformedPayload:      return "MalformedPayload";
    case InputProtocolError::StaleGeneration:       return "StaleGeneration";
    case InputProtocolError::SessionMismatch:       return "SessionMismatch";
    case InputProtocolError::InvalidContactId:      return "InvalidContactId";
    case InputProtocolError::InvalidTransition:     return "InvalidTransition";
    case InputProtocolError::InvalidCoordinates:    return "InvalidCoordinates";
    case InputProtocolError::ExceededContactLimit:  return "ExceededContactLimit";
    case InputProtocolError::QueueOverflow:         return "QueueOverflow";
    case InputProtocolError::ConnectionClosed:      return "ConnectionClosed";
    case InputProtocolError::UnknownMessageType:    return "UnknownMessageType";
    default:                                        return "Unknown";
    }
}

struct WireHeader {
    uint32_t magic{input_protocol_magic};
    uint16_t version{input_protocol_version};
    InputMessageType type{InputMessageType::Hello};
    uint64_t sequence_number{0};
    uint64_t timestamp_ms{0};
    uint32_t payload_size{0};
    uint32_t status_or_flags{0};
};

struct HelloPayload {
    uint16_t client_version{input_protocol_version};
    uint32_t display_width{1080};
    uint32_t display_height{1920};
    std::string session_id;
    std::string generation_id;
};

struct HelloAckPayload {
    uint16_t guest_version{input_protocol_version};
    InputProtocolError status{InputProtocolError::None};
    uint32_t max_contacts{input_max_simultaneous_contacts};
    uint32_t supported_caps{0x07}; // Touch=1, Key=2, PointerAim=4
    std::string session_id;
    std::string generation_id;
};

#pragma pack(push, 1)
struct WireTouchContact {
    uint32_t contact_id{0};
    uint8_t transition{0}; // 0 = Down, 1 = Move, 2 = Up, 3 = Cancel
    uint8_t reserved[3]{0, 0, 0};
    float norm_u{0.0f};
    float norm_v{0.0f};
    int32_t guest_x{0};
    int32_t guest_y{0};
};
#pragma pack(pop)

struct TouchFramePayload {
    std::vector<WireTouchContact> contacts;
};

struct TouchAckPayload {
    uint64_t acked_sequence{0};
    uint32_t active_contact_mask{0};
    InputProtocolError status{InputProtocolError::None};
};

struct KeyEventPayload {
    int32_t android_keycode{0};
    bool is_down{false};
};

struct KeyAckPayload {
    uint64_t acked_sequence{0};
    InputProtocolError status{InputProtocolError::None};
};

struct CancelAllPayload {
    uint32_t reason_code{0};
};

struct CancelAllAckPayload {
    uint64_t acked_sequence{0};
    InputProtocolError status{InputProtocolError::None};
};

struct PingPayload {
    uint64_t cookie{0};
};

struct PongPayload {
    uint64_t cookie{0};
};

struct ErrorPayload {
    InputProtocolError error_code{InputProtocolError::None};
    std::string message;
};

// Wire encoding & decoding functions
std::array<std::byte, input_wire_header_size> encode_wire_header(const WireHeader& header) noexcept;
InputProtocolError decode_wire_header(std::span<const std::byte> bytes, WireHeader& out_header) noexcept;

std::vector<std::byte> encode_hello_message(uint64_t seq, uint64_t ts, const HelloPayload& payload);
std::optional<HelloPayload> decode_hello_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_hello_ack_message(uint64_t seq, uint64_t ts, const HelloAckPayload& payload);
std::optional<HelloAckPayload> decode_hello_ack_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_touch_frame_message(uint64_t seq, uint64_t ts, const TouchFramePayload& payload);
std::optional<TouchFramePayload> decode_touch_frame_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_touch_ack_message(uint64_t seq, uint64_t ts, const TouchAckPayload& payload);
std::optional<TouchAckPayload> decode_touch_ack_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_key_event_message(uint64_t seq, uint64_t ts, const KeyEventPayload& payload);
std::optional<KeyEventPayload> decode_key_event_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_key_ack_message(uint64_t seq, uint64_t ts, const KeyAckPayload& payload);
std::optional<KeyAckPayload> decode_key_ack_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_cancel_all_message(uint64_t seq, uint64_t ts, const CancelAllPayload& payload);
std::optional<CancelAllPayload> decode_cancel_all_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_cancel_all_ack_message(uint64_t seq, uint64_t ts, const CancelAllAckPayload& payload);
std::optional<CancelAllAckPayload> decode_cancel_all_ack_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_ping_message(uint64_t seq, uint64_t ts, const PingPayload& payload);
std::optional<PingPayload> decode_ping_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_pong_message(uint64_t seq, uint64_t ts, const PongPayload& payload);
std::optional<PongPayload> decode_pong_message(std::span<const std::byte> payload, InputProtocolError& out_error);

std::vector<std::byte> encode_error_message(uint64_t seq, uint64_t ts, const ErrorPayload& payload);
std::optional<ErrorPayload> decode_error_message(std::span<const std::byte> payload, InputProtocolError& out_error);

} // namespace fang::studio
