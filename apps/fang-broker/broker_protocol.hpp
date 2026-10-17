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

inline constexpr std::uint16_t protocol_version = 2;
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
    ShutdownOwner = 8,
    StartReferenceRuntime = 9,
    QueryRuntimeStatus = 10,
    ShutdownRuntime = 11
};

inline constexpr std::uint16_t reference_runtime_configuration_version = 1;

enum class RuntimeLifecycleState : std::uint8_t {
    Stopped,
    Starting,
    ProcessCreated,
    OwnershipVerified,
    GuestBooting,
    Ready,
    Stopping,
    RecoveryRequired,
    Failed
};

enum class RuntimeFailureStage : std::uint16_t {
    None,
    ValidateConfiguration,
    ResolveSdk,
    ResolveAvd,
    CheckAdbPort,
    CreateJob,
    CreateProcess,
    VerifyOwnership,
    AdbTransport,
    GuestBoot,
    Shutdown,
    Finalize,
    Unknown
};

inline constexpr std::string_view runtime_lifecycle_name(RuntimeLifecycleState state) noexcept {
    switch (state) {
    case RuntimeLifecycleState::Stopped: return "Stopped";
    case RuntimeLifecycleState::Starting: return "Starting";
    case RuntimeLifecycleState::ProcessCreated: return "Process created";
    case RuntimeLifecycleState::OwnershipVerified: return "Ownership verified";
    case RuntimeLifecycleState::GuestBooting: return "Guest booting";
    case RuntimeLifecycleState::Ready: return "Ready";
    case RuntimeLifecycleState::Stopping: return "Stopping";
    case RuntimeLifecycleState::RecoveryRequired: return "Recovery required";
    case RuntimeLifecycleState::Failed: return "Failed";
    }
    return "Unknown";
}

inline constexpr std::string_view runtime_failure_stage_name(RuntimeFailureStage stage) noexcept {
    switch (stage) {
    case RuntimeFailureStage::None: return "None";
    case RuntimeFailureStage::ValidateConfiguration: return "ValidateConfiguration";
    case RuntimeFailureStage::ResolveSdk: return "ResolveSdk";
    case RuntimeFailureStage::ResolveAvd: return "ResolveAvd";
    case RuntimeFailureStage::CheckAdbPort: return "CheckAdbPort";
    case RuntimeFailureStage::CreateJob: return "CreateJob";
    case RuntimeFailureStage::CreateProcess: return "CreateProcess";
    case RuntimeFailureStage::VerifyOwnership: return "VerifyOwnership";
    case RuntimeFailureStage::AdbTransport: return "AdbTransport";
    case RuntimeFailureStage::GuestBoot: return "GuestBoot";
    case RuntimeFailureStage::Shutdown: return "Shutdown";
    case RuntimeFailureStage::Finalize: return "Finalize";
    case RuntimeFailureStage::Unknown: return "Unknown";
    }
    return "Unknown";
}

struct StartReferenceRuntimeRequest {
    std::uint16_t configuration_version{reference_runtime_configuration_version};
    std::string avd_name;
};

struct RuntimeGenerationRequest {
    std::string generation_id;
};

struct RuntimeStatusData {
    std::uint16_t schema_version{1};
    RuntimeLifecycleState state{RuntimeLifecycleState::Stopped};
    RuntimeFailureStage failure_stage{RuntimeFailureStage::None};
    bool guest_ready{};
    std::uint32_t process_id{};
    std::uint64_t process_creation_time{};
    std::uint32_t active_process_count{};
    std::uint32_t native_error{};
    std::uint64_t elapsed_ms{};
    std::string generation_id;
    std::string avd_name;
    std::string adb_serial;
    std::string diagnostic;
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
    StoreNotInitialized,
    InvalidRuntimeConfiguration,
    RuntimeAlreadyExists,
    RuntimeNotFound,
    RuntimeStartFailed,
    RuntimeShutdownFailed
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
    std::vector<std::byte> data;
};

struct ReplyParseResult {
    std::optional<BrokerReply> reply;
    ProtocolError error{ProtocolError::None};
    std::uint64_t request_id{};
    bool has_request_id{};
};

std::vector<std::byte> encode_start_reference_runtime_request(
    const StartReferenceRuntimeRequest& request);
std::optional<StartReferenceRuntimeRequest> decode_start_reference_runtime_request(
    std::span<const std::byte> payload);
std::vector<std::byte> encode_runtime_generation_request(
    const RuntimeGenerationRequest& request);
std::optional<RuntimeGenerationRequest> decode_runtime_generation_request(
    std::span<const std::byte> payload);
std::vector<std::byte> encode_runtime_status_data(const RuntimeStatusData& status);
std::optional<RuntimeStatusData> decode_runtime_status_data(
    std::span<const std::byte> payload);

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
