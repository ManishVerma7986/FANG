#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "broker_native_job.hpp"
#include "broker_native_journal.hpp"
#include "broker_pipe.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace fang::broker;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::wstring event_name(std::wstring_view suffix) {
    std::string error;
    const auto generation = create_runtime_generation_id(error);
    require(!generation.empty(), error);
    std::wstring result = L"Local\\Fang.BrokerNativeTest.";
    result.append(generation.begin(), generation.end());
    result.push_back(L'.');
    result.append(suffix);
    return result;
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    HANDLE get() const noexcept { return value_; }
    void reset() noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = nullptr;
    }
private:
    HANDLE value_{};
};

class ScopedFileDelete {
public:
    explicit ScopedFileDelete(std::wstring path) : path_(std::move(path)) {}
    ScopedFileDelete(const ScopedFileDelete&) = delete;
    ScopedFileDelete& operator=(const ScopedFileDelete&) = delete;
    ~ScopedFileDelete() { if (!path_.empty()) DeleteFileW(path_.c_str()); }
    const std::wstring& path() const noexcept { return path_; }
private:
    std::wstring path_;
};

class HelperProcess {
public:
    explicit HelperProcess(HANDLE job) {
        ready_name_ = event_name(L"ready");
        stop_name_ = event_name(L"stop");
        ready_event_ = CreateEventW(nullptr, TRUE, FALSE, ready_name_.c_str());
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, stop_name_.c_str());
        try {
            require(ready_event_ != nullptr && stop_event_ != nullptr,
                "Could not create the native helper synchronization events.");
            std::array<wchar_t, 32768> module_path{};
            const DWORD path_length = GetModuleFileNameW(nullptr, module_path.data(),
                static_cast<DWORD>(module_path.size()));
            require(path_length > 0 && path_length < module_path.size(),
                "Could not determine the native test executable path.");
            std::wstring command = L"\"" + std::wstring(module_path.data(), path_length) +
                L"\" --helper \"" + ready_name_ + L"\" \"" + stop_name_ + L"\"";
            std::vector<wchar_t> mutable_command(command.begin(), command.end());
            mutable_command.push_back(L'\0');
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION information{};
            require(CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                DETACHED_PROCESS | CREATE_SUSPENDED, nullptr, nullptr, &startup, &information) != 0,
                "Could not start the controlled native ownership helper.");
            process_ = information.hProcess;
            thread_ = information.hThread;
            process_id_ = information.dwProcessId;
            FILETIME created{}, exited{}, kernel{}, user{};
            require(GetProcessTimes(process_, &created, &exited, &kernel, &user) != 0,
                "Could not capture the helper process creation time.");
            ULARGE_INTEGER timestamp{};
            timestamp.LowPart = created.dwLowDateTime;
            timestamp.HighPart = created.dwHighDateTime;
            creation_time_ = timestamp.QuadPart;

            require(AssignProcessToJobObject(job, process_) != 0,
                "Could not assign the controlled helper to the native Job Object.");
            require(ResumeThread(thread_) != static_cast<DWORD>(-1),
                "Could not resume the helper after Job assignment.");
            require(WaitForSingleObject(ready_event_, 5000) == WAIT_OBJECT_0,
                "The controlled helper did not signal readiness.");
            CloseHandle(thread_);
            thread_ = nullptr;
        } catch (...) {
            if (stop_event_ != nullptr) SetEvent(stop_event_);
            if (thread_ != nullptr) ResumeThread(thread_);
            WaitForSingleObject(process_, 5000);
            if (thread_ != nullptr) CloseHandle(thread_);
            if (process_ != nullptr) CloseHandle(process_);
            if (ready_event_ != nullptr) CloseHandle(ready_event_);
            if (stop_event_ != nullptr) CloseHandle(stop_event_);
            thread_ = process_ = ready_event_ = stop_event_ = nullptr;
            throw;
        }
    }

    HelperProcess(const HelperProcess&) = delete;
    HelperProcess& operator=(const HelperProcess&) = delete;

    ~HelperProcess() {
        stop_and_wait();
        if (thread_ != nullptr) CloseHandle(thread_);
        if (process_ != nullptr) CloseHandle(process_);
        if (ready_event_ != nullptr) CloseHandle(ready_event_);
        if (stop_event_ != nullptr) CloseHandle(stop_event_);
    }

    DWORD process_id() const noexcept { return process_id_; }
    std::uint64_t creation_time() const noexcept { return creation_time_; }
    bool is_running() const noexcept {
        return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    bool stop_and_wait() noexcept {
        if (process_ == nullptr) return true;
        if (stop_event_ != nullptr) SetEvent(stop_event_);
        const DWORD wait = WaitForSingleObject(process_, 5000);
        if (wait != WAIT_OBJECT_0) return false;
        DWORD exit_code{};
        return GetExitCodeProcess(process_, &exit_code) != 0 && exit_code == 0;
    }

private:
    HANDLE process_{};
    HANDLE thread_{};
    HANDLE ready_event_{};
    HANDLE stop_event_{};
    std::wstring ready_name_;
    std::wstring stop_name_;
    DWORD process_id_{};
    std::uint64_t creation_time_{};
};

CanonicalAvdIdentity test_identity(std::string sid, std::string_view suffix = "device.avd") {
    return {std::move(sid), "C:\\FangNativeOwnershipTests\\" + std::string(suffix)};
}

void test_create_assign_close_reopen_and_recover() {
    const auto identity_result = current_user_identity();
    require(identity_result.succeeded, "Could not determine the current Windows user SID.");
    const auto& sid = identity_result.identity.sid;
    NativeNamedJobObjectProvider provider(sid);
    NativeJobMembershipQuery membership(sid);
    const auto identity = test_identity(sid);

    std::string error;
    const auto generation = create_runtime_generation_id(error);
    require(!generation.empty(), error);
    auto created = provider.create_non_killing(identity, generation);
    require(created.state == JobCreateState::Created && created.job != nullptr &&
        !created.object_name.empty() && created.error.empty(),
        "Native provider did not create a validated non-killing Job (state " +
            std::to_string(static_cast<unsigned int>(created.state)) + "): " + created.error);
    require(assess_new_job_creation(created) == JobCreationDecision::Created,
        "A validated native Job result was not accepted by the creation decision.");

    auto native_job = std::dynamic_pointer_cast<NativeJobObject>(created.job);
    require(native_job != nullptr, "The native provider returned a non-native Job wrapper.");
    HelperProcess helper(static_cast<HANDLE>(native_job->native_handle()));
    RuntimeSessionMetadata metadata{identity, generation, created.object_name,
        RuntimeLaunchPhase::Active, helper.process_id(), helper.creation_time()};
    auto membership_result = membership.query(*native_job, metadata);
    std::string member_list;
    for (const auto& member : membership_result.processes) {
        std::array<wchar_t, 32768> image{};
        DWORD image_size = static_cast<DWORD>(image.size());
        ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
            member.process_id));
        std::string image_text;
        if (process.get() != nullptr && QueryFullProcessImageNameW(process.get(), 0,
                image.data(), &image_size) != 0) {
            image_text.reserve(image_size);
            for (DWORD index = 0; index < image_size; ++index) {
                image_text.push_back(image[index] <= 0x7f
                    ? static_cast<char>(image[index]) : '?');
            }
        }
        member_list += " " + std::to_string(member.process_id) + "[" + image_text + "]";
    }
    const auto root_member = std::find_if(membership_result.processes.begin(),
        membership_result.processes.end(), [&](const auto& process) {
            return process.process_id == helper.process_id() &&
                process.creation_time == helper.creation_time();
        });
    require(membership_result.state == JobMembershipResult::State::Verified &&
        membership_result.active_process_count >= 1 &&
        root_member != membership_result.processes.end() &&
        membership_result.root_identity_verified && !membership_result.root_exit_verified,
        "Native membership query did not verify the helper PID, creation time, and live root (state " +
            std::to_string(static_cast<unsigned int>(membership_result.state)) + ", count " +
            std::to_string(membership_result.active_process_count) + ", expected " +
            std::to_string(helper.process_id()) + ", test " +
            std::to_string(GetCurrentProcessId()) + ", members" + member_list + "): " +
            membership_result.error);

    auto stale_identity = metadata;
    ++stale_identity.root_creation_time;
    const auto stale_result = membership.query(*native_job, stale_identity);
    require(stale_result.state == JobMembershipResult::State::SecurityMismatch,
        "A reused or stale root PID with a different creation time was accepted.");

    const auto collision = provider.create_non_killing(identity, generation);
    require(collision.state == JobCreateState::NameCollision && collision.job == nullptr,
        "Creation adopted an already existing named Job (state " +
            std::to_string(static_cast<unsigned int>(collision.state)) + "): " + collision.error);

    auto keeper = provider.reopen_for_recovery(identity, generation, metadata.named_job_object);
    require(keeper.state == JobOpenState::Opened && keeper.job != nullptr,
        "Could not acquire a separate test-owned keeper handle before closing the provider handle.");
    created.job.reset();
    native_job.reset();
    NativeNamedJobObjectProvider fresh_provider(sid);
    auto reopened = fresh_provider.reopen_for_recovery(
        identity, generation, metadata.named_job_object);
    require(reopened.state == JobOpenState::Opened && reopened.job != nullptr,
        "A fresh provider could not securely reopen the Job while its helper remained alive (state " +
            std::to_string(static_cast<unsigned int>(reopened.state)) + "): " + reopened.error);
    auto reopened_native = std::dynamic_pointer_cast<NativeJobObject>(reopened.job);
    require(reopened_native != nullptr, "Reopened Job did not retain its native wrapper.");
    membership_result = membership.query(*reopened_native, metadata);
    require(membership_result.state == JobMembershipResult::State::Verified &&
        membership_result.active_process_count >= 1,
        "The reopened Job did not report the live helper as a member.");

    const JournalLoadResult journal{JournalLoadState::Record, metadata, {}};
    const auto active_decision = assess_recovery(identity, journal, provider, membership);
    require(active_decision.state == RecoveryState::RuntimeMayBeActive &&
        active_decision.reason == RecoveryReason::ActiveMembers &&
        !active_decision.replacement_launch_permitted(),
        "A replacement launch was permitted while the controlled helper remained in the Job.");

    require(helper.stop_and_wait(), "The controlled helper did not exit normally after signaling.");
    membership_result = membership.query(*reopened_native, metadata);
    require(membership_result.state == JobMembershipResult::State::Verified &&
        membership_result.active_process_count == 0 && membership_result.processes.empty() &&
        membership_result.root_identity_verified && membership_result.root_exit_verified,
        "The empty Job did not include verified root identity and exit evidence.");
    const auto empty_decision = assess_recovery(identity, journal, provider, membership);
    require(empty_decision.state == RecoveryState::EmptyVerified &&
        empty_decision.reason == RecoveryReason::None &&
        empty_decision.replacement_launch_permitted(),
        "Recovery did not accept the independently verified empty Job and exited root.");
}

void test_missing_and_identity_mismatched_jobs() {
    const auto current = current_user_identity();
    require(current.succeeded, "Could not determine the current Windows user SID.");
    NativeNamedJobObjectProvider provider(current.identity.sid);
    const auto identity = test_identity(current.identity.sid, "missing.avd");
    std::string error;
    const auto generation = create_runtime_generation_id(error);
    require(!generation.empty(), error);
    std::wstring name;
    require(expected_job_object_name(identity, generation, name, error), error);
    const auto missing = provider.reopen_for_recovery(identity, generation, name);
    require(missing.state == JobOpenState::NotFound && missing.job == nullptr,
        "A nonexistent named Job did not return the explicit missing result.");
    const auto mismatch = provider.reopen_for_recovery(
        test_identity(current.identity.sid, "different.avd"), generation, name);
    require(mismatch.state == JobOpenState::SecurityMismatch && mismatch.job == nullptr,
        "A Job name derived for a different AVD identity was accepted.");
}

void test_final_handle_close_and_repeated_cycles() {
    const auto current = current_user_identity();
    require(current.succeeded, "Could not determine the current Windows user SID.");
    NativeNamedJobObjectProvider provider(current.identity.sid);
    const auto identity = test_identity(current.identity.sid, "final-handle.avd");
    std::string error;
    const auto generation = create_runtime_generation_id(error);
    require(!generation.empty(), error);
    auto created = provider.create_non_killing(identity, generation);
    require(created.state == JobCreateState::Created && created.job != nullptr,
        "Could not create the final-handle lifetime fixture: " + created.error);
    auto native_job = std::dynamic_pointer_cast<NativeJobObject>(created.job);
    require(native_job != nullptr, "Final-handle fixture is not a native Job wrapper.");
    HelperProcess helper(static_cast<HANDLE>(native_job->native_handle()));
    const auto object_name = created.object_name;
    created.job.reset();
    native_job.reset();

    require(helper.is_running(),
        "The non-killing Job terminated its helper when the final user handle closed.");
    const auto missing = provider.reopen_for_recovery(identity, generation, object_name);
    require(missing.state == JobOpenState::NotFound && missing.job == nullptr,
        "The named Job remained discoverable after its final handle closed.");
    require(helper.stop_and_wait(),
        "The helper did not exit normally after final Job-handle loss.");

    for (int cycle = 0; cycle < 5; ++cycle) {
        const auto cycle_identity = test_identity(current.identity.sid,
            "cycle-" + std::to_string(cycle) + ".avd");
        const auto cycle_generation = create_runtime_generation_id(error);
        require(!cycle_generation.empty(), error);
        auto cycle_job = provider.create_non_killing(cycle_identity, cycle_generation);
        require(cycle_job.state == JobCreateState::Created && cycle_job.job != nullptr,
            "Repeated provider creation failed: " + cycle_job.error);
        auto reopened = provider.reopen_for_recovery(cycle_identity,
            cycle_generation, cycle_job.object_name);
        require(reopened.state == JobOpenState::Opened && reopened.job != nullptr,
            "Repeated provider reopen failed: " + reopened.error);
        NativeJobMembershipQuery membership(current.identity.sid);
        RuntimeSessionMetadata metadata{cycle_identity, cycle_generation,
            cycle_job.object_name, RuntimeLaunchPhase::JobCreated, 0, 0};
        const auto observation = membership.query(*reopened.job, metadata);
        require(observation.state == JobMembershipResult::State::Verified &&
            observation.active_process_count == 0,
            "A fresh repeated-cycle Job did not report verified-empty membership.");
        reopened.job.reset();
        cycle_job.job.reset();
        const auto gone = provider.reopen_for_recovery(cycle_identity,
            cycle_generation, cycle_job.object_name);
        require(gone.state == JobOpenState::NotFound,
            "A repeated-cycle Job remained after all handles were closed.");
    }
}

void test_security_mismatch_and_access_denied() {
    const auto current = current_user_identity();
    require(current.succeeded, "Could not determine the current Windows user SID.");
    NativeNamedJobObjectProvider provider(current.identity.sid);
    std::string error;
    const auto generation = create_runtime_generation_id(error);
    require(!generation.empty(), error);

    const auto mismatch_identity = test_identity(current.identity.sid, "wrong-dacl.avd");
    std::wstring mismatch_name;
    require(expected_job_object_name(mismatch_identity, generation, mismatch_name, error), error);
    const std::wstring mismatch_sddl = L"O:" +
        std::wstring(current.identity.sid.begin(), current.identity.sid.end()) +
        L"D:P(A;;GA;;;SY)(A;;0x00120007;;;" +
        std::wstring(current.identity.sid.begin(), current.identity.sid.end()) +
        L")(A;;0x0004;;;AU)";
    PSECURITY_DESCRIPTOR mismatch_descriptor{};
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(mismatch_sddl.c_str(),
        SDDL_REVISION_1, &mismatch_descriptor, nullptr) != 0,
        "Could not construct the deliberately mismatched test DACL.");
    ScopedHandle mismatch_handle(CreateJobObjectW(nullptr, mismatch_name.c_str()));
    require(mismatch_handle.get() != nullptr,
        "Could not create the deliberately mismatched named Job.");
    BOOL dacl_present{};
    BOOL dacl_defaulted{};
    PACL mismatch_dacl{};
    require(GetSecurityDescriptorDacl(mismatch_descriptor, &dacl_present,
            &mismatch_dacl, &dacl_defaulted) != 0 && dacl_present && mismatch_dacl != nullptr,
        "Could not read the deliberately mismatched test DACL.");
    require(SetSecurityInfo(mismatch_handle.get(), SE_KERNEL_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, mismatch_dacl, nullptr) == ERROR_SUCCESS,
        "Could not set the deliberately mismatched test DACL.");
    LocalFree(mismatch_descriptor);
    mismatch_descriptor = nullptr;
    const auto mismatch_open = provider.reopen_for_recovery(
        mismatch_identity, generation, mismatch_name);
    require(mismatch_open.state == JobOpenState::SecurityMismatch && mismatch_open.job == nullptr,
        "A named Job with an unexpected additional DACL principal was accepted (state " +
            std::to_string(static_cast<unsigned int>(mismatch_open.state)) + "): " + mismatch_open.error);

    const auto denied_identity = test_identity(current.identity.sid, "denied-dacl.avd");
    const auto denied_generation = create_runtime_generation_id(error);
    require(!denied_generation.empty(), error);
    std::wstring denied_name;
    require(expected_job_object_name(denied_identity, denied_generation, denied_name, error), error);
    const std::wstring denied_sddl = L"D:P(A;;GA;;;SY)";
    PSECURITY_DESCRIPTOR denied_descriptor{};
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(denied_sddl.c_str(),
        SDDL_REVISION_1, &denied_descriptor, nullptr) != 0,
        "Could not construct the access-denied test DACL.");
    ScopedHandle denied_handle(CreateJobObjectW(nullptr, denied_name.c_str()));
    require(denied_handle.get() != nullptr,
        "Could not create the access-denied named Job.");
    PACL denied_dacl{};
    require(GetSecurityDescriptorDacl(denied_descriptor, &dacl_present,
            &denied_dacl, &dacl_defaulted) != 0 && dacl_present && denied_dacl != nullptr,
        "Could not read the access-denied test DACL.");
    require(SetSecurityInfo(denied_handle.get(), SE_KERNEL_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, denied_dacl, nullptr) == ERROR_SUCCESS,
        "Could not set the access-denied test DACL.");
    LocalFree(denied_descriptor);
    denied_descriptor = nullptr;
    const auto denied_open = provider.reopen_for_recovery(
        denied_identity, denied_generation, denied_name);
    require(denied_open.state == JobOpenState::AccessDenied && denied_open.job == nullptr,
        "A named Job that denied query access was not classified as AccessDenied (state " +
            std::to_string(static_cast<unsigned int>(denied_open.state)) + "): " + denied_open.error);
}

std::wstring native_journal_path(const CanonicalAvdIdentity& identity) {
    constexpr std::string_view fixed_generation =
        "00000000-0000-4000-8000-000000000000";
    std::wstring object_name;
    std::string error;
    require(expected_job_object_name(identity, fixed_generation, object_name, error), error);
    const auto namespace_separator = object_name.find_last_of(L'\\');
    const auto separator = object_name.rfind(L'.');
    require(separator != std::wstring::npos && namespace_separator != std::wstring::npos &&
        namespace_separator < separator, "Could not derive journal filename in test.");
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    require(size > 1, "LOCALAPPDATA is unavailable to the journal test.");
    std::wstring root(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), size);
    require(written > 0 && written < size, "Could not read LOCALAPPDATA in journal test.");
    root.resize(written);
    root += L"\\Fang\\RuntimeBroker\\Ownership\\";
    root += object_name.substr(namespace_separator + 1,
        separator - namespace_separator - 1);
    root += L".fjn";
    return root;
}

void overwrite_journal(const std::wstring& path, std::span<const std::byte> bytes) {
    ScopedHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    require(file.get() != INVALID_HANDLE_VALUE, "Could not open the test journal for corruption.");
    DWORD written{};
    require(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
            &written, nullptr) != 0 && written == bytes.size() && FlushFileBuffers(file.get()) != 0,
        "Could not write the bounded test journal corruption fixture.");
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

void copy_journal_security(const std::wstring& source, const std::wstring& target) {
    PSID owner{};
    PACL dacl{};
    PSECURITY_DESCRIPTOR descriptor{};
    const DWORD read_result = GetNamedSecurityInfoW(const_cast<LPWSTR>(source.c_str()),
        SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    require(read_result == ERROR_SUCCESS,
        "Could not read the original journal ACL for identity-mismatch fixture.");
    const DWORD write_result = SetNamedSecurityInfoW(const_cast<LPWSTR>(target.c_str()),
        SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            PROTECTED_DACL_SECURITY_INFORMATION,
        owner, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    require(write_result == ERROR_SUCCESS,
        "Could not preserve the protected journal ACL on identity-mismatch fixture.");
}

void test_durable_journal_validation_and_atomic_update() {
    const auto current = current_user_identity();
    require(current.succeeded, "Could not determine the current Windows user SID.");
    std::string error;
    const auto owner_generation = create_runtime_generation_id(error);
    const auto generation = create_runtime_generation_id(error);
    require(!owner_generation.empty() && !generation.empty(), error);
    const auto identity = test_identity(current.identity.sid,
        "journal-" + owner_generation + ".avd");
    ScopedFileDelete cleanup(native_journal_path(identity));
    NativeOwnershipJournal journal(current.identity.sid, current.identity.session_id,
        GetCurrentProcessId(), owner_generation);
    NativeNamedJobObjectProvider jobs(current.identity.sid);
    NativeJobMembershipQuery membership(current.identity.sid);

    auto loaded = journal.load(identity);
    require(loaded.state == JournalLoadState::Missing,
        "An absent journal did not remain a distinct Missing result.");

    auto created = jobs.create_non_killing(identity, generation);
    require(created.state == JobCreateState::Created && created.job != nullptr,
        "Could not create the native Job for the journal/recovery integration test: " + created.error);
    auto native_job = std::dynamic_pointer_cast<NativeJobObject>(created.job);
    require(native_job != nullptr, "Journal test received a non-native Job wrapper.");
    HelperProcess helper(static_cast<HANDLE>(native_job->native_handle()));
    RuntimeSessionMetadata metadata{identity, generation, created.object_name,
        RuntimeLaunchPhase::Active, helper.process_id(), helper.creation_time(),
        owner_generation, current.identity.session_id, GetCurrentProcessId()};
    metadata.named_job_object = created.object_name;
    const bool pending_written = journal.write_pending(metadata, error);
    require(pending_written,
        "Could not atomically persist pending ownership metadata: " + error);
    const DWORD journal_attributes = GetFileAttributesW(cleanup.path().c_str());
    require(journal_attributes != INVALID_FILE_ATTRIBUTES &&
        (journal_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
        "Journal write reported success but the expected file is absent (Win32 " +
            std::to_string(GetLastError()) + ").");
    loaded = journal.load(identity);
    require(loaded.state == JournalLoadState::Record && loaded.record.has_value() &&
        loaded.record->identity.key() == identity.key() &&
        loaded.record->generation_id == generation &&
        loaded.record->named_job_object == created.object_name &&
        loaded.record->root_process_id == metadata.root_process_id &&
        loaded.record->root_creation_time == metadata.root_creation_time &&
        loaded.record->owner_generation_id == owner_generation &&
        loaded.record->owner_session_id == current.identity.session_id &&
        loaded.record->owner_process_id == GetCurrentProcessId(),
        "The versioned journal did not preserve its identity/owner/root bindings (state " +
            std::to_string(static_cast<unsigned int>(loaded.state)) + ", error " + loaded.error +
            (loaded.record.has_value() ? ", generation " + loaded.record->generation_id +
                ", owner generation " + loaded.record->owner_generation_id +
                ", owner PID " + std::to_string(loaded.record->owner_process_id) +
                ", root PID " + std::to_string(loaded.record->root_process_id) : ", no record") + ").");
    auto active_decision = assess_recovery(identity, loaded, jobs, membership);
    require(active_decision.state == RecoveryState::RuntimeMayBeActive &&
        active_decision.reason == RecoveryReason::ActiveMembers &&
        !active_decision.replacement_launch_permitted(),
        "A real journal record and native Job query did not block replacement for a live helper.");

    const auto other_identity = test_identity(current.identity.sid,
        "journal-other-" + owner_generation + ".avd");
    ScopedFileDelete other_cleanup(native_journal_path(other_identity));
    require(CopyFileW(cleanup.path().c_str(), other_cleanup.path().c_str(), TRUE) != 0,
        "Could not create the copied-identity journal fixture.");
    copy_journal_security(cleanup.path(), other_cleanup.path());
    const auto identity_mismatch = journal.load(other_identity);
    require(identity_mismatch.state == JournalLoadState::IdentityMismatch &&
        !identity_mismatch.record.has_value(),
        "A journal copied to another identity's filename was accepted (state " +
            std::to_string(static_cast<unsigned int>(identity_mismatch.state)) + "): " +
            identity_mismatch.error);

    const auto different_owner_generation = create_runtime_generation_id(error);
    require(!different_owner_generation.empty(), error);
    NativeOwnershipJournal stale_owner(current.identity.sid, current.identity.session_id,
        GetCurrentProcessId(), different_owner_generation);
    auto stale_owner_loaded = stale_owner.load(identity);
    require(stale_owner_loaded.state == JournalLoadState::Stale,
        "A journal record from another owner generation was not classified as Stale.");

    std::array<std::byte, 256> unsupported{};
    HANDLE journal_file = CreateFileW(cleanup.path().c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(journal_file != INVALID_HANDLE_VALUE,
        "Could not open the journal for unsupported-version test.");
    ScopedHandle journal_handle(journal_file);
    DWORD bytes_read{};
    require(ReadFile(journal_handle.get(), unsupported.data(),
            static_cast<DWORD>(unsupported.size()), &bytes_read, nullptr) != 0 && bytes_read > 6,
        "Could not read the journal for unsupported-version test.");
    unsupported[4] = std::byte{2};
    require(SetFilePointer(journal_handle.get(), 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER,
        "Could not seek the journal version field.");
    DWORD bytes_written{};
    require(WriteFile(journal_handle.get(), unsupported.data(), bytes_read,
            &bytes_written, nullptr) != 0 && bytes_written == bytes_read &&
        FlushFileBuffers(journal_handle.get()) != 0,
        "Could not write unsupported journal schema version.");
    journal_handle.reset();
    loaded = journal.load(identity);
    require(loaded.state == JournalLoadState::Unsupported,
        "An unsupported journal schema version was not distinguished from corruption.");
    error.clear();
    require(!journal.write_pending(metadata, error) && !error.empty(),
        "A pending write overwrote an unsupported journal schema.");
    require(DeleteFileW(cleanup.path().c_str()) != 0,
        "Could not remove the isolated unsupported-schema test fixture.");
    require(journal.write_pending(metadata, error),
        "Could not create a fresh journal after removing its isolated test fixture: " + error);

    require(helper.stop_and_wait(),
        "The controlled helper did not stop normally before journal finalization.");
    require(journal.mark_empty_verified(identity, generation, error),
        "Could not atomically update the verified-empty journal generation: " + error);
    loaded = journal.load(identity);
    require(loaded.state == JournalLoadState::Record && loaded.record.has_value() &&
        loaded.record->phase == RuntimeLaunchPhase::StopRequested,
        "A journal tombstone was treated as empty without returning a Job generation for native validation.");
    const auto empty_decision = assess_recovery(identity, loaded, jobs, membership);
    require(empty_decision.state == RecoveryState::EmptyVerified &&
        empty_decision.reason == RecoveryReason::None &&
        empty_decision.replacement_launch_permitted(),
        "A persisted tombstone plus native empty Job/root evidence did not permit verified-empty recovery.");

    const std::array<std::byte, 4> truncated{
        std::byte{'F'}, std::byte{'J'}, std::byte{'R'}, std::byte{'N'}};
    overwrite_journal(cleanup.path(), truncated);
    loaded = journal.load(identity);
    require(loaded.state == JournalLoadState::Corrupt,
        "A truncated journal was not classified as Corrupt.");

    error.clear();
    require(!journal.write_pending(metadata, error) && !error.empty(),
        "A pending write overwrote a corrupt journal.");

    require(DeleteFileW(cleanup.path().c_str()) != 0,
        "Could not remove the isolated corrupt journal fixture.");
    error.clear();
    require(journal.write_pending(metadata, error),
        "Could not recreate the isolated journal before access-denial test: " + error);
    restrict_journal_to_system(cleanup.path());
    loaded = journal.load(identity);
    require(loaded.state == JournalLoadState::AccessDenied && !loaded.record.has_value(),
        "An unreadable journal was not classified as AccessDenied.");
}

int helper_main(const wchar_t* ready_name, const wchar_t* stop_name) {
    ScopedHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name));
    ScopedHandle stop(OpenEventW(SYNCHRONIZE, FALSE, stop_name));
    if (ready.get() == nullptr || stop.get() == nullptr || SetEvent(ready.get()) == 0) {
        return 2;
    }
    return WaitForSingleObject(stop.get(), 30000) == WAIT_OBJECT_0 ? 0 : 3;
}

}

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--helper") {
        return helper_main(argv[2], argv[3]);
    }
    try {
        test_create_assign_close_reopen_and_recover();
        test_missing_and_identity_mismatched_jobs();
        test_final_handle_close_and_repeated_cycles();
        test_security_mismatch_and_access_denied();
        test_durable_journal_validation_and_atomic_update();
    } catch (const std::exception& error) {
        std::cerr << "native broker ownership test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "5 native broker ownership test groups passed\n";
    return 0;
}
