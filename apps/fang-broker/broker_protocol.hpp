#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace fang::broker {

inline constexpr std::uint16_t protocol_version = 1;
inline constexpr std::size_t wire_header_size = 20;
inline constexpr std::size_t maximum_payload_size = 4096;
inline constexpr std::size_t maximum_wire_message_size = wire_header_size + maximum_payload_size;

enum class RequestType : std::uint16_t {
    Ping = 1,
    GetStatus = 2,
    InitializeOwnerStore = 3,
    QueryOwnership = 4,
    ReserveIdentity = 5,
    ReleaseReservation = 6,
    FinalizeEmpty = 7,
    ShutdownOwner = 8
};

enum class ProtocolError : std::uint16_t {
    None,
    Truncated,
    InvalidMagic,
    UnsupportedVersion,
    InvalidRequestId,
    Oversized,
    UnknownType,
    UnexpectedPayload,
    MalformedReply
};

enum class BrokerErrorCode : std::uint16_t {
    None,
    InvalidMessage,
    UnsupportedVersion,
    OversizedMessage,
    UnknownMessageType,
    DuplicateRequestId,
    UnauthorizedClient,
    ShuttingDown,
    InternalFailure,
    Busy,
    InvalidState,
    RecoveryRequired,
    StoreNotInitialized
};

enum class LifecycleState : std::uint8_t {
    Starting,
    Ready,
    ShuttingDown,
    Stopped,
    Failed
};

struct Request {
    RequestType type{};
    std::uint64_t request_id{};
    std::vector<std::byte> payload;
};

struct RequestHeader {
    std::uint16_t version{};
    std::uint16_t type{};
    std::uint64_t request_id{};
    std::uint32_t payload_size{};
    ProtocolError error{ProtocolError::None};
    bool has_request_id{};
};

struct RequestParseResult {
    std::optional<Request> request;
    ProtocolError error{ProtocolError::None};
    std::uint64_t request_id{};
    bool has_request_id{};
};

struct BrokerReply {
    BrokerErrorCode error{BrokerErrorCode::None};
    LifecycleState state{LifecycleState::Starting};
    std::string message;
};

struct ReplyParseResult {
    std::optional<BrokerReply> reply;
    ProtocolError error{ProtocolError::None};
    std::uint64_t request_id{};
    bool has_request_id{};
};

std::array<std::byte, wire_header_size> encode_request_header(const Request& request) noexcept;
std::vector<std::byte> encode_request(const Request& request);
RequestHeader inspect_request_header(std::span<const std::byte> bytes) noexcept;
RequestParseResult decode_request(std::span<const std::byte> bytes) noexcept;
std::vector<std::byte> encode_reply(std::uint64_t request_id, const BrokerReply& reply);
ReplyParseResult decode_reply(std::span<const std::byte> bytes);
BrokerErrorCode broker_error_for(ProtocolError error) noexcept;

class IRequestDispatcher {
public:
    virtual ~IRequestDispatcher() = default;
    virtual BrokerReply dispatch(const Request& request) = 0;
};

class RequestIdWindow {
public:
    explicit RequestIdWindow(std::size_t capacity = 4096);

    bool observe(std::uint64_t request_id);
    std::size_t size() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<std::uint64_t> order_;
    std::unordered_set<std::uint64_t> ids_;
};

}
