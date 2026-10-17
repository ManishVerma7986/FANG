#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "broker_native_job.hpp"
#include "broker_pipe.hpp"
#include "broker_reference_runtime.hpp"
#include "broker_supervisor.hpp"
#include "supervisor_runtime_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
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

class ControlledMembership final : public IJobMembershipQuery {
public:
    JobMembershipResult query(INamedJobObject&,
        const RuntimeSessionMetadata&) override {
        ++query_count;
        return result;
    }

    JobMembershipResult result{JobMembershipResult::State::Failed,
        0, false, false, {}, "injected native membership failure"};
    int query_count{};
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

#ifndef FANG_SUPERVISOR_TEST_SEAM
#error "Supervisor integration tests require FANG_SUPERVISOR_TEST_SEAM."
#endif

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
        if (completion_.valid()) {
            const auto status = completion_.wait_for(std::chrono::seconds(5));
            require(status == std::future_status::ready,
                "The test supervisor did not stop within its bounded shutdown deadline.");
            const auto result = completion_.get();
            require(!result || result.native_error == ERROR_OPERATION_ABORTED ||
                result.native_error == ERROR_SUCCESS,
                "The test supervisor reported an unexpected shutdown error: " + result.message);
        }
    }

    BrokerPipeServer& server() noexcept { return server_; }
    OwnershipSupervisor& owner() noexcept { return owner_; }
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
    ~RemoveDirectoryTree() noexcept {
        // The root is unique to this test generation; leave the shared parent untouched.
        remove_matching_files(L"*.fjn");
        remove_matching_files(L"*.fjn.*");
        remove_matching_files(L"owner-index.fji.*");
        const auto index = path_ + L"\\owner-index.fji";
        DeleteFileW(index.c_str());
        RemoveDirectoryW(path_.c_str());
    }
    const std::wstring& path() const noexcept { return path_; }
private:
    void remove_matching_files(std::wstring_view file_pattern) noexcept {
        try {
            const auto pattern = path_ + L"\\" + std::wstring(file_pattern);
            WIN32_FIND_DATAW entry{};
            struct SearchHandle {
                HANDLE value;
                ~SearchHandle() noexcept {
                    if (value != INVALID_HANDLE_VALUE) FindClose(value);
                }
            } search{FindFirstFileW(pattern.c_str(), &entry)};
            if (search.value == INVALID_HANDLE_VALUE) return;
            do {
                if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    const auto file = path_ + L"\\" + entry.cFileName;
                    DeleteFileW(file.c_str());
                }
            } while (FindNextFileW(search.value, &entry) != 0);
        } catch (...) {
        }
    }

    std::wstring path_;
};

CanonicalAvdIdentity identity_for(std::string sid, std::string_view label) {
    return {std::move(sid), "C:\\FangSupervisorTest\\" + std::string(label)};
}

std::wstring journal_file_in(std::wstring_view root) {
    const std::wstring pattern = std::wstring(root) + L"\\*.fjn";
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    require(search != INVALID_HANDLE_VALUE,
        "Could not locate the reserved identity's durable journal.");
    const std::wstring path = std::wstring(root) + L"\\" + entry.cFileName;
    FindClose(search);
    return path;
}

void corrupt_file(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Could not open the durable journal for corruption.");
    const std::byte invalid{0xff};
    DWORD written{};
    const BOOL write_succeeded = WriteFile(file, &invalid, 1, &written, nullptr);
    const DWORD write_error = write_succeeded != 0 ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    require(write_succeeded != 0 && written == 1,
        "Could not corrupt the durable journal; Win32 error " + std::to_string(write_error));
}

void restrict_journal_to_system(const std::wstring& path) {
    PSECURITY_DESCRIPTOR descriptor{};
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr) != 0,
        "Could not create the SYSTEM-only journal test DACL.");
    BOOL present{};
    BOOL defaulted{};
    PACL dacl{};
    const bool extracted = GetSecurityDescriptorDacl(descriptor, &present,
        &dacl, &defaulted) != 0 && present && dacl != nullptr;
    require(extracted, "Could not extract the SYSTEM-only journal test DACL.");
    const DWORD result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    require(result == ERROR_SUCCESS, "Could not apply the SYSTEM-only journal test DACL.");
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

    BrokerPipeClient client(pipe_name, 1500);
    const auto status_call = client.call({RequestType::GetStatus, 1012, {}});
    require(!status_call.transport_error && status_call.reply.has_value() &&
        status_call.reply->error == BrokerErrorCode::None &&
        status_call.reply->state == LifecycleState::Ready,
        "The bounded supervisor pipe client did not round-trip a structured status reply.");

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

std::wstring helper_event_name(std::string_view generation, std::wstring_view suffix) {
    std::wstring result = L"Local\\Fang.SupervisorNativeTest.";
    result.append(generation.begin(), generation.end());
    result.push_back(L'.');
    result.append(suffix);
    return result;
}

std::string generation_from_reserve(const std::string& message) {
    constexpr std::string_view marker = "Reserved generation=";
    const auto begin = message.find(marker);
    require(begin != std::string::npos, "Reservation reply omitted the generation.");
    const auto value_begin = begin + marker.size();
    const auto value_end = message.find(';', value_begin);
    require(value_end != std::string::npos && value_end > value_begin,
        "Reservation reply has a malformed generation.");
    return message.substr(value_begin, value_end - value_begin);
}

class SupervisorHelperProcess {
public:
    SupervisorHelperProcess(HANDLE job, std::wstring ready_name, std::wstring stop_name)
        : ready_name_(std::move(ready_name)), stop_name_(std::move(stop_name)) {
        ready_event_.reset(CreateEventW(nullptr, TRUE, FALSE, ready_name_.c_str()));
        stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, stop_name_.c_str()));
        require(ready_event_ != nullptr && stop_event_ != nullptr,
            "Could not create the supervisor helper synchronization events.");
        std::array<wchar_t, 32768> module_path{};
        const DWORD path_length = GetModuleFileNameW(nullptr, module_path.data(),
            static_cast<DWORD>(module_path.size()));
        require(path_length > 0 && path_length < module_path.size(),
            "Could not determine the supervisor test executable path.");
        std::wstring command = L"\"" + std::wstring(module_path.data(), path_length) +
            L"\" --helper \"" + ready_name_ + L"\" \"" + stop_name_ + L"\"";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        require(CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                DETACHED_PROCESS | CREATE_SUSPENDED, nullptr, nullptr, &startup,
                &information) != 0,
            "Could not create the suspended supervisor helper.");
        process_.reset(information.hProcess);
        thread_.reset(information.hThread);
        process_id_ = information.dwProcessId;
        try {
            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            require(GetProcessTimes(process_.get(), &created, &exited, &kernel, &user) != 0,
                "Could not capture the supervisor helper creation time.");
            ULARGE_INTEGER timestamp{};
            timestamp.LowPart = created.dwLowDateTime;
            timestamp.HighPart = created.dwHighDateTime;
            creation_time_ = timestamp.QuadPart;
            require(AssignProcessToJobObject(job, process_.get()) != 0,
                "Could not assign the supervisor helper to the owned Job.");
            require(ResumeThread(thread_.get()) != static_cast<DWORD>(-1),
                "Could not resume the supervisor helper.");
            thread_resumed_ = true;
            require(WaitForSingleObject(ready_event_.get(), 5000) == WAIT_OBJECT_0,
                "The supervisor helper did not signal readiness.");
            thread_.reset();
        } catch (...) {
            try {
                if (!stop_and_wait()) report_cleanup_failure();
            } catch (...) {
                OutputDebugStringA("Supervisor helper cleanup threw during setup failure.\n");
            }
            throw;
        }
    }

    SupervisorHelperProcess(const SupervisorHelperProcess&) = delete;
    SupervisorHelperProcess& operator=(const SupervisorHelperProcess&) = delete;

    ~SupervisorHelperProcess() noexcept {
        try {
            if (!stop_and_wait()) report_cleanup_failure();
        } catch (...) {
            OutputDebugStringA("Supervisor helper cleanup threw during destruction.\n");
        }
    }

    std::uint32_t process_id() const noexcept { return process_id_; }
    std::uint64_t creation_time() const noexcept { return creation_time_; }
    std::string_view cleanup_error() const noexcept { return cleanup_error_; }

    bool stop_and_wait(DWORD timeout_ms = 5000) {
        cleanup_error_.clear();
        if (process_.get() == nullptr) return true;
        const DWORD initial_wait = WaitForSingleObject(process_.get(), 0);
        if (initial_wait == WAIT_OBJECT_0) return true;
        if (initial_wait == WAIT_FAILED) {
            remember_cleanup_error("Could not inspect helper process before shutdown; Win32 error " +
                std::to_string(GetLastError()));
        }
        if (stop_event_ == nullptr || SetEvent(stop_event_.get()) == 0) {
            const DWORD native_error = stop_event_ == nullptr ? ERROR_INVALID_HANDLE : GetLastError();
            remember_cleanup_error("Could not signal helper shutdown; Win32 error " +
                std::to_string(native_error));
        }
        if (thread_.get() != nullptr && !thread_resumed_) {
            if (ResumeThread(thread_.get()) == static_cast<DWORD>(-1)) {
                remember_cleanup_error("Could not resume suspended helper for graceful shutdown; "
                    "Win32 error " + std::to_string(GetLastError()));
            } else {
                thread_resumed_ = true;
            }
        }
        const DWORD wait = WaitForSingleObject(process_.get(), timeout_ms);
        if (wait == WAIT_OBJECT_0) return true;
        if (wait == WAIT_TIMEOUT) {
            remember_cleanup_error("Timed out waiting for helper process " +
                std::to_string(process_id_) + " after " + std::to_string(timeout_ms) + " ms.");
        } else if (wait == WAIT_FAILED) {
            remember_cleanup_error("Could not wait for helper process " +
                std::to_string(process_id_) + "; Win32 error " + std::to_string(GetLastError()));
        } else {
            remember_cleanup_error("Unexpected wait result while stopping helper process " +
                std::to_string(process_id_) + ".");
        }
        return false;
    }

private:
    void remember_cleanup_error(std::string message) {
        if (!cleanup_error_.empty()) cleanup_error_ += "; ";
        cleanup_error_ += std::move(message);
    }

    void report_cleanup_failure() const noexcept {
        try {
            std::cerr << "Supervisor helper cleanup failed for PID " << process_id_ << ": "
                << (cleanup_error_.empty() ? "unknown error" : cleanup_error_) << '\n';
        } catch (...) {
            OutputDebugStringA("Supervisor helper cleanup failed; see test process diagnostics.\n");
        }
    }

    struct HandleCloser {
        void operator()(void* value) const noexcept {
            auto handle = static_cast<HANDLE>(value);
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
    };

    struct EventCloser {
        void operator()(HANDLE value) const noexcept {
            if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        }
    };

    std::wstring ready_name_;
    std::wstring stop_name_;
    std::unique_ptr<std::remove_pointer<HANDLE>::type, EventCloser> ready_event_;
    std::unique_ptr<std::remove_pointer<HANDLE>::type, EventCloser> stop_event_;
    std::unique_ptr<std::remove_pointer<HANDLE>::type, HandleCloser> process_;
    std::unique_ptr<std::remove_pointer<HANDLE>::type, HandleCloser> thread_;
    std::string cleanup_error_;
    std::uint32_t process_id_{};
    std::uint64_t creation_time_{};
    bool thread_resumed_{};
};

void test_helper_lifetime_survives_client_disconnect() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string error;
    const auto fixture_id = create_runtime_generation_id(error);
    require(!fixture_id.empty(), error);
    RemoveDirectoryTree cleanup(unique_test_root(fixture_id));
    SupervisorFixture supervisor(user.identity, cleanup.path());
    const auto pipe_name = supervisor_pipe_name_for_user(user.identity.sid);
    auto reply = rpc_call(pipe_name, {RequestType::InitializeOwnerStore, 3001, {}});
    require(reply.error == BrokerErrorCode::None, "Could not initialize helper test store.");
    const auto identity = identity_for(user.identity.sid, "helper-lifetime");
    reply = rpc_call(pipe_name, {RequestType::ReserveIdentity, 3002,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None,
        "Could not reserve helper identity: " + reply.message);
    const auto generation = generation_from_reserve(reply.message);
    void* owned_job = supervisor.owner().test_job_handle_for(identity.key());
    require(owned_job != nullptr, "The supervisor did not retain the reserved Job handle.");
    SupervisorHelperProcess helper(static_cast<HANDLE>(owned_job),
        helper_event_name(fixture_id, L"ready"), helper_event_name(fixture_id, L"stop"));
    require(helper.process_id() != 0 && helper.creation_time() != 0,
        "The helper identity is incomplete.");
    reply = rpc_call(pipe_name, {RequestType::QueryOwnership, 3003,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None &&
        reply.message.find("RuntimeMayBeActive") != std::string::npos,
        "The supervisor did not report live helper as RuntimeMayBeActive: " + reply.message);
    reply = rpc_call(pipe_name, {RequestType::ReserveIdentity, 3004,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::Busy,
        "A second runtime was not rejected while the helper is live.");
    reply = rpc_call(pipe_name, {RequestType::FinalizeEmpty, 3005,
        encode_generation_payload(identity, generation)});
    require(reply.error == BrokerErrorCode::RecoveryRequired,
        "Finalize bypassed native verification while the helper is live.");
    std::string shutdown_error;
    require(!supervisor.owner().shutdown_if_empty(shutdown_error) &&
        !shutdown_error.empty() &&
        supervisor.owner().lifecycle() == SupervisorLifecycle::Ready,
        "A live helper was mistaken for an empty runtime during supervisor shutdown.");
    require(helper.stop_and_wait(), "The helper did not exit normally on stop signal: " +
        std::string(helper.cleanup_error()));
    reply = rpc_call(pipe_name, {RequestType::FinalizeEmpty, 3006,
        encode_generation_payload(identity, generation)});
    require(reply.error == BrokerErrorCode::None,
        "Empty was not finalized after normal helper exit: " + reply.message);
    reply = rpc_call(pipe_name, {RequestType::QueryOwnership, 3007,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None &&
        reply.message.find("EmptyVerified") != std::string::npos,
        "The supervisor did not report EmptyVerified after helper exit: " + reply.message);
    shutdown_error = "stale caller error";
    require(supervisor.owner().shutdown_if_empty(shutdown_error),
        "Fresh native-backed empty evidence did not permit supervisor shutdown: " +
            shutdown_error);
    require(shutdown_error.empty() &&
        supervisor.owner().lifecycle() == SupervisorLifecycle::ShuttingDown,
        "Successful verified-empty shutdown did not enter ShuttingDown.");
}

void test_invalid_transitions_and_duplicate_ids() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string error;
    const auto fixture_id = create_runtime_generation_id(error);
    require(!fixture_id.empty(), error);
    RemoveDirectoryTree cleanup(unique_test_root(fixture_id));
    SupervisorFixture supervisor(user.identity, cleanup.path());
    const auto pipe_name = supervisor_pipe_name_for_user(user.identity.sid);
    auto reply = rpc_call(pipe_name, {RequestType::InitializeOwnerStore, 4001, {}});
    require(reply.error == BrokerErrorCode::None, "Could not initialize transition store.");
    const auto identity = identity_for(user.identity.sid, "transitions");
    reply = rpc_call(pipe_name, {RequestType::FinalizeEmpty, 4002,
        encode_generation_payload(identity, "00000000-0000-4000-8000-000000000000")});
    require(reply.error == BrokerErrorCode::RecoveryRequired,
        "Finalize without a reservation was not rejected.");
    reply = rpc_call(pipe_name, {RequestType::ReleaseReservation, 4003,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::InvalidState,
        "ReleaseReservation was not rejected as an illegal transition.");
    reply = rpc_call(pipe_name, {RequestType::ShutdownOwner, 4004, {}});
    require(reply.error == BrokerErrorCode::InvalidState,
        "ShutdownOwner over client RPC was not rejected.");
    reply = rpc_call(pipe_name, {RequestType::ReserveIdentity, 4005,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None, "Could not reserve transition identity.");
    const auto generation = generation_from_reserve(reply.message);
    reply = rpc_call(pipe_name, {RequestType::QueryOwnership, 4005,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::DuplicateRequestId,
        "A duplicate request ID was not rejected.");
    reply = rpc_call(pipe_name, {RequestType::ReserveIdentity, 4006,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::Busy,
        "A duplicate reservation was not rejected as busy.");
    reply = rpc_call(pipe_name, {RequestType::FinalizeEmpty, 4007,
        encode_generation_payload(identity, generation)});
    require(reply.error == BrokerErrorCode::None, "Could not finalize transition identity.");
}

void test_shutdown_fails_closed_on_stale_or_unreadable_durable_state() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string error;
    const auto fixture_id = create_runtime_generation_id(error);
    require(!fixture_id.empty(), error);
    RemoveDirectoryTree cleanup(unique_test_root(fixture_id));
    SupervisorFixture supervisor(user.identity, cleanup.path());
    const auto pipe_name = supervisor_pipe_name_for_user(user.identity.sid);
    auto reply = rpc_call(pipe_name, {RequestType::InitializeOwnerStore, 6001, {}});
    require(reply.error == BrokerErrorCode::None, "Could not initialize shutdown test store.");
    const auto identity = identity_for(user.identity.sid, "shutdown-durable-state");
    reply = rpc_call(pipe_name, {RequestType::ReserveIdentity, 6002,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None,
        "Could not reserve shutdown test identity: " + reply.message);
    const auto generation = generation_from_reserve(reply.message);

    std::string mismatched_generation = create_runtime_generation_id(error);
    require(!mismatched_generation.empty(), error);
    require(mismatched_generation != generation,
        "The generated mismatch test generation unexpectedly matched the reservation.");
    require(supervisor.owner().test_override_expected_generation(
        identity.key(), mismatched_generation),
        "Could not install the stale in-memory generation test condition.");
    std::string shutdown_error;
    require(!supervisor.owner().shutdown_if_empty(shutdown_error) &&
        !shutdown_error.empty() &&
        supervisor.owner().lifecycle() == SupervisorLifecycle::Ready,
        "A durable record with a generation different from the in-memory expectation "
        "authorized shutdown.");

    require(supervisor.owner().test_override_expected_generation(identity.key(), generation),
        "Could not restore the expected generation after the mismatch test.");
    const auto journal_path = journal_file_in(cleanup.path());
    corrupt_file(journal_path);
    shutdown_error.clear();
    require(!supervisor.owner().shutdown_if_empty(shutdown_error) &&
        !shutdown_error.empty() &&
        supervisor.owner().lifecycle() == SupervisorLifecycle::Ready,
        "A corrupt durable journal authorized shutdown or changed the lifecycle.");

    require(DeleteFileW(journal_path.c_str()) != 0,
        "Could not remove the durable journal for the missing-record test.");
    shutdown_error.clear();
    require(!supervisor.owner().shutdown_if_empty(shutdown_error) &&
        !shutdown_error.empty() &&
        supervisor.owner().lifecycle() == SupervisorLifecycle::Ready,
        "A missing durable journal authorized shutdown or changed the lifecycle.");

    std::wstring job_name;
    require(expected_job_object_name(identity, generation, job_name, error),
        "Could not derive the expected native Job name to restore the journal: " + error);
    RuntimeSessionMetadata metadata{identity, generation, job_name,
        RuntimeLaunchPhase::JobCreated, 0, 0, std::string(supervisor.owner_generation()),
        user.identity.session_id, GetCurrentProcessId()};
    NativeOwnershipJournal journal(user.identity.sid, user.identity.session_id,
        GetCurrentProcessId(), std::string(supervisor.owner_generation()), cleanup.path());
    error.clear();
    require(journal.write_pending(metadata, error),
        "Could not restore the journal for the inaccessible-record test: " + error);
    restrict_journal_to_system(journal_path);
    shutdown_error.clear();
    require(!supervisor.owner().shutdown_if_empty(shutdown_error) &&
        !shutdown_error.empty() &&
        supervisor.owner().lifecycle() == SupervisorLifecycle::Ready,
        "An inaccessible durable journal authorized shutdown or changed the lifecycle.");
    require(DeleteFileW(journal_path.c_str()) != 0,
        "Could not remove the isolated inaccessible-journal fixture.");
}

void test_shutdown_rechecks_empty_owner_store_without_owned_generations() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string error;
    const auto fixture_id = create_runtime_generation_id(error);
    require(!fixture_id.empty(), error);
    RemoveDirectoryTree cleanup(unique_test_root(fixture_id));
    SupervisorFixture supervisor(user.identity, cleanup.path());

    std::string shutdown_error;
    require(!supervisor.owner().shutdown_if_empty(shutdown_error) &&
        !shutdown_error.empty() &&
        supervisor.owner().lifecycle() == SupervisorLifecycle::StoreUninitialized,
        "Shutdown without a verified owner store was allowed or changed lifecycle.");

    const auto pipe_name = supervisor_pipe_name_for_user(user.identity.sid);
    const auto initialized = rpc_call(pipe_name,
        {RequestType::InitializeOwnerStore, 7001, {}});
    require(initialized.error == BrokerErrorCode::None,
        "Could not initialize the empty-store shutdown fixture.");
    shutdown_error.clear();
    require(supervisor.owner().shutdown_if_empty(shutdown_error),
        "Freshly reloaded empty owner store did not permit shutdown: " + shutdown_error);
    require(supervisor.owner().lifecycle() == SupervisorLifecycle::ShuttingDown,
        "Empty-store shutdown did not enter ShuttingDown.");
}

void test_native_query_failures_preserve_supervisor_ownership_state() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string error;
    const auto owner_generation = create_runtime_generation_id(error);
    require(!owner_generation.empty(), error);
    RemoveDirectoryTree cleanup(unique_test_root(owner_generation));
    ControlledMembership membership;
    OwnershipSupervisor owner(user.identity.sid, user.identity.session_id,
        GetCurrentProcessId(), owner_generation, cleanup.path(), membership);
    NativeOwnershipJournal durable(user.identity.sid, user.identity.session_id,
        GetCurrentProcessId(), owner_generation, cleanup.path());
    const auto identity = identity_for(user.identity.sid, "query-failure-finalize");

    auto reply = owner.dispatch({RequestType::InitializeOwnerStore, 8001, {}});
    require(reply.error == BrokerErrorCode::None,
        "Could not initialize injected-query ownership store.");
    reply = owner.dispatch({RequestType::ReserveIdentity, 8002,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None,
        "Could not reserve injected-query test identity: " + reply.message);
    const auto generation = generation_from_reserve(reply.message);

    const auto require_durable_record = [&](std::string_view expected_generation,
            RuntimeLaunchPhase expected_phase, OwnershipIndexEntryState expected_index) {
        const auto record = durable.load(identity);
        require(record.state == JournalLoadState::Record && record.record.has_value() &&
            record.record->generation_id == expected_generation &&
            record.record->phase == expected_phase,
            "Durable runtime record mismatch: state=" +
                std::to_string(static_cast<unsigned int>(record.state)) +
                ", generation=" + (record.record.has_value()
                    ? record.record->generation_id : "<missing>") +
                ", phase=" + (record.record.has_value()
                    ? std::to_string(static_cast<unsigned int>(record.record->phase))
                    : "<missing>") + ", expected generation=" +
                std::string(expected_generation) + ", expected phase=" +
                std::to_string(static_cast<unsigned int>(expected_phase)) +
                (record.error.empty() ? "" : ", error=" + record.error));
        const auto store = durable.load_store();
        const auto indexed = std::find_if(store.entries.begin(), store.entries.end(),
            [&](const auto& entry) { return entry.identity.key() == identity.key(); });
        require(store.state == OwnershipStoreState::Initialized &&
            indexed != store.entries.end() &&
            indexed->runtime_generation_id == expected_generation &&
            indexed->state == expected_index,
            "A rejected native query changed the durable ownership index.");
    };

    for (const auto& failed_query : {
            JobMembershipResult{JobMembershipResult::State::Failed,
                0, false, false, {}, "injected membership query failure"},
            JobMembershipResult{JobMembershipResult::State::AccessDenied,
                0, false, false, {}, "injected membership access denial"},
            JobMembershipResult{JobMembershipResult::State::Verified,
                0, false, false, {}, "injected contradictory query error"}}) {
        membership.result = failed_query;
        reply = owner.dispatch({RequestType::FinalizeEmpty, 8010 +
                static_cast<std::uint32_t>(membership.query_count),
            encode_generation_payload(identity, generation)});
        require(reply.error == BrokerErrorCode::RecoveryRequired &&
            owner.lifecycle() == SupervisorLifecycle::Ready,
            "FinalizeEmpty accepted inconclusive native evidence or changed lifecycle.");
        require_durable_record(generation, RuntimeLaunchPhase::JobCreated,
            OwnershipIndexEntryState::Owned);

        std::string shutdown_error;
        require(!owner.shutdown_if_empty(shutdown_error) && !shutdown_error.empty() &&
            owner.lifecycle() == SupervisorLifecycle::Ready,
            "shutdown_if_empty entered ShuttingDown without verified native emptiness.");
        require_durable_record(generation, RuntimeLaunchPhase::JobCreated,
            OwnershipIndexEntryState::Owned);
    }

    membership.result = {JobMembershipResult::State::Verified, 0, false, false, {}, {}};
    reply = owner.dispatch({RequestType::FinalizeEmpty, 8020,
        encode_generation_payload(identity, generation)});
    require(reply.error == BrokerErrorCode::None,
        "Valid positively verified empty evidence did not permit finalization: " + reply.message);
    require_durable_record(generation, RuntimeLaunchPhase::JobCreated,
        OwnershipIndexEntryState::EmptyVerified);
    reply = owner.dispatch({RequestType::ReserveIdentity, 8021,
        encode_identity_payload(identity)});
    require(reply.error == BrokerErrorCode::None,
        "A fresh reservation was not allowed after verified-empty finalization: " + reply.message);
    const auto replacement_generation = generation_from_reserve(reply.message);
    require(replacement_generation != generation,
        "Verified-empty recovery reused the prior runtime generation.");

    const auto ambiguous_identity = identity_for(user.identity.sid,
        "query-failure-replacement");
    reply = owner.dispatch({RequestType::ReserveIdentity, 8022,
        encode_identity_payload(ambiguous_identity)});
    require(reply.error == BrokerErrorCode::None,
        "Could not reserve ambiguous-replacement fixture: " + reply.message);
    const auto ambiguous_generation = generation_from_reserve(reply.message);
    reply = owner.dispatch({RequestType::FinalizeEmpty, 8023,
        encode_generation_payload(ambiguous_identity, ambiguous_generation)});
    require(reply.error == BrokerErrorCode::None,
        "Could not create a trusted empty record for replacement validation.");
    membership.result = {JobMembershipResult::State::Failed,
        0, false, false, {}, "injected query failure after durable empty marker"};
    reply = owner.dispatch({RequestType::ReserveIdentity, 8024,
        encode_identity_payload(ambiguous_identity)});
    require(reply.error == BrokerErrorCode::RecoveryRequired &&
        owner.lifecycle() == SupervisorLifecycle::RecoveryRequired,
        "A new reservation replaced an identity whose fresh native state was ambiguous.");
    const auto ambiguous_record = durable.load(ambiguous_identity);
    const auto ambiguous_store = durable.load_store();
    const auto ambiguous_index = std::find_if(ambiguous_store.entries.begin(),
        ambiguous_store.entries.end(), [&](const auto& entry) {
            return entry.identity.key() == ambiguous_identity.key();
        });
    require(ambiguous_record.state == JournalLoadState::Record &&
        ambiguous_record.record.has_value() &&
        ambiguous_record.record->generation_id == ambiguous_generation &&
        ambiguous_record.record->phase == RuntimeLaunchPhase::JobCreated &&
        ambiguous_store.state == OwnershipStoreState::Initialized &&
        ambiguous_index != ambiguous_store.entries.end() &&
        ambiguous_index->runtime_generation_id == ambiguous_generation &&
        ambiguous_index->state == OwnershipIndexEntryState::EmptyVerified,
        "A rejected replacement converted or replaced the durable verified-empty record.");
}

void test_reference_config_rejects_path_injection() {
    std::string error;
    require(!resolve_reference_runtime_config("..\\other-avd", error).has_value() &&
        !error.empty(),
        "The reference runtime configuration accepted an AVD name containing a path separator.");
    error.clear();
    require(!resolve_reference_runtime_config("FANG/other", error).has_value() &&
        !error.empty(),
        "The reference runtime configuration accepted a slash in the AVD name.");
}

void test_runtime_rpc_rejects_malformed_start_and_unknown_generation() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string error;
    const auto owner_generation = create_runtime_generation_id(error);
    require(!owner_generation.empty(), error);
    RemoveDirectoryTree cleanup(unique_test_root(owner_generation));
    ControlledMembership membership;
    OwnershipSupervisor owner(user.identity.sid, user.identity.session_id,
        GetCurrentProcessId(), owner_generation, cleanup.path(), membership);

    std::vector<std::byte> malformed_start{
        std::byte{1}, std::byte{0}, std::byte{3}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{'.'}, std::byte{'.'}, std::byte{'\\'}};
    const auto invalid_start = owner.dispatch({RequestType::StartReferenceRuntime,
        9101, std::move(malformed_start)});
    const auto invalid_start_data = decode_runtime_status_data(invalid_start.data);
    require(invalid_start.error == BrokerErrorCode::InvalidRuntimeConfiguration &&
        invalid_start_data.has_value() &&
        invalid_start_data->state == RuntimeLifecycleState::Failed &&
        invalid_start_data->failure_stage == RuntimeFailureStage::ValidateConfiguration &&
        owner.lifecycle() == SupervisorLifecycle::StoreUninitialized,
        "Malformed start configuration was not rejected with structured failure before resource access.");

    const RuntimeGenerationRequest unknown_generation{
        "01234567-89ab-4cde-8fab-0123456789ab"};
    const auto unknown_payload = encode_runtime_generation_request(unknown_generation);
    const auto status = owner.dispatch({RequestType::QueryRuntimeStatus, 9102, unknown_payload});
    const auto status_data = decode_runtime_status_data(status.data);
    require(status.error == BrokerErrorCode::RuntimeNotFound && status_data.has_value() &&
        status_data->state == RuntimeLifecycleState::Failed &&
        status_data->generation_id == unknown_generation.generation_id,
        "Unknown runtime status did not return a structured not-found result.");
    const auto shutdown = owner.dispatch({RequestType::ShutdownRuntime, 9103, unknown_payload});
    require(shutdown.error == BrokerErrorCode::RuntimeNotFound &&
        decode_runtime_status_data(shutdown.data).has_value(),
        "Shutdown for an unknown runtime was not rejected deterministically.");
}

void test_async_studio_client_reports_invalid_avd_without_starting_guest() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string error;
    const auto fixture_id = create_runtime_generation_id(error);
    require(!fixture_id.empty(), error);
    RemoveDirectoryTree cleanup(unique_test_root(fixture_id));
    SupervisorFixture supervisor(user.identity, cleanup.path());
    fang::studio::SupervisorRuntimeClient client(
        supervisor_pipe_name_for_user(user.identity.sid),
        L"unused-while-supervisor-is-running.exe");

    require(client.start("../not-an-avd"),
        "The asynchronous client did not queue its invalid-input test request.");
    const ULONGLONG deadline = GetTickCount64() + 5000;
    auto snapshot = client.snapshot();
    while (snapshot.request_pending && GetTickCount64() < deadline) {
        Sleep(10);
        snapshot = client.snapshot();
    }
    require(!snapshot.request_pending &&
        snapshot.error == BrokerErrorCode::InvalidRuntimeConfiguration &&
        snapshot.runtime.state == RuntimeLifecycleState::Failed &&
        snapshot.runtime.failure_stage == RuntimeFailureStage::ValidateConfiguration &&
        snapshot.runtime.generation_id.empty(),
        "The Studio client did not report structured invalid-AVD failure without creating a generation.");
}

void test_concurrent_initialization_is_serialized() {
    const auto user = current_user_identity();
    require(user.succeeded, "Could not inspect current Windows identity.");
    std::string error;
    const auto fixture_id = create_runtime_generation_id(error);
    require(!fixture_id.empty(), error);
    RemoveDirectoryTree cleanup(unique_test_root(fixture_id));
    SupervisorFixture supervisor(user.identity, cleanup.path());
    const auto pipe_name = supervisor_pipe_name_for_user(user.identity.sid);
    std::array<BrokerErrorCode, 4> results{BrokerErrorCode::InternalFailure,
        BrokerErrorCode::InternalFailure, BrokerErrorCode::InternalFailure,
        BrokerErrorCode::InternalFailure};
    std::array<std::thread, 4> initializers{
        std::thread([&] {
            results[0] = rpc_call(pipe_name, {RequestType::InitializeOwnerStore, 5001, {}}).error;
        }),
        std::thread([&] {
            results[1] = rpc_call(pipe_name, {RequestType::InitializeOwnerStore, 5002, {}}).error;
        }),
        std::thread([&] {
            results[2] = rpc_call(pipe_name, {RequestType::InitializeOwnerStore, 5003, {}}).error;
        }),
        std::thread([&] {
            results[3] = rpc_call(pipe_name, {RequestType::InitializeOwnerStore, 5004, {}}).error;
        })};
    for (auto& initializer : initializers) initializer.join();
    for (const auto result : results) {
        require(result == BrokerErrorCode::None,
            "Concurrent initialization did not serialize to success.");
    }
    auto replay = rpc_call(pipe_name, {RequestType::InitializeOwnerStore, 5001, {}});
    require(replay.error == BrokerErrorCode::DuplicateRequestId,
        "A replayed initialization request ID was not rejected.");
}
void test_concurrent_initialization_is_serialized();

void test_reference_emulator_arguments_and_port_validation() {
    // 1. Port validity tests
    require(valid_emulator_port(5554), "5554 must be valid.");
    require(valid_emulator_port(5556), "5556 must be valid.");
    require(valid_emulator_port(5584), "5584 must be valid.");
    require(!valid_emulator_port(0), "Port 0 must be invalid.");
    require(!valid_emulator_port(5555), "Odd port 5555 must be invalid.");
    require(!valid_emulator_port(80), "Privileged port 80 must be invalid.");
    require(!valid_emulator_port(65535), "Odd port 65535 must be invalid.");

    // 2. Reference emulator argument construction
    ReferenceRuntimeConfig config;
    config.avd_name = "Pixel_7_API_36";
    config.emulator_port = 5556;
    config.graphics_mode = "swiftshader_indirect";

    const auto args = reference_emulator_arguments(config);

    auto has_arg = [&](std::wstring_view expected) {
        return std::find(args.begin(), args.end(), expected) != args.end();
    };
    auto arg_after = [&](std::wstring_view flag) -> std::wstring_view {
        auto it = std::find(args.begin(), args.end(), flag);
        if (it != args.end() && std::next(it) != args.end()) {
            return *std::next(it);
        }
        return {};
    };

    require(has_arg(L"-avd"), "Must contain -avd");
    require(arg_after(L"-avd") == L"Pixel_7_API_36", "-avd must be Pixel_7_API_36");
    require(has_arg(L"-no-snapshot"), "Must contain -no-snapshot");
    require(has_arg(L"-no-audio"), "Must contain -no-audio");
    require(has_arg(L"-no-boot-anim"), "Must contain -no-boot-anim");
    require(has_arg(L"-port"), "Must contain -port");
    require(arg_after(L"-port") == L"5556", "-port must be 5556");
    require(has_arg(L"-share-vid"), "Must contain -share-vid");
    require(has_arg(L"-gpu"), "Must contain -gpu");
    require(arg_after(L"-gpu") == L"swiftshader_indirect", "-gpu must be swiftshader_indirect");

    // 3. Port fidelity in arguments (no silent fallback to 5554)
    config.emulator_port = 5555; // odd port
    config.graphics_mode.clear(); // no graphics mode
    const auto odd_args = reference_emulator_arguments(config);
    auto odd_arg_after = [&](std::wstring_view flag) -> std::wstring_view {
        auto it = std::find(odd_args.begin(), odd_args.end(), flag);
        if (it != odd_args.end() && std::next(it) != odd_args.end()) {
            return *std::next(it);
        }
        return {};
    };
    require(odd_arg_after(L"-port") == L"5555", "Arguments must preserve configured port directly");
    require(std::find(odd_args.begin(), odd_args.end(), L"-share-vid") != odd_args.end(),
        "Must still contain -share-vid");
    require(std::find(odd_args.begin(), odd_args.end(), L"-gpu") == odd_args.end(),
        "Must omit -gpu when graphics_mode is empty");

    // 4. ReferenceRuntimeProcess::create_suspended rejects invalid port immediately
    struct DummyJob final : INamedJobObject {};
    DummyJob dummy_job;
    std::string create_error;
    auto failed_proc = ReferenceRuntimeProcess::create_suspended(config, dummy_job, create_error);
    require(failed_proc == nullptr, "create_suspended must fail on invalid port 5555");
    require(create_error.find("invalid") != std::string::npos,
        "create_suspended error must describe invalid port: " + create_error);
}

int supervisor_helper_main(const wchar_t* ready_name, const wchar_t* stop_name) {
    ScopedHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name));
    ScopedHandle stop(OpenEventW(SYNCHRONIZE, FALSE, stop_name));
    if (ready.get() == nullptr || stop.get() == nullptr || SetEvent(ready.get()) == 0) {
        return 2;
    }
    return WaitForSingleObject(stop.get(), 30000) == WAIT_OBJECT_0 ? 0 : 3;
}

}

#ifdef FANG_SUPERVISOR_TEST_SEAM
fang::broker::OwnershipSupervisor::OwnershipSupervisor(std::string user_sid,
        std::uint32_t session_id, std::uint32_t process_id,
        std::string owner_generation_id, std::wstring storage_root,
        fang::broker::IJobMembershipQuery& membership)
    : user_sid_(std::move(user_sid)), session_id_(session_id), process_id_(process_id),
      owner_generation_id_(std::move(owner_generation_id)), request_ids_(4096),
      journal_(user_sid_, session_id_, process_id_, owner_generation_id_,
          std::move(storage_root)), jobs_(user_sid_), native_membership_(user_sid_),
      membership_(membership) {
    const auto store = journal_.load_store();
    const bool all_empty = store.state == fang::broker::OwnershipStoreState::Initialized &&
        std::all_of(store.entries.begin(), store.entries.end(),
            [](const auto& entry) { return entry.state == fang::broker::OwnershipIndexEntryState::EmptyVerified; });
    if (store.state == fang::broker::OwnershipStoreState::Missing) {
        lifecycle_ = fang::broker::SupervisorLifecycle::StoreUninitialized;
    } else if (all_empty) {
        lifecycle_ = fang::broker::SupervisorLifecycle::Ready;
    } else {
        lifecycle_ = fang::broker::SupervisorLifecycle::RecoveryRequired;
    }
}
#endif

void* fang::broker::OwnershipSupervisor::test_job_handle_for(
        std::string_view identity_key) const {
    std::lock_guard lock(mutex_);
    const auto held = owned_generations_.find(std::string(identity_key));
    if (held == owned_generations_.end() || held->second.job == nullptr) {
        return nullptr;
    }
    auto* native = dynamic_cast<fang::broker::NativeJobObject*>(held->second.job.get());
    return native == nullptr ? nullptr : native->native_handle();
}

bool fang::broker::OwnershipSupervisor::test_override_expected_generation(
        std::string_view identity_key, std::string_view generation_id) {
    std::lock_guard lock(mutex_);
    const auto held = owned_generations_.find(std::string(identity_key));
    if (held == owned_generations_.end()) {
        return false;
    }
    held->second.metadata.generation_id.assign(generation_id);
    return true;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--helper") {
        return supervisor_helper_main(argv[2], argv[3]);
    }
    try {
        test_supervisor_rpc_bootstrap_reservation_and_reconnect();
        test_concurrent_reservation_has_single_winner();
        test_helper_lifetime_survives_client_disconnect();
        test_invalid_transitions_and_duplicate_ids();
        test_concurrent_initialization_is_serialized();
        test_shutdown_fails_closed_on_stale_or_unreadable_durable_state();
        test_shutdown_rechecks_empty_owner_store_without_owned_generations();
        test_native_query_failures_preserve_supervisor_ownership_state();
        test_reference_config_rejects_path_injection();
        test_runtime_rpc_rejects_malformed_start_and_unknown_generation();
        test_async_studio_client_reports_invalid_avd_without_starting_guest();
        test_reference_emulator_arguments_and_port_validation();
    } catch (const std::exception& error) {
        std::cerr << "supervisor native test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "12 supervisor RPC test groups passed\n";
    return 0;
}
