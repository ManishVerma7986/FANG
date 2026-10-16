#include "broker_protocol.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace fang::broker {
namespace {

constexpr std::array<std::byte, 4> magic{
    std::byte{'F'}, std::byte{'A'}, std::byte{'N'}, std::byte{'G'}
};
constexpr std::uint16_t reply_type = 0x8000;
constexpr std::size_t reply_fixed_payload_size = sizeof(std::uint16_t) + sizeof(std::uint8_t) +
    sizeof(std::uint32_t);

void write_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value & 0xff);
    output[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
}

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
}

void write_u64(std::span<std::byte> output, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
}

std::uint16_t read_u16(std::span<const std::byte> input, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1]) << 8);
}

std::uint32_t read_u32(std::span<const std::byte> input, std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index])) <<
            (index * 8);
    }
    return value;
}

std::uint64_t read_u64(std::span<const std::byte> input, std::size_t offset) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index])) <<
            (index * 8);
    }
    return value;
}

std::array<std::byte, wire_header_size> encode_header(std::uint16_t type,
    std::uint64_t request_id, std::uint32_t payload_size) noexcept {
    std::array<std::byte, wire_header_size> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    write_u16(header, 4, protocol_version);
    write_u16(header, 6, type);
    write_u64(header, 8, request_id);
    write_u32(header, 16, payload_size);
    return header;
}

bool valid_lifecycle_state(std::uint8_t state) noexcept {
    return state <= static_cast<std::uint8_t>(LifecycleState::Failed);
}

bool valid_broker_error(std::uint16_t error) noexcept {
    return error <= static_cast<std::uint16_t>(BrokerErrorCode::StoreNotInitialized);
}

bool valid_request_type(std::uint16_t type) noexcept {
    return type >= static_cast<std::uint16_t>(RequestType::Ping) &&
        type <= static_cast<std::uint16_t>(RequestType::ShutdownOwner);
}

}

std::array<std::byte, wire_header_size> encode_request_header(const Request& request) noexcept {
    return encode_header(static_cast<std::uint16_t>(request.type), request.request_id,
        static_cast<std::uint32_t>(request.payload.size()));
}

std::vector<std::byte> encode_request(const Request& request) {
    if (request.payload.size() > maximum_payload_size) {
        throw std::length_error("Broker request payload exceeds the protocol limit.");
    }
    const auto header = encode_request_header(request);
    std::vector<std::byte> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), request.payload.begin(), request.payload.end());
    return bytes;
}

RequestHeader inspect_request_header(std::span<const std::byte> bytes) noexcept {
    RequestHeader result;
    if (bytes.size() < wire_header_size) {
        result.error = ProtocolError::Truncated;
        return result;
    }
    result.version = read_u16(bytes, 4);
    result.type = read_u16(bytes, 6);
    result.request_id = read_u64(bytes, 8);
    result.payload_size = read_u32(bytes, 16);
    result.has_request_id = result.request_id != 0;
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        result.error = ProtocolError::InvalidMagic;
    } else if (result.version != protocol_version) {
        result.error = ProtocolError::UnsupportedVersion;
    } else if (!result.has_request_id) {
        result.error = ProtocolError::InvalidRequestId;
    } else if (result.payload_size > maximum_payload_size) {
        result.error = ProtocolError::Oversized;
    } else if (!valid_request_type(result.type)) {
        result.error = ProtocolError::UnknownType;
    }
    return result;
}

RequestParseResult decode_request(std::span<const std::byte> bytes) noexcept {
    RequestParseResult result;
    const auto header = inspect_request_header(bytes);
    result.request_id = header.request_id;
    result.has_request_id = header.has_request_id;
    if (header.error != ProtocolError::None) {
        result.error = header.error;
        return result;
    }
    if (bytes.size() < wire_header_size + header.payload_size) {
        result.error = ProtocolError::Truncated;
        return result;
    }
    if (bytes.size() != wire_header_size + header.payload_size) {
        result.error = ProtocolError::UnexpectedPayload;
        return result;
    }
    if ((header.type == static_cast<std::uint16_t>(RequestType::Ping) ||
         header.type == static_cast<std::uint16_t>(RequestType::GetStatus)) &&
        header.payload_size != 0) {
        result.error = ProtocolError::UnexpectedPayload;
        return result;
    }
    Request request{static_cast<RequestType>(header.type), header.request_id};
    const auto payload_begin = bytes.begin() + static_cast<std::ptrdiff_t>(wire_header_size);
    request.payload.assign(payload_begin,
        payload_begin + static_cast<std::ptrdiff_t>(header.payload_size));
    result.request = std::move(request);
    return result;
}

std::vector<std::byte> encode_reply(std::uint64_t request_id, const BrokerReply& reply) {
    if (request_id == 0) {
        throw std::invalid_argument("A broker reply requires a nonzero request identifier.");
    }
    if (reply.message.size() > maximum_payload_size - reply_fixed_payload_size) {
        throw std::length_error("Broker reply text exceeds the protocol limit.");
    }

    const auto payload_size = reply_fixed_payload_size + reply.message.size();
    const auto header = encode_header(reply_type, request_id, static_cast<std::uint32_t>(payload_size));
    std::vector<std::byte> bytes(header.begin(), header.end());
    bytes.resize(wire_header_size + payload_size);
    auto output = std::span<std::byte>(bytes);
    write_u16(output, wire_header_size, static_cast<std::uint16_t>(reply.error));
    output[wire_header_size + 2] = static_cast<std::byte>(reply.state);
    write_u32(output, wire_header_size + 3, static_cast<std::uint32_t>(reply.message.size()));
    std::transform(reply.message.begin(), reply.message.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(wire_header_size + reply_fixed_payload_size),
        [](char value) { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
    return bytes;
}

ReplyParseResult decode_reply(std::span<const std::byte> bytes) {
    ReplyParseResult result;
    if (bytes.size() < wire_header_size) {
        result.error = ProtocolError::Truncated;
        return result;
    }
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        result.error = ProtocolError::InvalidMagic;
        return result;
    }
    const auto version = read_u16(bytes, 4);
    const auto type = read_u16(bytes, 6);
    const auto request_id = read_u64(bytes, 8);
    const auto payload_size = read_u32(bytes, 16);
    result.request_id = request_id;
    result.has_request_id = request_id != 0;
    if (version != protocol_version) {
        result.error = ProtocolError::UnsupportedVersion;
        return result;
    }
    if (!result.has_request_id || type != reply_type || payload_size > maximum_payload_size ||
        payload_size < reply_fixed_payload_size ||
        bytes.size() != wire_header_size + payload_size) {
        result.error = ProtocolError::MalformedReply;
        return result;
    }
    const auto error_code = read_u16(bytes, wire_header_size);
    const auto state = std::to_integer<std::uint8_t>(bytes[wire_header_size + 2]);
    const auto message_size = read_u32(bytes, wire_header_size + 3);
    if (!valid_broker_error(error_code) || !valid_lifecycle_state(state) ||
        message_size != payload_size - reply_fixed_payload_size) {
        result.error = ProtocolError::MalformedReply;
        return result;
    }
    BrokerReply reply;
    reply.error = static_cast<BrokerErrorCode>(error_code);
    reply.state = static_cast<LifecycleState>(state);
    const auto begin = bytes.begin() +
        static_cast<std::ptrdiff_t>(wire_header_size + reply_fixed_payload_size);
    reply.message.reserve(message_size);
    std::transform(begin, begin + static_cast<std::ptrdiff_t>(message_size),
        std::back_inserter(reply.message),
        [](std::byte value) { return static_cast<char>(std::to_integer<unsigned char>(value)); });
    result.reply = std::move(reply);
    return result;
}

BrokerErrorCode broker_error_for(ProtocolError error) noexcept {
    switch (error) {
    case ProtocolError::None: return BrokerErrorCode::None;
    case ProtocolError::UnsupportedVersion: return BrokerErrorCode::UnsupportedVersion;
    case ProtocolError::Oversized: return BrokerErrorCode::OversizedMessage;
    case ProtocolError::UnknownType: return BrokerErrorCode::UnknownMessageType;
    case ProtocolError::Truncated:
    case ProtocolError::InvalidMagic:
    case ProtocolError::InvalidRequestId:
    case ProtocolError::UnexpectedPayload:
    case ProtocolError::MalformedReply:
        return BrokerErrorCode::InvalidMessage;
    }
    return BrokerErrorCode::InvalidMessage;
}

RequestIdWindow::RequestIdWindow(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("The request identifier history capacity must be positive.");
    }
}

bool RequestIdWindow::observe(std::uint64_t request_id) {
    if (request_id == 0) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!ids_.insert(request_id).second) {
        return false;
    }
    order_.push_back(request_id);
    if (order_.size() > capacity_) {
        ids_.erase(order_.front());
        order_.pop_front();
    }
    return true;
}

std::size_t RequestIdWindow::size() const {
    std::lock_guard lock(mutex_);
    return order_.size();
}

}
