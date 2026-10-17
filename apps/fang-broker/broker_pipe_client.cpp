#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "broker_pipe.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace fang::broker {
namespace {

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() { if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_{};
};

enum class TransferResult { Complete, TimedOut, Failed };

TransferResult transfer_exact(HANDLE pipe, std::byte* data, DWORD size, bool write,
    DWORD timeout_ms, DWORD& native_error) {
    DWORD completed{};
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (completed < size) {
        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (event.get() == nullptr) {
            native_error = GetLastError();
            return TransferResult::Failed;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred{};
        const BOOL immediate = write
            ? WriteFile(pipe, data + completed, size - completed, &transferred, &overlapped)
            : ReadFile(pipe, data + completed, size - completed, &transferred, &overlapped);
        if (!immediate) {
            native_error = GetLastError();
            if (native_error != ERROR_IO_PENDING) return TransferResult::Failed;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                CancelIoEx(pipe, &overlapped);
                DWORD ignored{};
                GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                native_error = ERROR_TIMEOUT;
                return TransferResult::TimedOut;
            }
            const DWORD wait = WaitForSingleObject(event.get(),
                static_cast<DWORD>(deadline - now));
            if (wait == WAIT_TIMEOUT) {
                CancelIoEx(pipe, &overlapped);
                DWORD ignored{};
                GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                native_error = ERROR_TIMEOUT;
                return TransferResult::TimedOut;
            }
            if (wait != WAIT_OBJECT_0 ||
                GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) == 0) {
                native_error = GetLastError();
                return TransferResult::Failed;
            }
        }
        if (transferred == 0) {
            native_error = ERROR_BROKEN_PIPE;
            return TransferResult::Failed;
        }
        completed += transferred;
    }
    return TransferResult::Complete;
}

BrokerCallResult transport_failure(DWORD error, std::string message) {
    return {{error == ERROR_TIMEOUT ? BrokerErrorCode::InternalFailure
            : BrokerErrorCode::InternalFailure, error, std::move(message)},
        std::nullopt, ProtocolError::None};
}

}

BrokerPipeClient::BrokerPipeClient(std::wstring pipe_name, std::uint32_t timeout_ms)
    : pipe_name_(std::move(pipe_name)), timeout_ms_(timeout_ms) {}

BrokerCallResult BrokerPipeClient::call(const Request& request) const {
    if (pipe_name_.empty() || timeout_ms_ == 0 || request.request_id == 0) {
        return transport_failure(ERROR_INVALID_PARAMETER,
            "Broker client endpoint, timeout, or request identifier is invalid.");
    }
    const auto deadline = GetTickCount64() + timeout_ms_;
    HANDLE raw_pipe = INVALID_HANDLE_VALUE;
    do {
        raw_pipe = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (raw_pipe != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            return transport_failure(error, "Could not connect to the supervisor pipe.");
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline || WaitNamedPipeW(pipe_name_.c_str(),
                static_cast<DWORD>(std::min<ULONGLONG>(100, deadline - now))) == 0) {
            if (GetLastError() != ERROR_SEM_TIMEOUT && GetLastError() != ERROR_FILE_NOT_FOUND &&
                GetLastError() != ERROR_PIPE_BUSY) {
                return transport_failure(GetLastError(), "Could not wait for the supervisor pipe.");
            }
        }
    } while (GetTickCount64() < deadline);
    if (raw_pipe == INVALID_HANDLE_VALUE) {
        return transport_failure(ERROR_TIMEOUT, "Timed out connecting to the supervisor pipe.");
    }
    ScopedHandle pipe(raw_pipe);

    std::vector<std::byte> frame;
    try {
        frame = encode_request(request);
    } catch (const std::exception& error) {
        return transport_failure(ERROR_INVALID_DATA, error.what());
    }
    DWORD native_error{};
    const DWORD remaining = static_cast<DWORD>(deadline > GetTickCount64()
        ? deadline - GetTickCount64() : 1);
    auto transfer = transfer_exact(pipe.get(), frame.data(),
        static_cast<DWORD>(frame.size()), true, remaining, native_error);
    if (transfer != TransferResult::Complete) {
        return transport_failure(native_error, transfer == TransferResult::TimedOut
            ? "Timed out writing the supervisor request."
            : "Could not write the supervisor request.");
    }

    std::array<std::byte, wire_header_size> header{};
    transfer = transfer_exact(pipe.get(), header.data(),
        static_cast<DWORD>(header.size()), false, timeout_ms_, native_error);
    if (transfer != TransferResult::Complete) {
        return transport_failure(native_error, transfer == TransferResult::TimedOut
            ? "Timed out reading the supervisor reply header."
            : "Could not read the supervisor reply header.");
    }
    const auto payload_size = static_cast<std::uint32_t>(
        std::to_integer<std::uint8_t>(header[16])) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[17])) << 8) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[18])) << 16) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[19])) << 24);
    if (payload_size > maximum_payload_size) {
        return { {BrokerErrorCode::OversizedMessage, ERROR_INVALID_DATA,
            "Supervisor reply exceeds the protocol payload limit."},
            std::nullopt, ProtocolError::Oversized};
    }
    std::vector<std::byte> response(header.begin(), header.end());
    response.resize(wire_header_size + payload_size);
    if (payload_size > 0) {
        transfer = transfer_exact(pipe.get(), response.data() + wire_header_size,
            payload_size, false, timeout_ms_, native_error);
        if (transfer != TransferResult::Complete) {
            return transport_failure(native_error, transfer == TransferResult::TimedOut
                ? "Timed out reading the supervisor reply payload."
                : "Could not read the supervisor reply payload.");
        }
    }
    std::byte acknowledgement{0xa5};
    transfer = transfer_exact(pipe.get(), &acknowledgement,
        1, true, timeout_ms_, native_error);
    if (transfer != TransferResult::Complete) {
        return transport_failure(native_error, "Could not acknowledge the supervisor reply.");
    }
    auto decoded = decode_reply(response);
    if (decoded.error != ProtocolError::None || !decoded.reply.has_value() ||
        decoded.request_id != request.request_id) {
        return {{BrokerErrorCode::InvalidMessage, ERROR_INVALID_DATA,
            "Supervisor reply was malformed or did not match the request identifier."},
            std::nullopt, decoded.error == ProtocolError::None
                ? ProtocolError::MalformedReply : decoded.error};
    }
    return {{}, std::move(decoded.reply), ProtocolError::None};
}

}