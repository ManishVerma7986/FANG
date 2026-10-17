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
    sizeof(std::uint32_t) * 2;
constexpr std::size_t maximum_runtime_text_size = 1024;
constexpr std::size_t maximum_avd_name_size = 128;

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
    return error <= static_cast<std::uint16_t>(BrokerErrorCode::RuntimeShutdownFailed);
}

bool valid_request_type(std::uint16_t type) noexcept {
    return type >= static_cast<std::uint16_t>(RequestType::Ping) &&
        type <= static_cast<std::uint16_t>(RequestType::ShutdownRuntime);
}

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xff));
    output.push_back(static_cast<std::byte>((value >> 8) & 0xff));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xff));
    }
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xff));
    }
}

bool take_u16(std::span<const std::byte> input, std::size_t& offset,
    std::uint16_t& value) noexcept {
    if (offset > input.size() || input.size() - offset < sizeof(value)) return false;
    value = read_u16(input, offset);
    offset += sizeof(value);
    return true;
}

bool take_u32(std::span<const std::byte> input, std::size_t& offset,
    std::uint32_t& value) noexcept {
    if (offset > input.size() || input.size() - offset < sizeof(value)) return false;
    value = read_u32(input, offset);
    offset += sizeof(value);
    return true;
}

bool take_u64(std::span<const std::byte> input, std::size_t& offset,
    std::uint64_t& value) noexcept {
    if (offset > input.size() || input.size() - offset < sizeof(value)) return false;
    value = read_u64(input, offset);
    offset += sizeof(value);
    return true;
}

void append_text(std::vector<std::byte>& output, std::string_view value) {
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char character : value) output.push_back(static_cast<std::byte>(character));
}

bool take_text(std::span<const std::byte> input, std::size_t& offset,
    std::size_t maximum_size, std::string& value) {
    std::uint32_t size{};
    if (!take_u32(input, offset, size) || size > maximum_size ||
        offset > input.size() || input.size() - offset < size) return false;
    value.clear();
    value.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index) {
        value.push_back(static_cast<char>(std::to_integer<unsigned char>(input[offset + index])));
    }
    offset += size;
    return value.find('\0') == std::string::npos;
}

bool valid_avd_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > maximum_avd_name_size) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '_' || value == '-';
    });
}

bool valid_generation_id(std::string_view generation) noexcept {
    if (generation.size() != 36) return false;
    for (std::size_t index = 0; index < generation.size(); ++index) {
        const char value = generation[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-') return false;
        } else if (!((value >= '0' && value <= '9') ||
                (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool valid_runtime_state(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(RuntimeLifecycleState::Failed);
}

bool valid_failure_stage(std::uint16_t value) noexcept {
    return value <= static_cast<std::uint16_t>(RuntimeFailureStage::Unknown);
}

}

std::vector<std::byte> encode_start_reference_runtime_request(
        const StartReferenceRuntimeRequest& request) {
    if (request.configuration_version != reference_runtime_configuration_version ||
        !valid_avd_name(request.avd_name)) {
        throw std::invalid_argument("The reference runtime request is invalid.");
    }
    std::vector<std::byte> payload;
    append_u16(payload, request.configuration_version);
    append_text(payload, request.avd_name);
    if (payload.size() > maximum_payload_size) {
        throw std::length_error("The reference runtime request exceeds the protocol limit.");
    }
    return payload;
}

std::optional<StartReferenceRuntimeRequest> decode_start_reference_runtime_request(
        std::span<const std::byte> payload) {
    std::size_t offset{};
    StartReferenceRuntimeRequest result;
    if (!take_u16(payload, offset, result.configuration_version) ||
        result.configuration_version != reference_runtime_configuration_version ||
        !take_text(payload, offset, maximum_avd_name_size, result.avd_name) ||
        offset != payload.size() || !valid_avd_name(result.avd_name)) return std::nullopt;
    return result;
}

std::vector<std::byte> encode_runtime_generation_request(
        const RuntimeGenerationRequest& request) {
    if (!valid_generation_id(request.generation_id)) {
        throw std::invalid_argument("The runtime generation identifier is invalid.");
    }
    std::vector<std::byte> payload;
    append_text(payload, request.generation_id);
    return payload;
}

std::optional<RuntimeGenerationRequest> decode_runtime_generation_request(
        std::span<const std::byte> payload) {
    std::size_t offset{};
    RuntimeGenerationRequest result;
    if (!take_text(payload, offset, 36, result.generation_id) ||
        offset != payload.size() || !valid_generation_id(result.generation_id)) return std::nullopt;
    return result;
}

std::vector<std::byte> encode_runtime_status_data(const RuntimeStatusData& status) {
    if (status.schema_version != 1 || !valid_runtime_state(static_cast<std::uint8_t>(status.state)) ||
        !valid_failure_stage(static_cast<std::uint16_t>(status.failure_stage)) ||
        status.generation_id.size() > 36 || status.avd_name.size() > maximum_avd_name_size ||
        status.adb_serial.size() > 32 || status.diagnostic.size() > maximum_runtime_text_size ||
        status.generation_id.find('\0') != std::string::npos ||
        status.adb_serial.find('\0') != std::string::npos ||
        status.diagnostic.find('\0') != std::string::npos ||
        (!status.generation_id.empty() && !valid_generation_id(status.generation_id)) ||
        (!status.avd_name.empty() && !valid_avd_name(status.avd_name))) {
        throw std::invalid_argument("The runtime status data is invalid or exceeds its bounds.");
    }
    std::vector<std::byte> output;
    append_u16(output, status.schema_version);
    output.push_back(static_cast<std::byte>(status.state));
    output.push_back(status.guest_ready ? std::byte{1} : std::byte{0});
    append_u16(output, static_cast<std::uint16_t>(status.failure_stage));
    append_u32(output, status.process_id);
    append_u64(output, status.process_creation_time);
    append_u32(output, status.active_process_count);
    append_u32(output, status.native_error);
    append_u64(output, status.elapsed_ms);
    append_text(output, status.generation_id);
    append_text(output, status.avd_name);
    append_text(output, status.adb_serial);
    append_text(output, status.diagnostic);
    if (output.size() > maximum_payload_size) {
        throw std::length_error("The runtime status data exceeds the protocol limit.");
    }
    return output;
}

std::optional<RuntimeStatusData> decode_runtime_status_data(
        std::span<const std::byte> payload) {
    std::size_t offset{};
    RuntimeStatusData result;
    std::uint8_t state{};
    std::uint8_t ready{};
    std::uint16_t failure_stage{};
    if (!take_u16(payload, offset, result.schema_version) || result.schema_version != 1 ||
        offset + 2 > payload.size()) return std::nullopt;
    state = std::to_integer<std::uint8_t>(payload[offset++]);
    ready = std::to_integer<std::uint8_t>(payload[offset++]);
    if (!valid_runtime_state(state) || ready > 1 || !take_u16(payload, offset, failure_stage) ||
        !valid_failure_stage(failure_stage) || !take_u32(payload, offset, result.process_id) ||
        !take_u64(payload, offset, result.process_creation_time) ||
        !take_u32(payload, offset, result.active_process_count) ||
        !take_u32(payload, offset, result.native_error) || !take_u64(payload, offset, result.elapsed_ms) ||
        !take_text(payload, offset, 36, result.generation_id) ||
        !take_text(payload, offset, maximum_avd_name_size, result.avd_name) ||
        !take_text(payload, offset, 32, result.adb_serial) ||
        !take_text(payload, offset, maximum_runtime_text_size, result.diagnostic) ||
        offset != payload.size() ||
        (!result.generation_id.empty() && !valid_generation_id(result.generation_id)) ||
        (!result.avd_name.empty() && !valid_avd_name(result.avd_name))) return std::nullopt;
    result.state = static_cast<RuntimeLifecycleState>(state);
    result.guest_ready = ready != 0;
    result.failure_stage = static_cast<RuntimeFailureStage>(failure_stage);
    return result;
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
    if (reply.message.size() > maximum_payload_size - reply_fixed_payload_size ||
        reply.data.size() > maximum_payload_size - reply_fixed_payload_size - reply.message.size()) {
        throw std::length_error("Broker reply data exceeds the protocol limit.");
    }

    const auto payload_size = reply_fixed_payload_size + reply.message.size() + reply.data.size();
    const auto header = encode_header(reply_type, request_id, static_cast<std::uint32_t>(payload_size));
    std::vector<std::byte> bytes(header.begin(), header.end());
    bytes.resize(wire_header_size + payload_size);
    auto output = std::span<std::byte>(bytes);
    write_u16(output, wire_header_size, static_cast<std::uint16_t>(reply.error));
    output[wire_header_size + 2] = static_cast<std::byte>(reply.state);
    write_u32(output, wire_header_size + 3, static_cast<std::uint32_t>(reply.message.size()));
    write_u32(output, wire_header_size + 7, static_cast<std::uint32_t>(reply.data.size()));
    std::transform(reply.message.begin(), reply.message.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(wire_header_size + reply_fixed_payload_size),
        [](char value) { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
    std::copy(reply.data.begin(), reply.data.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(wire_header_size + reply_fixed_payload_size +
            reply.message.size()));
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
    const auto data_size = read_u32(bytes, wire_header_size + 7);
    if (!valid_broker_error(error_code) || !valid_lifecycle_state(state) ||
        message_size > payload_size - reply_fixed_payload_size ||
        data_size != payload_size - reply_fixed_payload_size - message_size) {
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
    const auto data_begin = begin + static_cast<std::ptrdiff_t>(message_size);
    reply.data.assign(data_begin, data_begin + static_cast<std::ptrdiff_t>(data_size));
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
