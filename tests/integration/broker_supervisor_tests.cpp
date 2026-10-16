#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "broker_native_job.hpp"
#include "broker_pipe.hpp"
#include "broker_supervisor.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace fang::broker;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

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

struct RpcReply {
    ReplyParseResult parsed;
    BrokerErrorCode error{BrokerErrorCode::InternalFailure};
    std::string message;
};

bool write_all(HANDLE pipe, const std::byte* data, DWORD size) {
    DWORD completed{};
    while (completed < size) {
        DWORD written{};
        if (WriteFile(pipe, data + completed, size - completed, &written, nullptr) == 0 ||
            written == 0) return false;
        completed += written;
    }
    return true;
}

bool read_all(HANDLE pipe, std::byte* data, DWORD size) {
    DWORD completed{};
    while (completed < size) {
        DWORD received{};
        if (ReadFile(pipe, data + completed, size - completed, &received, nullptr) == 0 ||
            received == 0) return false;
        completed += received;
    }
    return true;
}

HANDLE connect_pipe(std::wstring_view name) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    const std::wstring pipe_name(name);
    while (GetTickCount64() < deadline) {
        HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
        WaitNamedPipeW(pipe_name.c_str(), 50);
        Sleep(5);
    }
    return INVALID_HANDLE_VALUE;
}

RpcReply rpc_call(std::wstring_view pipe_name, const Request& request) {
    ScopedHandle pipe(connect_pipe(pipe_name));
    require(pipe.get() != INVALID_HANDLE_VALUE, "Could not connect to the supervisor pipe.");
    const auto frame = encode_request(request);
    require(write_all(pipe.get(), frame.data(), static_cast<DWORD>(frame.size())),
        "Could not send the supervisor request.");
    std::array<std::byte, wire_header_size> header{};
    require(read_all(pipe.get(), header.data(), static_cast<DWORD>(header.size())),
        "Could not read the supervisor reply header.");
    const auto payload_size = static_cast<std::uint32_t>(
        std::to_integer<std::uint8_t>(header[16])) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[17])) << 8) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[18])) << 16) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[19])) << 24);
    require(payload_size <= maximum_payload_size,
        "Supervisor reply exceeded the protocol payload bound.");
    std::vector<std::byte> bytes(header.begin(), header.end());
    bytes.resize(wire_header_size + payload_size);
    if (payload_size > 0) {
        require(read_all(pipe.get(), bytes.data() + wire_header_size, payload_size),
            "Could not read the supervisor reply payload.");
    }
    const std::byte acknowledgement{0xa5};
    require(write_all(pipe.get(), &acknowledgement, 1),
        "Could not acknowledge the supervisor reply.");
    RpcReply result;
    result.parsed = decode_reply(bytes);
    require(result.parsed.reply.has_value() && result.parsed.error == ProtocolError::None,
        "Supervisor returned a malformed reply.");
    result.error = result.parsed.reply->error;
    result.message = result.parsed.reply->message;
    return result;
}

class SupervisorFixture {
public:
    SupervisorFixture(const UserIdentity& identity, std::wstring storage_root)
        : owner_generation_(create_generation()),
          owner_(identity.sid, identity.session_id, GetCurrentProcessId(),
              owner_generation_, std::move(storage_root)),
          server_(supervisor_pipe_name_for_user(identity.sid), identity.sid,
              transport_, owner_, 1500, PipeEndpointKind::Supervisor, identity.session_id),
          completion_(std::async(std::launch::async, [&] { return server_.run(); })) {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (transport_.state() == LifecycleState::Starting && GetTickCount64() < deadline) {
            Sleep(5);
        }
        require(transport_.state() == LifecycleState::Ready,
            "The test supervisor did not reach Ready.");
    }

    SupervisorFixture(const SupervisorFixture&) = delete;
    SupervisorFixture& operator=(const SupervisorFixture&) = delete;

    ~SupervisorFixture() {
        server_.request_stop();
        if (completion_.valid()) completion_.wait_for(std::chrono::seconds(5));
    }

    BrokerPipeServer& server() noexcept { return server_; }
    std::string_view owner_generation() const noexcept { return owner_generation_; }

private:
    static std::string create_generation() {
        std::string error;
        auto value = create_runtime_generation_id(error);
        require(!value.empty(), error);
        return value;
    }

    std::string owner_generation_;
    OwnershipSupervisor owner_;
    BrokerLifecycle transport_;
    BrokerPipeServer server_;
    std::future<BrokerError> completion_;
};

std::wstring unique_test_root(std::string_view generation) {
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    require(size > 1, "LOCALAPPDATA is unavailable to supervisor tests.");
    std::wstring root(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), size);
    require(written > 0 && written < size, "Could not read LOCALAPPDATA.");
    root.resize(written);
    root += L"\\Fang\\RuntimeSupervisorTests\\";
    root.append(generation.begin(), generation.end());
    return root;
}

class RemoveDirectoryTree {
public:
    explicit RemoveDirectoryTree(std::wstring path) : path_(std::move(path)) {}
    RemoveDirectoryTree(const RemoveDirectoryTree&) = delete;
    RemoveDirectoryTree& operator=(const RemoveDirectoryTree&) = delete;
    ~RemoveDirectoryTree() {
        const auto index = path_ + L"\\owner-index.fji";
        DeleteFileW(index.c_str());
        RemoveDirectoryW(path_.c_str());
        const auto parent = path_.substr(0, path_.find_last_of(L'\\'));
        RemoveDirectoryW(parent.c_str());
    }
    const std::wstring& path() const noexcept { return path_; }
private:
    std::wstring path_;
};

CanonicalAvdIdentity identity_for(std::string sid, std::string_view label) {
    return {std::move(sid), "C:\\FangSupervisorTest\\" + std::string(label)};
}

void test_supervisor_rpc_bootstrap_reservation_and_reconnect() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string generation_error;
    const auto fixture_id = create_runtime_generation_id(generation_error);
    require(!fixture_id.empty(), generation_error);
    RemoveDirectoryTree cleanup(unique_test_root(fixture_id));
    SupervisorFixture supervisor(user.identity, cleanup.path());
    const auto pipe_name = supervisor_pipe_name_for_user(user.identity.sid);
    const auto identity = identity_for(user.identity.sid, "rpc-roundtrip");

    auto reply = rpc_call(pipe_name, {RequestType::QueryOwnership, 1001,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::StoreNotInitialized,
        "Missing authority store was treated as initialized-empty.");

    std::array<BrokerErrorCode, 2> initialize_results{
        BrokerErrorCode::InternalFailure, BrokerErrorCode::InternalFailure};
    std::array<std::thread, 2> initializers{
        std::thread([&] {
            initialize_results[0] = rpc_call(pipe_name,
                {RequestType::InitializeOwnerStore, 1002, {}}).error;
        }),
        std::thread([&] {
            initialize_results[1] = rpc_call(pipe_name,
                {RequestType::InitializeOwnerStore, 1009, {}}).error;
        })};
    for (auto& initializer : initializers) initializer.join();
    require(initialize_results[0] == BrokerErrorCode::None &&
        initialize_results[1] == BrokerErrorCode::None,
        "Concurrent explicit store initialization did not serialize idempotently.");
    reply = rpc_call(pipe_name, {RequestType::QueryOwnership, 1010,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None &&
        reply.message.find("does not create a Job") == std::string::npos,
        "Store initialization result was not observable through owner RPC.");
    reply = rpc_call(pipe_name, {RequestType::QueryOwnership, 1003,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None &&
        reply.message.find("EmptyVerified") != std::string::npos,
        "Initialized store plus absent identity did not return live-owner EmptyVerified evidence.");

    reply = rpc_call(pipe_name, {RequestType::ReserveIdentity, 1004,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None &&
        reply.message.find("no process was launched") != std::string::npos,
        "The supervisor did not reserve the identity with a native Job handle.");
    const auto generation_start = reply.message.find("generation=");
    require(generation_start != std::string::npos,
        "Reservation reply omitted its runtime generation.");
    const auto generation_end = reply.message.find(';', generation_start);
    const auto runtime_generation = reply.message.substr(generation_start + 11,
        generation_end - generation_start - 11);
    require(runtime_generation.size() == 36,
        "Reservation returned an invalid runtime generation identifier.");

    reply = rpc_call(pipe_name, {RequestType::ReserveIdentity, 1005,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::Busy,
        "A duplicate identity reservation was not rejected deterministically.");
    reply = rpc_call(pipe_name, {RequestType::ReleaseReservation, 1011,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::InvalidState,
        "An unverified reservation release bypassed native empty-state validation.");

    reply = rpc_call(pipe_name, {RequestType::QueryOwnership, 1006,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None &&
        reply.message.find("EmptyVerified") != std::string::npos,
        "The supervisor could not query its own held empty Job after client disconnect.");

    reply = rpc_call(pipe_name, {RequestType::FinalizeEmpty, 1007,
        encode_generation_payload(identity, runtime_generation)});
    require(reply.error == BrokerErrorCode::None &&
        reply.message.find("native Job/root validation") != std::string::npos,
        "The supervisor did not finalize after native empty verification.");

    reply = rpc_call(pipe_name, {RequestType::ReserveIdentity, 1008,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None,
        "A new generation was not allowed after fresh native empty verification: " + reply.message);

    reply = rpc_call(pipe_name, {RequestType::ReserveIdentity, 1008,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::DuplicateRequestId,
        "A replayed supervisor request ID was not rejected.");
}

void test_concurrent_reservation_has_single_winner() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string error;
    const auto fixture_id = create_runtime_generation_id(error);
    require(!fixture_id.empty(), error);
    RemoveDirectoryTree cleanup(unique_test_root(fixture_id));
    SupervisorFixture supervisor(user.identity, cleanup.path());
    const auto pipe_name = supervisor_pipe_name_for_user(user.identity.sid);
    auto reply = rpc_call(pipe_name, {RequestType::InitializeOwnerStore, 2001, {}});
    require(reply.error == BrokerErrorCode::None, "Could not initialize concurrent test store.");
    const auto identity = identity_for(user.identity.sid, "concurrent");
    std::array<BrokerErrorCode, 2> results{BrokerErrorCode::InternalFailure,
        BrokerErrorCode::InternalFailure};
    std::array<std::thread, 2> clients{
        std::thread([&] {
            results[0] = rpc_call(pipe_name, {RequestType::ReserveIdentity, 2002,
                encode_identity_payload(identity)}).error;
        }),
        std::thread([&] {
            results[1] = rpc_call(pipe_name, {RequestType::ReserveIdentity, 2003,
                encode_identity_payload(identity)}).error;
        })};
    for (auto& client : clients) client.join();
    const auto successes = static_cast<int>(results[0] == BrokerErrorCode::None) +
        static_cast<int>(results[1] == BrokerErrorCode::None);
    const auto busy = static_cast<int>(results[0] == BrokerErrorCode::Busy) +
        static_cast<int>(results[1] == BrokerErrorCode::Busy);
    require(successes == 1 && busy == 1,
        "Concurrent clients did not produce exactly one reservation winner and one busy result.");
}

}

int wmain() {
    try {
        test_supervisor_rpc_bootstrap_reservation_and_reconnect();
        test_concurrent_reservation_has_single_winner();
    } catch (const std::exception& error) {
        std::cerr << "supervisor native test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "2 supervisor RPC test groups passed\n";
    return 0;
}
