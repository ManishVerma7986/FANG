#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>

#include "broker_pipe.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <vector>

namespace fang::broker {
namespace {

constexpr DWORD pipe_buffer_size = static_cast<DWORD>(maximum_wire_message_size);

enum class IoResult {
    Complete,
    Disconnected,
    Cancelled,
    TimedOut,
    Failed
};

struct IoOutcome {
    IoResult result{IoResult::Failed};
    DWORD error{};
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    ~ScopedHandle() {
        reset();
    }

    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    HANDLE handle_{};
};

class LocalMemory {
public:
    explicit LocalMemory(void* memory = nullptr) noexcept : memory_(memory) {}
    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;
    ~LocalMemory() {
        if (memory_ != nullptr) {
            LocalFree(memory_);
        }
    }

private:
    void* memory_{};
};

BrokerError win32_error(BrokerErrorCode code, DWORD error, std::string message) {
    return {code, error, std::move(message)};
}

bool sid_string(PSID sid, std::string& value, DWORD& error) {
    LPWSTR wide_sid{};
    if (ConvertSidToStringSidW(sid, &wide_sid) == 0) {
        error = GetLastError();
        return false;
    }
    LocalMemory memory(wide_sid);
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_sid, -1,
        nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        error = GetLastError();
        return false;
    }
    value.resize(static_cast<std::size_t>(length));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_sid, -1,
            value.data(), length, nullptr, nullptr) == 0) {
        error = GetLastError();
        value.clear();
        return false;
    }
    value.resize(static_cast<std::size_t>(length - 1));
    return true;
}

std::wstring widen(std::string_view value, DWORD& error) {
    if (value.empty()) {
        error = ERROR_INVALID_PARAMETER;
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        error = GetLastError();
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length) == 0) {
        error = GetLastError();
        return {};
    }
    return result;
}

bool read_token_identity(HANDLE token, std::string& sid_text, DWORD& session_id, DWORD& error) {
    DWORD size{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    const DWORD sizing_error = GetLastError();
    if (sizing_error != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        error = sizing_error;
        return false;
    }
    std::vector<std::byte> user_buffer(size);
    if (GetTokenInformation(token, TokenUser, user_buffer.data(), size, &size) == 0) {
        error = GetLastError();
        return false;
    }
    auto* user = reinterpret_cast<TOKEN_USER*>(user_buffer.data());
    if (!sid_string(user->User.Sid, sid_text, error)) {
        return false;
    }

    DWORD session_size = sizeof(session_id);
    if (GetTokenInformation(token, TokenSessionId, &session_id, sizeof(session_id),
            &session_size) == 0) {
        error = GetLastError();
        return false;
    }
    return true;
}

IoOutcome wait_overlapped(HANDLE pipe, HANDLE stop_event, OVERLAPPED& operation,
    DWORD& transferred, DWORD timeout_ms = INFINITE) {
    HANDLE events[]{stop_event, operation.hEvent};
    const DWORD wait = WaitForMultipleObjects(2, events, FALSE, timeout_ms);
    if (wait == WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        WaitForSingleObject(operation.hEvent, INFINITE);
        DWORD ignored{};
        GetOverlappedResult(pipe, &operation, &ignored, FALSE);
        return {IoResult::Cancelled, ERROR_OPERATION_ABORTED};
    }
    if (wait == WAIT_TIMEOUT) {
        CancelIoEx(pipe, &operation);
        WaitForSingleObject(operation.hEvent, INFINITE);
        DWORD ignored{};
        GetOverlappedResult(pipe, &operation, &ignored, FALSE);
        return {IoResult::TimedOut, ERROR_TIMEOUT};
    }
    if (wait != WAIT_OBJECT_0 + 1) {
        const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
        CancelIoEx(pipe, &operation);
        WaitForSingleObject(operation.hEvent, INFINITE);
        DWORD ignored{};
        GetOverlappedResult(pipe, &operation, &ignored, FALSE);
        return {IoResult::Failed, error};
    }
    if (GetOverlappedResult(pipe, &operation, &transferred, FALSE) == 0) {
        const DWORD error = GetLastError();
        return {error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA
            ? IoResult::Disconnected : IoResult::Failed, error};
    }
    return {IoResult::Complete, ERROR_SUCCESS};
}

IoOutcome transfer_exact(HANDLE pipe, HANDLE stop_event, void* bytes, DWORD size, bool write,
    DWORD timeout_ms = INFINITE) {
    const auto deadline = timeout_ms == INFINITE
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto* cursor = static_cast<std::byte*>(bytes);
    DWORD completed{};
    while (completed < size) {
        DWORD operation_timeout = INFINITE;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                return {IoResult::TimedOut, ERROR_TIMEOUT};
            }
            operation_timeout = static_cast<DWORD>(
                std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
        }
        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (event.get() == nullptr) {
            return {IoResult::Failed, GetLastError()};
        }
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        DWORD transferred{};
        const DWORD remaining = size - completed;
        const BOOL immediate = write
            ? WriteFile(pipe, cursor + completed, remaining, &transferred, &operation)
            : ReadFile(pipe, cursor + completed, remaining, &transferred, &operation);
        if (immediate == 0) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                return {error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA
                    ? IoResult::Disconnected : IoResult::Failed, error};
            }
            const auto outcome = wait_overlapped(pipe, stop_event, operation, transferred,
                operation_timeout);
            if (outcome.result != IoResult::Complete) {
                return outcome;
            }
        }
        if (transferred == 0) {
            return {IoResult::Disconnected, ERROR_BROKEN_PIPE};
        }
        completed += transferred;
    }
    return {IoResult::Complete, ERROR_SUCCESS};
}

IoOutcome connect_pipe(HANDLE pipe, HANDLE stop_event) {
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == nullptr) {
        return {IoResult::Failed, GetLastError()};
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    const BOOL connected = ConnectNamedPipe(pipe, &operation);
    if (connected != 0) {
        return {IoResult::Complete, ERROR_SUCCESS};
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
        return {IoResult::Complete, ERROR_SUCCESS};
    }
    if (error != ERROR_IO_PENDING) {
        return {IoResult::Failed, error};
    }
    DWORD ignored{};
    return wait_overlapped(pipe, stop_event, operation, ignored);
}

BrokerErrorCode error_code_for_io(DWORD error) {
    static_cast<void>(error);
    return BrokerErrorCode::InternalFailure;
}

bool write_reply(HANDLE pipe, HANDLE stop_event, std::uint64_t request_id,
    const BrokerReply& reply, BrokerError& error, DWORD timeout_ms) {
    auto response = encode_reply(request_id, reply);
    auto outcome = transfer_exact(pipe, stop_event,
        response.data(), static_cast<DWORD>(response.size()), true, timeout_ms);
    if (outcome.result != IoResult::Complete) {
        if (outcome.result == IoResult::Failed) {
            error = win32_error(error_code_for_io(outcome.error), outcome.error,
                "Could not write the broker response.");
        }
        return false;
    }
    std::byte acknowledgement{};
    outcome = transfer_exact(pipe, stop_event, &acknowledgement, 1, false, timeout_ms);
    if (outcome.result == IoResult::Failed) {
        error = win32_error(error_code_for_io(outcome.error), outcome.error,
            "Could not complete named-pipe response acknowledgement.");
        return false;
    }
    return outcome.result == IoResult::Complete && acknowledgement == std::byte{0xa5};
}

BrokerError authorize_pipe_client(HANDLE pipe, std::string_view expected_sid,
    std::optional<std::uint32_t> expected_session_id) {
    ULONG client_process_id{};
    if (GetNamedPipeClientProcessId(pipe, &client_process_id) == 0 || client_process_id == 0) {
        const DWORD error = GetLastError();
        return win32_error(BrokerErrorCode::UnauthorizedClient, error,
            "Windows could not identify the named-pipe client process.");
    }
    if (ImpersonateNamedPipeClient(pipe) == 0) {
        const DWORD error = GetLastError();
        return win32_error(BrokerErrorCode::UnauthorizedClient, error,
            "The broker could not impersonate the named-pipe client for identity validation.");
    }

    BrokerError result;
    HANDLE token_handle{};
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token_handle) == 0) {
        const DWORD error = GetLastError();
        result = win32_error(BrokerErrorCode::UnauthorizedClient, error,
            "The broker could not inspect the named-pipe client's access token.");
    } else {
        ScopedHandle token(token_handle);
        std::string actual_sid;
        DWORD token_session{};
        DWORD error{};
        if (!read_token_identity(token.get(), actual_sid, token_session, error)) {
            result = win32_error(BrokerErrorCode::UnauthorizedClient, error,
                "The broker could not read the named-pipe client's SID and session.");
        } else {
            DWORD process_session{};
            if (ProcessIdToSessionId(client_process_id, &process_session) == 0) {
                error = GetLastError();
                result = win32_error(BrokerErrorCode::UnauthorizedClient, error,
                    "The broker could not verify the named-pipe client's process session.");
            } else {
                if (actual_sid != expected_sid || token_session != process_session ||
                    (expected_session_id.has_value() &&
                        token_session != *expected_session_id)) {
                    result = {BrokerErrorCode::UnauthorizedClient, ERROR_ACCESS_DENIED,
                        "The named-pipe client identity did not match the broker's user or process session."};
                }
            }
        }
    }

    if (RevertToSelf() == 0) {
        const DWORD error = GetLastError();
        return win32_error(BrokerErrorCode::InternalFailure, error,
            "The broker could not revert its named-pipe client impersonation.");
    }
    return result;
}

}

IdentityResult current_user_identity() {
    HANDLE token_handle{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_handle) == 0) {
        const DWORD error = GetLastError();
        return {false, {}, win32_error(BrokerErrorCode::InternalFailure, error,
            "Could not open the broker process token.")};
    }
    ScopedHandle token(token_handle);
    std::string sid_text;
    DWORD session_id{};
    DWORD error{};
    if (!read_token_identity(token.get(), sid_text, session_id, error)) {
        return {false, {}, win32_error(BrokerErrorCode::InternalFailure, error,
            "Could not read the broker user SID and session.")};
    }
    return {true, {std::move(sid_text), session_id}, {}};
}

std::wstring pipe_name_for_user(std::string_view user_sid) {
    DWORD error{};
    const auto wide_sid = widen(user_sid, error);
    if (wide_sid.empty()) {
        return {};
    }
    return L"\\\\.\\pipe\\Fang.RuntimeBroker.v1." + wide_sid;
}

std::wstring supervisor_pipe_name_for_user(std::string_view user_sid) {
    DWORD error{};
    const auto wide_sid = widen(user_sid, error);
    if (wide_sid.empty()) return {};
    return L"\\\\.\\pipe\\Fang.RuntimeSupervisor.v1." + wide_sid;
}

BrokerError authorize_client(std::string_view expected_user_sid, const ClientIdentity& client) {
    if (expected_user_sid.empty() || client.sid.empty() || client.process_id == 0 ||
        client.sid != expected_user_sid ||
        client.token_session_id != client.process_session_id) {
        return {BrokerErrorCode::UnauthorizedClient, ERROR_ACCESS_DENIED,
            "The named-pipe client identity did not match the broker's user or process session."};
    }
    return {};
}

BrokerPipeServer::BrokerPipeServer(std::wstring pipe_name, std::string expected_user_sid,
    BrokerLifecycle& lifecycle, IRequestDispatcher& dispatcher,
    std::uint32_t request_timeout_ms, PipeEndpointKind endpoint,
    std::optional<std::uint32_t> expected_session_id)
    : pipe_name_(std::move(pipe_name)), expected_user_sid_(std::move(expected_user_sid)),
        lifecycle_(lifecycle), dispatcher_(dispatcher), request_timeout_ms_(request_timeout_ms),
        endpoint_(endpoint), expected_session_id_(expected_session_id) {
    const HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stop_event_ = stop_event;
    if (stop_event == nullptr) {
        stop_event_error_ = GetLastError();
    }
}

BrokerPipeServer::~BrokerPipeServer() {
    if (stop_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stop_event_));
    }
}

bool BrokerPipeServer::request_stop() noexcept {
    if (lifecycle_.state() == LifecycleState::Ready) {
        lifecycle_.request_shutdown();
    }
    return stop_event_ != nullptr && SetEvent(static_cast<HANDLE>(stop_event_)) != 0;
}

BrokerError BrokerPipeServer::run() {
    if (run_started_.test_and_set(std::memory_order_acq_rel)) {
        return {BrokerErrorCode::InternalFailure, ERROR_INVALID_STATE,
            "The broker pipe server can only be run once."};
    }
    if (stop_event_ == nullptr) {
        BrokerError failure{BrokerErrorCode::InternalFailure, stop_event_error_,
            "Could not create the broker shutdown event."};
        lifecycle_.fail(failure);
        return failure;
    }
    if (pipe_name_.empty() || expected_user_sid_.empty()) {
        BrokerError failure{BrokerErrorCode::InternalFailure, ERROR_INVALID_PARAMETER,
            "The broker pipe name or expected user SID is empty."};
        lifecycle_.fail(failure);
        return failure;
    }

    DWORD sid_error{};
    const auto wide_sid = widen(expected_user_sid_, sid_error);
    if (wide_sid.empty()) {
        BrokerError failure{BrokerErrorCode::InternalFailure, sid_error,
            "The broker user SID is not valid UTF-8."};
        lifecycle_.fail(failure);
        return failure;
    }
    PSID parsed_sid{};
    if (ConvertStringSidToSidW(wide_sid.c_str(), &parsed_sid) == 0) {
        const DWORD error = GetLastError();
        BrokerError failure{BrokerErrorCode::InternalFailure, error,
            "The broker user SID is not a valid Windows SID."};
        lifecycle_.fail(failure);
        return failure;
    }
    LocalMemory parsed_sid_memory(parsed_sid);
    std::string canonical_sid;
    if (!sid_string(parsed_sid, canonical_sid, sid_error) || canonical_sid != expected_user_sid_) {
        BrokerError failure{BrokerErrorCode::InternalFailure,
            sid_error == ERROR_SUCCESS ? ERROR_INVALID_SID : sid_error,
            "The broker user SID is not in canonical string form."};
        lifecycle_.fail(failure);
        return failure;
    }
    const auto expected_pipe_name = endpoint_ == PipeEndpointKind::Broker
        ? L"\\\\.\\pipe\\Fang.RuntimeBroker.v1." + wide_sid
        : L"\\\\.\\pipe\\Fang.RuntimeSupervisor.v1." + wide_sid;
    if (pipe_name_ != expected_pipe_name) {
        BrokerError failure{BrokerErrorCode::InternalFailure, ERROR_INVALID_NAME,
            "The broker pipe name does not match its user-scoped endpoint."};
        lifecycle_.fail(failure);
        return failure;
    }
    const std::wstring sddl = L"O:" + wide_sid + L"D:P(A;;GA;;;SY)(A;;GRGW;;;" +
        wide_sid + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        const DWORD error = GetLastError();
        BrokerError failure{BrokerErrorCode::InternalFailure, error,
            "Could not create the explicit per-user named-pipe security descriptor."};
        lifecycle_.fail(failure);
        return failure;
    }
    LocalMemory descriptor_memory(descriptor);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    if (!lifecycle_.mark_ready()) {
        BrokerError failure{BrokerErrorCode::InternalFailure, ERROR_INVALID_STATE,
            "The broker lifecycle could not transition from Starting to Ready."};
        lifecycle_.fail(failure);
        return failure;
    }

    while (WaitForSingleObject(static_cast<HANDLE>(stop_event_), 0) != WAIT_OBJECT_0) {
        ScopedHandle pipe(CreateNamedPipeW(pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, pipe_buffer_size, pipe_buffer_size, 0, &security));
        if (pipe.get() == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            BrokerError failure{error_code_for_io(error), error,
                "Could not create the secured local broker pipe instance."};
            lifecycle_.fail(failure);
            return failure;
        }

        const auto connected = connect_pipe(pipe.get(), static_cast<HANDLE>(stop_event_));
        if (connected.result == IoResult::Cancelled) {
            break;
        }
        if (connected.result == IoResult::Failed) {
            BrokerError failure{error_code_for_io(connected.error), connected.error,
                "The broker could not wait for a local pipe client."};
            lifecycle_.fail(failure);
            return failure;
        }
        if (connected.result == IoResult::Complete) {
            const auto error = serve_client(pipe.get());
            if (error) {
                if (error.native_error == ERROR_BROKEN_PIPE || error.native_error == ERROR_NO_DATA ||
                    error.native_error == ERROR_OPERATION_ABORTED) {
                    continue;
                }
                lifecycle_.fail(error);
                return error;
            }
        }
        if (DisconnectNamedPipe(pipe.get()) == 0) {
            const DWORD error = GetLastError();
            if (error != ERROR_PIPE_NOT_CONNECTED && error != ERROR_NO_DATA) {
                BrokerError failure{BrokerErrorCode::InternalFailure, error,
                    "Could not disconnect the completed broker pipe instance."};
                lifecycle_.fail(failure);
                return failure;
            }
        }
    }

    if (lifecycle_.state() == LifecycleState::Ready) {
        lifecycle_.request_shutdown();
    }
    if (!lifecycle_.mark_stopped()) {
        BrokerError failure{BrokerErrorCode::InternalFailure, ERROR_INVALID_STATE,
            "The broker could not transition to Stopped during shutdown."};
        lifecycle_.fail(failure);
        return failure;
    }
    return {};
}

BrokerError BrokerPipeServer::serve_client(void* pipe_handle) {
    const auto pipe = static_cast<HANDLE>(pipe_handle);
    const auto stop_event = static_cast<HANDLE>(stop_event_);
    std::array<std::byte, wire_header_size> header_bytes{};
    auto io = transfer_exact(pipe, stop_event, header_bytes.data(),
        static_cast<DWORD>(header_bytes.size()), false, request_timeout_ms_);
    if (io.result == IoResult::Disconnected || io.result == IoResult::TimedOut) {
        return {BrokerErrorCode::None, ERROR_BROKEN_PIPE, {}};
    }
    if (io.result == IoResult::Cancelled) {
        return {BrokerErrorCode::None, ERROR_OPERATION_ABORTED, {}};
    }
    if (io.result == IoResult::Failed) {
        return win32_error(error_code_for_io(io.error), io.error,
            "Could not read the broker request header.");
    }

    const auto inspected = inspect_request_header(header_bytes);
    if (inspected.error != ProtocolError::None) {
        if (inspected.payload_size > 0 && inspected.payload_size <= maximum_payload_size) {
            std::vector<std::byte> discarded_payload(inspected.payload_size);
            io = transfer_exact(pipe, stop_event, discarded_payload.data(),
                inspected.payload_size, false, request_timeout_ms_);
            if (io.result == IoResult::Disconnected || io.result == IoResult::Cancelled ||
                io.result == IoResult::TimedOut) {
                return {BrokerErrorCode::None, io.error, {}};
            }
            if (io.result == IoResult::Failed) {
                return win32_error(error_code_for_io(io.error), io.error,
                    "Could not consume the bounded malformed request payload.");
            }
        }
        if (inspected.has_request_id) {
            BrokerError write_error;
            write_reply(pipe, stop_event, inspected.request_id,
                {broker_error_for(inspected.error), lifecycle_.state(),
                    "The request header is invalid."}, write_error, request_timeout_ms_);
            return write_error;
        }
        return {};
    }

    std::vector<std::byte> frame(header_bytes.begin(), header_bytes.end());
    frame.resize(wire_header_size + inspected.payload_size);
    if (inspected.payload_size > 0) {
        io = transfer_exact(pipe, stop_event, frame.data() + wire_header_size,
            inspected.payload_size, false, request_timeout_ms_);
        if (io.result == IoResult::Disconnected || io.result == IoResult::TimedOut) {
            return {BrokerErrorCode::None, ERROR_BROKEN_PIPE, {}};
        }
        if (io.result == IoResult::Cancelled) {
            return {BrokerErrorCode::None, ERROR_OPERATION_ABORTED, {}};
        }
        if (io.result == IoResult::Failed) {
            return win32_error(error_code_for_io(io.error), io.error,
                "Could not read the broker request payload.");
        }
    }

    const auto decoded = decode_request(frame);
    if (decoded.error != ProtocolError::None || !decoded.request.has_value()) {
        BrokerError write_error;
        write_reply(pipe, stop_event, inspected.request_id,
            {broker_error_for(decoded.error), lifecycle_.state(),
                "The request envelope is malformed."}, write_error, request_timeout_ms_);
        return write_error;
    }

    const auto authorization = authorize_pipe_client(pipe, expected_user_sid_,
        expected_session_id_);
    if (authorization) {
        if (authorization.code != BrokerErrorCode::UnauthorizedClient) {
            return authorization;
        }
        BrokerError write_error;
        write_reply(pipe, stop_event, decoded.request_id,
            {BrokerErrorCode::UnauthorizedClient, lifecycle_.state(), authorization.message},
            write_error, request_timeout_ms_);
        return write_error;
    }

    const auto reply = dispatcher_.dispatch(*decoded.request);
    BrokerError write_error;
        if (!write_reply(pipe, stop_event, decoded.request_id, reply, write_error,
            request_timeout_ms_)) {
        return write_error;
    }
    return {};
}

}
