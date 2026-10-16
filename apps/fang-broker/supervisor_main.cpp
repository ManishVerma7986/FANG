#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>

#include "broker_native_job.hpp"
#include "broker_pipe.hpp"
#include "broker_supervisor.hpp"

#include <iostream>
#include <string>

namespace {

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_{};
};

class LocalMemory {
public:
    explicit LocalMemory(void* value = nullptr) noexcept : value_(value) {}
    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;
    ~LocalMemory() { if (value_ != nullptr) LocalFree(value_); }
private:
    void* value_{};
};

int fail(std::string_view message, DWORD native_error = ERROR_SUCCESS) {
    std::cerr << "supervisor_error win32=" << native_error << " " << message << '\n';
    return 1;
}

}

int wmain() {
    using namespace fang::broker;

    const auto identity = current_user_identity();
    if (!identity.succeeded) return fail(identity.error.message, identity.error.native_error);
    std::string generation_error;
    const auto owner_generation = create_runtime_generation_id(generation_error);
    if (owner_generation.empty()) return fail(generation_error);
    const auto pipe_name = supervisor_pipe_name_for_user(identity.identity.sid);
    if (pipe_name.empty()) return fail("Could not construct the per-user supervisor endpoint.");

    const std::wstring sid(identity.identity.sid.begin(), identity.identity.sid.end());
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;0x00100001;;;" + sid + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        return fail("Could not build the supervisor singleton DACL.", GetLastError());
    }
    LocalMemory descriptor_memory(descriptor);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const std::wstring singleton_name = L"Global\\Fang.RuntimeSupervisor.Owner." + sid;
    ScopedHandle singleton(CreateMutexW(&security, FALSE, singleton_name.c_str()));
    if (singleton.get() == nullptr) {
        return fail("Could not create the per-user supervisor singleton.", GetLastError());
    }
    const DWORD mutex_wait = WaitForSingleObject(singleton.get(), 0);
    if (mutex_wait == WAIT_TIMEOUT) {
        return fail("Another per-user supervisor already owns the singleton.");
    }
    if (mutex_wait != WAIT_OBJECT_0 && mutex_wait != WAIT_ABANDONED) {
        return fail("Could not acquire the per-user supervisor singleton.", GetLastError());
    }

    BrokerLifecycle transport_lifecycle;
    OwnershipSupervisor supervisor(identity.identity.sid, identity.identity.session_id,
        GetCurrentProcessId(), owner_generation);
    BrokerPipeServer server(pipe_name, identity.identity.sid, transport_lifecycle,
        supervisor, 5000, PipeEndpointKind::Supervisor, identity.identity.session_id);
    std::cout << "supervisor_starting owner_generation=" << owner_generation << '\n';
    const auto result = server.run();
    if (result) return fail(result.message, result.native_error);
    return 0;
}
