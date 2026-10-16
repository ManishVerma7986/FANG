#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#include "broker_native_job.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fang::broker {
namespace {

constexpr DWORD job_rights = JOB_OBJECT_QUERY | JOB_OBJECT_ASSIGN_PROCESS | READ_CONTROL;
constexpr DWORD setup_job_rights = job_rights | JOB_OBJECT_SET_ATTRIBUTES | WRITE_DAC;
constexpr DWORD reopen_rights = JOB_OBJECT_QUERY | READ_CONTROL;
constexpr DWORD system_job_rights = 0x001f003f;
constexpr std::size_t maximum_job_members = 65536;
constexpr std::wstring_view job_namespace = L"Global\\Fang.RuntimeBroker.Job.";

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept {
        const auto handle = handle_;
        handle_ = nullptr;
        return handle;
    }
private:
    HANDLE handle_{};
};

class LocalMemory {
public:
    explicit LocalMemory(void* value = nullptr) noexcept : value_(value) {}
    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;
    ~LocalMemory() {
        if (value_ != nullptr) {
            LocalFree(value_);
        }
    }
private:
    void* value_{};
};

class BCryptAlgorithm {
public:
    ~BCryptAlgorithm() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }
    BCRYPT_ALG_HANDLE* put() noexcept { return &handle_; }
    BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }
private:
    BCRYPT_ALG_HANDLE handle_{};
};

class BCryptHash {
public:
    ~BCryptHash() {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }
    BCRYPT_HASH_HANDLE* put() noexcept { return &handle_; }
    BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }
private:
    BCRYPT_HASH_HANDLE handle_{};
};

std::string win32_message(DWORD error) {
    return "Win32 error " + std::to_string(error);
}

bool is_access_denied(DWORD error) noexcept {
    return error == ERROR_ACCESS_DENIED;
}

bool is_unsupported(DWORD error) noexcept {
    return error == ERROR_NOT_SUPPORTED || error == ERROR_CALL_NOT_IMPLEMENTED;
}

bool wide_sid_for(std::string_view sid_text, std::wstring& wide_sid,
    std::string& error) {
    if (sid_text.empty() || sid_text.size() > 256 ||
        !std::all_of(sid_text.begin(), sid_text.end(), [](unsigned char value) {
            return value >= 0x20 && value <= 0x7e;
        })) {
        error = "The ownership SID is invalid.";
        return false;
    }
    wide_sid.assign(sid_text.begin(), sid_text.end());
    PSID sid{};
    if (ConvertStringSidToSidW(wide_sid.c_str(), &sid) == 0) {
        error = "The ownership SID is invalid: " + win32_message(GetLastError());
        return false;
    }
    LocalMemory sid_memory(sid);
    LPWSTR canonical{};
    if (ConvertSidToStringSidW(sid, &canonical) == 0) {
        error = "The ownership SID could not be canonicalized: " + win32_message(GetLastError());
        return false;
    }
    LocalMemory canonical_memory(canonical);
    if (wide_sid != canonical) {
        error = "The ownership SID is not in canonical string form.";
        return false;
    }
    return true;
}

bool current_process_sid(std::string& sid_text, std::string& error, DWORD& native_error) {
    native_error = ERROR_SUCCESS;
    HANDLE token_handle{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_handle) == 0) {
        native_error = GetLastError();
        error = "Could not open the current process token: " + win32_message(native_error);
        return false;
    }
    ScopedHandle token(token_handle);
    DWORD size{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    const DWORD sizing_error = GetLastError();
    if (sizing_error != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        native_error = sizing_error;
        error = "Could not size the current process user token: " + win32_message(sizing_error);
        return false;
    }
    std::vector<std::byte> buffer(size);
    if (GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) == 0) {
        native_error = GetLastError();
        error = "Could not read the current process user token: " + win32_message(native_error);
        return false;
    }
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR text{};
    if (ConvertSidToStringSidW(user->User.Sid, &text) == 0) {
        native_error = GetLastError();
        error = "Could not convert the current process SID: " + win32_message(native_error);
        return false;
    }
    LocalMemory text_memory(text);
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
        nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        native_error = GetLastError();
        error = "Could not encode the current process SID: " + win32_message(native_error);
        return false;
    }
    sid_text.resize(static_cast<std::size_t>(length));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
            sid_text.data(), length, nullptr, nullptr) == 0) {
        native_error = GetLastError();
        error = "Could not encode the current process SID: " + win32_message(native_error);
        sid_text.clear();
        return false;
    }
    sid_text.resize(static_cast<std::size_t>(length - 1));
    return true;
}

bool valid_generation_id(std::string_view value) noexcept {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return value[14] == '4' &&
        (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

bool sha256(std::string_view input, std::array<UCHAR, 32>& digest,
    std::string& error) {
    if (input.size() > std::numeric_limits<ULONG>::max()) {
        error = "The ownership identity is too large to hash.";
        return false;
    }
    BCryptAlgorithm algorithm;
    NTSTATUS status = BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM,
        nullptr, 0);
    if (status < 0) {
        error = "SHA-256 provider is unavailable (NTSTATUS " + std::to_string(status) + ").";
        return false;
    }
    DWORD object_size{};
    DWORD result_size{};
    status = BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0);
    if (status < 0 || object_size == 0) {
        error = "Could not query SHA-256 provider requirements (NTSTATUS " +
            std::to_string(status) + ").";
        return false;
    }
    std::vector<UCHAR> hash_object(object_size);
    BCryptHash hash;
    status = BCryptCreateHash(algorithm.get(), hash.put(), hash_object.data(), object_size,
        nullptr, 0, 0);
    if (status < 0) {
        error = "Could not initialize SHA-256 (NTSTATUS " + std::to_string(status) + ").";
        return false;
    }
    status = BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(
        const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0);
    if (status < 0) {
        error = "Could not hash the ownership identity (NTSTATUS " + std::to_string(status) + ").";
        return false;
    }
    status = BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (status < 0) {
        error = "Could not finish the ownership identity hash (NTSTATUS " +
            std::to_string(status) + ").";
        return false;
    }
    return true;
}

std::wstring hex_digest(const std::array<UCHAR, 32>& digest) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

bool make_job_name(const CanonicalAvdIdentity& identity, std::string_view generation_id,
    std::wstring& object_name, std::string& error) {
    if (!identity.valid() || !valid_generation_id(generation_id)) {
        error = "A valid canonical identity and version-4 generation identifier are required.";
        return false;
    }
    std::wstring ignored_sid;
    if (!wide_sid_for(identity.user_sid, ignored_sid, error)) {
        return false;
    }
    std::string material = std::to_string(identity.user_sid.size()) + ":" +
        identity.user_sid + std::to_string(identity.canonical_avd_location.size()) + ":" +
        identity.canonical_avd_location;
    std::array<UCHAR, 32> digest{};
    if (!sha256(material, digest, error)) {
        return false;
    }
    object_name.assign(job_namespace);
    object_name += hex_digest(digest);
    object_name.push_back(L'.');
    object_name.append(generation_id.begin(), generation_id.end());
    if (object_name.size() >= 240) {
        error = "The derived Job Object name exceeds the supported object-name length.";
        return false;
    }
    return true;
}

bool validate_job_security(HANDLE job, std::string_view expected_sid,
    std::string& error, DWORD* native_error = nullptr) {
    PSID owner{};
    PACL dacl{};
    PSECURITY_DESCRIPTOR descriptor{};
    const DWORD result = GetSecurityInfo(job, SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) {
        if (native_error != nullptr) *native_error = result;
        error = "Could not inspect Job Object security: " + win32_message(result);
        return false;
    }
    LocalMemory descriptor_memory(descriptor);

    std::wstring expected_wide;
    if (!wide_sid_for(expected_sid, expected_wide, error)) {
        return false;
    }
    PSID expected_owner{};
    if (ConvertStringSidToSidW(expected_wide.c_str(), &expected_owner) == 0) {
        if (native_error != nullptr) *native_error = GetLastError();
        error = "Could not parse the expected Job owner SID: " + win32_message(GetLastError());
        return false;
    }
    LocalMemory expected_owner_memory(expected_owner);
    if (owner == nullptr || !EqualSid(owner, expected_owner)) {
        error = "The named Job Object owner does not match the broker user.";
        return false;
    }
    if (dacl == nullptr) {
        error = "The named Job Object has a null DACL.";
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    if (GetSecurityDescriptorControl(descriptor, &control, &revision) == 0) {
        const DWORD native_result = GetLastError();
        if (native_error != nullptr) *native_error = native_result;
        error = "Could not inspect Job Object DACL protection: " + win32_message(native_result);
        return false;
    }
    if ((control & SE_DACL_PROTECTED) == 0) {
        if (native_error != nullptr) *native_error = ERROR_SUCCESS;
        error = "The named Job Object DACL is not protected from inheritance.";
        return false;
    }
    ACL_SIZE_INFORMATION acl_info{};
    if (GetAclInformation(dacl, &acl_info, sizeof(acl_info), AclSizeInformation) == 0) {
        const DWORD native_result = GetLastError();
        if (native_error != nullptr) *native_error = native_result;
        error = "Could not inspect Job Object DACL entries: " + win32_message(native_result);
        return false;
    }
    if (acl_info.AceCount != 2) {
        if (native_error != nullptr) *native_error = ERROR_SUCCESS;
        error = "The named Job Object DACL does not contain exactly the expected principals.";
        return false;
    }

    PSID system_sid{};
    if (ConvertStringSidToSidW(L"S-1-5-18", &system_sid) == 0) {
        if (native_error != nullptr) *native_error = GetLastError();
        error = "Could not parse the SYSTEM SID: " + win32_message(GetLastError());
        return false;
    }
    LocalMemory system_sid_memory(system_sid);
    bool found_user{};
    bool found_system{};
    for (DWORD index = 0; index < acl_info.AceCount; ++index) {
        void* raw_ace{};
        if (GetAce(dacl, index, &raw_ace) == 0) {
            if (native_error != nullptr) *native_error = GetLastError();
            error = "Could not inspect a Job Object DACL entry: " + win32_message(GetLastError());
            return false;
        }
        auto* header = static_cast<ACE_HEADER*>(raw_ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceFlags != 0) {
            error = "The named Job Object DACL contains an unexpected ACE.";
            return false;
        }
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
        PSID ace_sid = &ace->SidStart;
        if (EqualSid(ace_sid, expected_owner)) {
            if (found_user || ace->Mask != job_rights) {
                error = "The named Job Object user access mask is unexpected.";
                return false;
            }
            found_user = true;
        } else if (EqualSid(ace_sid, system_sid)) {
            if (found_system || ace->Mask != system_job_rights) {
                error = "The named Job Object SYSTEM access mask is unexpected: " +
                    std::to_string(ace->Mask) + ".";
                return false;
            }
            found_system = true;
        } else {
            error = "The named Job Object DACL grants access to an unexpected principal.";
            return false;
        }
    }
    if (!found_user || !found_system) {
        error = "The named Job Object DACL is missing a required principal.";
        return false;
    }
    return true;
}

bool validate_job_limits(HANDLE job, std::string& error, DWORD* native_error = nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits), nullptr) == 0) {
        const DWORD result = GetLastError();
        if (native_error != nullptr) *native_error = result;
        error = "Could not query Job Object limits: " + win32_message(result);
        return false;
    }
    if (limits.BasicLimitInformation.LimitFlags != 0) {
        error = "The Job Object has unexpected limits; kill-on-close and breakaway must remain disabled.";
        return false;
    }
    return true;
}

bool validate_job(HANDLE job, std::string_view expected_sid, std::string& error,
    DWORD* native_error = nullptr) {
    if (native_error != nullptr) *native_error = ERROR_SUCCESS;
    return validate_job_security(job, expected_sid, error, native_error) &&
        validate_job_limits(job, error, native_error);
}

bool set_job_dacl(HANDLE job, std::wstring_view user_sid, DWORD user_mask,
    std::string& error, DWORD& native_error) {
    native_error = ERROR_SUCCESS;
    constexpr wchar_t hex_digits[] = L"0123456789abcdef";
    std::wstring mask(8, L'0');
    for (int index = 7; index >= 0; --index) {
        mask[static_cast<std::size_t>(index)] = hex_digits[user_mask & 0x0f];
        user_mask >>= 4;
    }
    const std::wstring sddl = L"O:" + std::wstring(user_sid) + L"D:P(A;;GA;;;SY)(A;;0x" +
        mask + L";;;" + std::wstring(user_sid) + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        native_error = GetLastError();
        error = "Could not construct the Job Object DACL: " + win32_message(native_error);
        return false;
    }
    LocalMemory descriptor_memory(descriptor);
    BOOL present{};
    BOOL defaulted{};
    PACL dacl{};
    if (GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) == 0) {
        native_error = GetLastError();
        error = "Could not extract the protected Job Object DACL: " +
            win32_message(native_error);
        return false;
    }
    if (!present || dacl == nullptr) {
        native_error = ERROR_INVALID_ACL;
        error = "Could not extract the protected Job Object DACL.";
        return false;
    }
    const DWORD result = SetSecurityInfo(job, SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
    if (result != ERROR_SUCCESS) {
        native_error = result;
        error = "Could not apply the protected Job Object DACL: " + win32_message(result);
        return false;
    }
    return true;
}

JobOpenState open_state_for_error(DWORD error) noexcept {
    if (is_access_denied(error)) return JobOpenState::AccessDenied;
    if (error == ERROR_FILE_NOT_FOUND) return JobOpenState::NotFound;
    if (is_unsupported(error)) return JobOpenState::Unsupported;
    return JobOpenState::Failed;
}

JobCreateState create_state_for_error(DWORD error) noexcept {
    if (is_access_denied(error)) return JobCreateState::AccessDenied;
    if (is_unsupported(error)) return JobCreateState::Unsupported;
    return JobCreateState::Failed;
}

std::uint64_t filetime_value(const FILETIME& value) noexcept {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

JobMembershipResult membership_failure(JobMembershipResult::State state,
    std::string message) {
    return {state, 0, false, false, {}, std::move(message)};
}

JobMembershipResult::State query_state_for_error(DWORD error) noexcept {
    if (is_access_denied(error)) return JobMembershipResult::State::AccessDenied;
    if (is_unsupported(error)) return JobMembershipResult::State::Unsupported;
    return JobMembershipResult::State::Failed;
}

bool query_job_members(HANDLE job, std::vector<JobMembershipResult::ProcessIdentity>& members,
    DWORD& active_count, JobMembershipResult::State& failure_state, std::string& error) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
            &accounting, sizeof(accounting), nullptr) == 0) {
        const DWORD native_error = GetLastError();
        failure_state = query_state_for_error(native_error);
        error = "Could not query Job Object accounting: " + win32_message(native_error);
        return false;
    }
    if (accounting.ActiveProcesses > maximum_job_members) {
        failure_state = JobMembershipResult::State::Unsupported;
        error = "The Job Object member count exceeds the supported validation bound.";
        return false;
    }

    DWORD capacity = std::max<DWORD>(64, accounting.ActiveProcesses);
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (capacity > maximum_job_members) {
            failure_state = JobMembershipResult::State::Unsupported;
            error = "The Job Object member list exceeds the supported validation bound.";
            return false;
        }
        const auto header_size = offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList);
        std::vector<std::byte> buffer(header_size + sizeof(ULONG_PTR) * capacity);
        auto* process_list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
        process_list->NumberOfAssignedProcesses = capacity;
        if (QueryInformationJobObject(job, JobObjectBasicProcessIdList,
                process_list, static_cast<DWORD>(buffer.size()), nullptr) != 0) {
            if (process_list->NumberOfProcessIdsInList > capacity ||
                process_list->NumberOfAssignedProcesses > maximum_job_members) {
                failure_state = JobMembershipResult::State::Failed;
                error = "The Job Object returned an inconsistent member list.";
                return false;
            }
            if (process_list->NumberOfProcessIdsInList != accounting.ActiveProcesses ||
                process_list->NumberOfAssignedProcesses != accounting.ActiveProcesses) {
                failure_state = JobMembershipResult::State::Failed;
                error = "Job Object accounting changed during the membership snapshot.";
                return false;
            }
            members.clear();
            members.reserve(process_list->NumberOfProcessIdsInList);
            for (DWORD index = 0; index < process_list->NumberOfProcessIdsInList; ++index) {
                const auto raw_pid = process_list->ProcessIdList[index];
                if (raw_pid == 0 || raw_pid > std::numeric_limits<DWORD>::max()) {
                    failure_state = JobMembershipResult::State::Failed;
                    error = "The Job Object returned an invalid process identifier.";
                    return false;
                }
                const DWORD pid = static_cast<DWORD>(raw_pid);
                ScopedHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                    FALSE, pid));
                if (process.get() == nullptr) {
                    const DWORD native_error = GetLastError();
                    failure_state = query_state_for_error(native_error);
                    error = "Could not open a current Job member for identity validation: " +
                        win32_message(native_error);
                    return false;
                }
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user) == 0) {
                    const DWORD native_error = GetLastError();
                    failure_state = query_state_for_error(native_error);
                    error = "Could not read a Job member's creation time: " + win32_message(native_error);
                    return false;
                }
                if (WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) {
                    failure_state = JobMembershipResult::State::Failed;
                    error = "A Job member exited while the membership snapshot was being verified.";
                    return false;
                }
                BOOL is_member{};
                if (IsProcessInJob(process.get(), job, &is_member) == 0) {
                    const DWORD native_error = GetLastError();
                    failure_state = query_state_for_error(native_error);
                    error = "Could not verify process membership in the exact Job Object: " +
                        win32_message(native_error);
                    return false;
                }
                if (is_member == 0) {
                    failure_state = JobMembershipResult::State::SecurityMismatch;
                    error = "A listed PID is not a member of the exact validated Job Object.";
                    return false;
                }
                members.push_back({pid, filetime_value(created)});
            }
            active_count = accounting.ActiveProcesses;
            return true;
        }
        const DWORD native_error = GetLastError();
        if (native_error != ERROR_MORE_DATA) {
            failure_state = query_state_for_error(native_error);
            error = "Could not enumerate Job Object members: " + win32_message(native_error);
            return false;
        }
        const DWORD requested = process_list->NumberOfAssignedProcesses;
        const DWORD doubled = capacity > maximum_job_members / 2
            ? static_cast<DWORD>(maximum_job_members)
            : capacity * 2;
        capacity = std::max(requested, doubled);
    }
    failure_state = JobMembershipResult::State::Failed;
    error = "The Job Object membership list did not stabilize.";
    return false;
}

bool verify_root_exit(DWORD root_pid, std::uint64_t expected_creation_time,
    bool& identity_verified, bool& exit_verified, JobMembershipResult::State& failure_state,
    std::string& error) {
    ScopedHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
        FALSE, root_pid));
    if (process.get() == nullptr) {
        const DWORD native_error = GetLastError();
        if (native_error == ERROR_INVALID_PARAMETER) {
            identity_verified = true;
            exit_verified = true;
            return true;
        }
        failure_state = query_state_for_error(native_error);
        error = "Could not verify the recorded root process: " + win32_message(native_error);
        return false;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user) == 0) {
        const DWORD native_error = GetLastError();
        failure_state = query_state_for_error(native_error);
        error = "Could not read the recorded root process creation time: " +
            win32_message(native_error);
        return false;
    }
    if (filetime_value(created) != expected_creation_time) {
        failure_state = JobMembershipResult::State::SecurityMismatch;
        error = "The recorded root PID now identifies a different process generation.";
        return false;
    }
    identity_verified = true;
    const DWORD wait = WaitForSingleObject(process.get(), 0);
    if (wait == WAIT_OBJECT_0) {
        exit_verified = true;
        return true;
    }
    if (wait == WAIT_TIMEOUT) {
        failure_state = JobMembershipResult::State::SecurityMismatch;
        error = "The recorded root process is live but is absent from the validated Job.";
        return false;
    }
    const DWORD native_error = GetLastError();
    failure_state = query_state_for_error(native_error);
    error = "Could not verify root process exit: " + win32_message(native_error);
    return false;
}

}

NativeJobObject::NativeJobObject(void* handle, std::string user_sid,
        std::wstring object_name) noexcept
    : handle_(handle), user_sid_(std::move(user_sid)), object_name_(std::move(object_name)) {}

NativeJobObject::~NativeJobObject() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

void* NativeJobObject::native_handle() const noexcept {
    return handle_;
}

std::wstring_view NativeJobObject::object_name() const noexcept {
    return object_name_;
}

NativeNamedJobObjectProvider::NativeNamedJobObjectProvider(std::string expected_user_sid)
    : expected_user_sid_(std::move(expected_user_sid)) {}

JobCreateResult NativeNamedJobObjectProvider::create_non_killing(
    const CanonicalAvdIdentity& identity, std::string_view generation_id) {
    if (identity.user_sid != expected_user_sid_) {
        return {JobCreateState::SecurityMismatch, {},
            "The AVD identity does not belong to the configured broker user.", {}};
    }
    std::string actual_sid;
    std::string error;
    DWORD identity_error{};
    if (!current_process_sid(actual_sid, error, identity_error)) {
        return {create_state_for_error(identity_error), {}, std::move(error), {}};
    }
    if (actual_sid != expected_user_sid_) {
        return {JobCreateState::SecurityMismatch, {},
            "The provider is not running as the expected user.", {}};
    }
    std::wstring object_name;
    if (!make_job_name(identity, generation_id, object_name, error)) {
        return {JobCreateState::SecurityMismatch, {}, std::move(error), {}};
    }

    std::wstring sid_wide;
    if (!wide_sid_for(expected_user_sid_, sid_wide, error)) {
        return {JobCreateState::SecurityMismatch, {}, std::move(error), {}};
    }
    constexpr wchar_t hex_digits[] = L"0123456789abcdef";
    std::wstring setup_mask(8, L'0');
    DWORD setup_mask_value = setup_job_rights;
    for (int index = 7; index >= 0; --index) {
        setup_mask[static_cast<std::size_t>(index)] = hex_digits[setup_mask_value & 0x0f];
        setup_mask_value >>= 4;
    }
    const std::wstring sddl = L"O:" + sid_wide +
        L"D:P(A;;GA;;;SY)(A;;0x" + setup_mask + L";;;" + sid_wide + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        const DWORD native_error = GetLastError();
        return {is_unsupported(native_error) ? JobCreateState::Unsupported : JobCreateState::Failed,
            {}, "Could not create the Job Object security descriptor: " +
                win32_message(native_error), {}};
    }
    LocalMemory descriptor_memory(descriptor);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    SetLastError(ERROR_SUCCESS);
    HANDLE raw_job = CreateJobObjectW(&security, object_name.c_str());
    if (raw_job == nullptr) {
        const DWORD native_error = GetLastError();
        if (native_error == ERROR_ACCESS_DENIED) {
            ScopedHandle existing(OpenJobObjectW(READ_CONTROL, FALSE, object_name.c_str()));
            if (existing.get() != nullptr) {
                return {JobCreateState::NameCollision, {},
                    "The generated Job name already exists; its handle was not adopted.",
                    object_name};
            }
        }
        return {create_state_for_error(native_error), {},
            "Could not create the named Job Object: " + win32_message(native_error), {}};
    }
    ScopedHandle job(raw_job);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return {JobCreateState::NameCollision, {},
            "The generated named Job Object already exists; it was not adopted.", object_name};
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits)) == 0) {
        const DWORD native_error = GetLastError();
        return {create_state_for_error(native_error), {},
            "Could not establish non-killing, no-breakaway Job limits: " +
                win32_message(native_error), object_name};
    }
    DWORD native_error{};
    if (!set_job_dacl(job.get(), sid_wide, job_rights, error, native_error) ||
        !validate_job(job.get(), expected_user_sid_, error, &native_error)) {
        const auto state = native_error == ERROR_SUCCESS
            ? JobCreateState::SecurityMismatch : create_state_for_error(native_error);
        return {state, {}, std::move(error), object_name};
    }
    auto native_job = std::shared_ptr<INamedJobObject>(new NativeJobObject(
        job.release(), expected_user_sid_, object_name));
    return {JobCreateState::Created, std::move(native_job), {}, object_name};
}

JobOpenResult NativeNamedJobObjectProvider::reopen_for_recovery(
    const CanonicalAvdIdentity& identity, std::string_view generation_id,
    std::wstring_view object_name) {
    if (identity.user_sid != expected_user_sid_) {
        return {JobOpenState::SecurityMismatch, {},
            "The recorded AVD identity does not belong to the configured broker user."};
    }
    std::string actual_sid;
    std::string error;
    DWORD identity_error{};
    if (!current_process_sid(actual_sid, error, identity_error)) {
        return {open_state_for_error(identity_error), {}, std::move(error)};
    }
    if (actual_sid != expected_user_sid_) {
        return {JobOpenState::SecurityMismatch, {},
            "The provider is not running as the expected user."};
    }
    std::wstring expected_name;
    if (!make_job_name(identity, generation_id, expected_name, error) ||
        expected_name != object_name) {
        return {JobOpenState::SecurityMismatch, {}, error.empty()
            ? "The recorded Job name is not bound to this AVD identity and generation."
            : std::move(error)};
    }
    HANDLE raw_job = OpenJobObjectW(reopen_rights, FALSE, expected_name.c_str());
    if (raw_job == nullptr) {
        const DWORD native_error = GetLastError();
        return {open_state_for_error(native_error), {},
            "Could not reopen the recorded Job Object: " + win32_message(native_error)};
    }
    ScopedHandle job(raw_job);
    DWORD native_error{};
    if (!validate_job(job.get(), expected_user_sid_, error, &native_error)) {
        const auto state = native_error == ERROR_SUCCESS
            ? JobOpenState::SecurityMismatch : open_state_for_error(native_error);
        return {state, {}, std::move(error)};
    }
    auto native_job = std::shared_ptr<INamedJobObject>(new NativeJobObject(
        job.release(), expected_user_sid_, expected_name));
    return {JobOpenState::Opened, std::move(native_job), {}};
}

NativeJobMembershipQuery::NativeJobMembershipQuery(std::string expected_user_sid)
    : expected_user_sid_(std::move(expected_user_sid)) {}

JobMembershipResult NativeJobMembershipQuery::query(INamedJobObject& abstraction,
        const RuntimeSessionMetadata& expected_runtime) {
    auto* job = dynamic_cast<NativeJobObject*>(&abstraction);
    if (job == nullptr) {
        return membership_failure(JobMembershipResult::State::Failed,
            "The membership query received a non-native Job object.");
    }
    std::wstring expected_name;
    std::string error;
    if (expected_runtime.identity.user_sid != expected_user_sid_ ||
        job->user_sid_ != expected_user_sid_ ||
        !make_job_name(expected_runtime.identity, expected_runtime.generation_id,
            expected_name, error) || expected_name != job->object_name_) {
        return membership_failure(JobMembershipResult::State::SecurityMismatch,
            error.empty() ? "The Job handle does not match the recorded ownership identity." :
                std::move(error));
    }
    auto* handle = static_cast<HANDLE>(job->handle_);
    DWORD native_error{};
    if (!validate_job(handle, expected_user_sid_, error, &native_error)) {
        const auto state = native_error == ERROR_SUCCESS
            ? JobMembershipResult::State::SecurityMismatch
            : query_state_for_error(native_error);
        return membership_failure(state, std::move(error));
    }

    std::vector<JobMembershipResult::ProcessIdentity> members;
    DWORD active_count{};
    JobMembershipResult::State failure_state{JobMembershipResult::State::Failed};
    if (!query_job_members(handle, members, active_count, failure_state, error)) {
        return membership_failure(failure_state, std::move(error));
    }
    JobMembershipResult result{JobMembershipResult::State::Verified,
        active_count, false, false, std::move(members), {}};
    if (expected_runtime.phase < RuntimeLaunchPhase::ProcessCreationAttempted) {
        return result;
    }
    if (expected_runtime.root_process_id == 0 || expected_runtime.root_creation_time == 0) {
        return result;
    }
    const auto root = std::find_if(result.processes.begin(), result.processes.end(),
        [&](const auto& process) {
            return process.process_id == expected_runtime.root_process_id;
        });
    if (root != result.processes.end()) {
        if (root->creation_time != expected_runtime.root_creation_time) {
            return membership_failure(JobMembershipResult::State::SecurityMismatch,
                "The root PID in the Job belongs to a different process generation.");
        }
        result.root_identity_verified = true;
        return result;
    }
    if (!verify_root_exit(expected_runtime.root_process_id,
            expected_runtime.root_creation_time, result.root_identity_verified,
            result.root_exit_verified, failure_state, error)) {
        return membership_failure(failure_state, std::move(error));
    }
    return result;
}

std::string create_runtime_generation_id(std::string& error) {
    std::array<UCHAR, 16> bytes{};
    const NTSTATUS status = BCryptGenRandom(nullptr, bytes.data(),
        static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        error = "Could not generate a runtime generation identifier (NTSTATUS " +
            std::to_string(status) + ").";
        return {};
    }
    bytes[6] = static_cast<UCHAR>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<UCHAR>((bytes[8] & 0x3f) | 0x80);
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        result.push_back(digits[bytes[index] >> 4]);
        result.push_back(digits[bytes[index] & 0x0f]);
    }
    error.clear();
    return result;
}

bool expected_job_object_name(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::wstring& object_name, std::string& error) {
    return make_job_name(identity, generation_id, object_name, error);
}

}
