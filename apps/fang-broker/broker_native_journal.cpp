#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "broker_native_journal.hpp"
#include "broker_native_job.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fang::broker {
namespace {

constexpr std::array<std::byte, 4> journal_magic{
    std::byte{'F'}, std::byte{'J'}, std::byte{'R'}, std::byte{'N'}
};
constexpr std::array<std::byte, 4> store_magic{
    std::byte{'F'}, std::byte{'I'}, std::byte{'D'}, std::byte{'X'}
};
constexpr std::uint16_t journal_schema_version = 1;
constexpr std::size_t maximum_journal_size = 64 * 1024;
constexpr std::string_view path_generation = "00000000-0000-4000-8000-000000000000";
constexpr DWORD expected_file_access = 0x001f01ff;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    HANDLE get() const noexcept { return value_; }
    void reset() noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = nullptr;
    }
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

struct JournalRecord {
    bool empty_tombstone{};
    RuntimeSessionMetadata metadata;
};

std::string win32_error(DWORD error) {
    return "Win32 error " + std::to_string(error);
}

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xff));
    output.push_back(static_cast<std::byte>((value >> 8) & 0xff));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    }
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    }
}

bool take_u16(std::span<const std::byte> input, std::size_t& offset, std::uint16_t& value) {
    if (input.size() - offset < 2) return false;
    value = std::to_integer<std::uint8_t>(input[offset]) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1]) << 8);
    offset += 2;
    return true;
}

bool take_u32(std::span<const std::byte> input, std::size_t& offset, std::uint32_t& value) {
    if (input.size() - offset < 4) return false;
    value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index])) <<
            (index * 8);
    }
    offset += 4;
    return true;
}

bool take_u64(std::span<const std::byte> input, std::size_t& offset, std::uint64_t& value) {
    if (input.size() - offset < 8) return false;
    value = 0;
    for (unsigned int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index])) <<
            (index * 8);
    }
    offset += 8;
    return true;
}

void append_text(std::vector<std::byte>& output, std::string_view text) {
    append_u32(output, static_cast<std::uint32_t>(text.size()));
    for (const unsigned char value : text) output.push_back(static_cast<std::byte>(value));
}

bool take_text(std::span<const std::byte> input, std::size_t& offset, std::string& text) {
    std::uint32_t size{};
    if (!take_u32(input, offset, size) || size > maximum_journal_size ||
        input.size() - offset < size) return false;
    text.clear();
    text.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index) {
        text.push_back(static_cast<char>(std::to_integer<unsigned char>(input[offset + index])));
    }
    offset += size;
    return text.find('\0') == std::string::npos;
}

bool wide_to_utf8(std::wstring_view input, std::string& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    output.resize(static_cast<std::size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), output.data(), size, nullptr, nullptr) == size;
}

bool utf8_to_wide(std::string_view input, std::wstring& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) return false;
    output.resize(static_cast<std::size_t>(size));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), output.data(), size) == size;
}

bool valid_generation(std::string_view value) {
    std::wstring ignored;
    std::string error;
    std::wstring name;
    return expected_job_object_name({"S-1-5-18", "journal-validation"}, value, name, error);
}

std::vector<std::byte> encode_record(const RuntimeSessionMetadata& metadata,
    bool empty_tombstone) {
    std::wstring expected_job_name;
    std::string validation_error;
    if (!metadata.identity.valid() || !valid_generation(metadata.generation_id) ||
        !valid_generation(metadata.owner_generation_id) || metadata.owner_process_id == 0 ||
        metadata.named_job_object.empty() ||
        (empty_tombstone && metadata.phase != RuntimeLaunchPhase::StopRequested &&
            metadata.phase != RuntimeLaunchPhase::JobCreated) ||
        (metadata.phase < RuntimeLaunchPhase::ProcessCreationAttempted &&
            (metadata.root_process_id != 0 || metadata.root_creation_time != 0)) ||
        (metadata.phase >= RuntimeLaunchPhase::ProcessCreationAttempted &&
            ((metadata.root_process_id == 0) != (metadata.root_creation_time == 0))) ||
        !expected_job_object_name(metadata.identity, metadata.generation_id,
            expected_job_name, validation_error) || expected_job_name != metadata.named_job_object) {
        return {};
    }
    std::string job_name;
    if (!wide_to_utf8(metadata.named_job_object, job_name)) return {};
    std::vector<std::byte> output(journal_magic.begin(), journal_magic.end());
    append_u16(output, journal_schema_version);
    output.push_back(empty_tombstone ? std::byte{0} : std::byte{1});
    output.push_back(static_cast<std::byte>(metadata.phase));
    append_u32(output, metadata.owner_session_id);
    append_u32(output, metadata.owner_process_id);
    append_u32(output, metadata.root_process_id);
    append_u64(output, metadata.root_creation_time);
    append_text(output, metadata.identity.user_sid);
    append_text(output, metadata.identity.canonical_avd_location);
    append_text(output, metadata.generation_id);
    append_text(output, metadata.owner_generation_id);
    append_text(output, job_name);
    if (output.size() > maximum_journal_size) return {};
    return output;
}

bool decode_record(std::span<const std::byte> input, JournalRecord& record,
    JournalLoadState& failure, std::string& error) {
    if (input.size() < journal_magic.size() + 4 || input.size() > maximum_journal_size ||
        !std::equal(journal_magic.begin(), journal_magic.end(), input.begin())) {
        failure = JournalLoadState::Corrupt;
        error = "The ownership journal has an invalid header or size.";
        return false;
    }
    std::size_t offset = journal_magic.size();
    std::uint16_t version{};
    if (!take_u16(input, offset, version)) {
        failure = JournalLoadState::Corrupt;
        error = "The ownership journal is truncated.";
        return false;
    }
    if (version != journal_schema_version) {
        failure = JournalLoadState::Unsupported;
        error = "The ownership journal schema version is unsupported.";
        return false;
    }
    const auto kind = std::to_integer<std::uint8_t>(input[offset++]);
    const auto phase = std::to_integer<std::uint8_t>(input[offset++]);
    if (kind > 1 || phase > static_cast<std::uint8_t>(RuntimeLaunchPhase::StopRequested)) {
        failure = JournalLoadState::Corrupt;
        error = "The ownership journal contains an invalid state or launch phase.";
        return false;
    }
    record.empty_tombstone = kind == 0;
    record.metadata.phase = static_cast<RuntimeLaunchPhase>(phase);
    if (!take_u32(input, offset, record.metadata.owner_session_id) ||
        !take_u32(input, offset, record.metadata.owner_process_id) ||
        !take_u32(input, offset, record.metadata.root_process_id) ||
        !take_u64(input, offset, record.metadata.root_creation_time) ||
        !take_text(input, offset, record.metadata.identity.user_sid) ||
        !take_text(input, offset, record.metadata.identity.canonical_avd_location) ||
        !take_text(input, offset, record.metadata.generation_id) ||
        !take_text(input, offset, record.metadata.owner_generation_id)) {
        failure = JournalLoadState::Corrupt;
        error = "The ownership journal is truncated or contains malformed fields.";
        return false;
    }
    std::string job_name;
    if (!take_text(input, offset, job_name) || offset != input.size() ||
        !utf8_to_wide(job_name, record.metadata.named_job_object)) {
        failure = JournalLoadState::Corrupt;
        error = "The ownership journal Job name is malformed or has trailing data.";
        return false;
    }
    if (!record.metadata.identity.valid() || !valid_generation(record.metadata.generation_id) ||
        !valid_generation(record.metadata.owner_generation_id) ||
        record.metadata.owner_process_id == 0 || record.metadata.named_job_object.empty() ||
        (record.empty_tombstone &&
            record.metadata.phase != RuntimeLaunchPhase::StopRequested &&
            record.metadata.phase != RuntimeLaunchPhase::JobCreated) ||
        (record.metadata.phase < RuntimeLaunchPhase::ProcessCreationAttempted &&
            (record.metadata.root_process_id != 0 || record.metadata.root_creation_time != 0)) ||
        (record.metadata.phase >= RuntimeLaunchPhase::ProcessCreationAttempted &&
            ((record.metadata.root_process_id == 0) !=
                (record.metadata.root_creation_time == 0)))) {
        failure = JournalLoadState::Corrupt;
        error = "The ownership journal contains inconsistent ownership metadata.";
        return false;
    }
    std::wstring expected_job_name;
    if (!expected_job_object_name(record.metadata.identity, record.metadata.generation_id,
            expected_job_name, error) || expected_job_name != record.metadata.named_job_object) {
        failure = JournalLoadState::Corrupt;
        if (error.empty()) error = "The journal Job name does not match its identity/generation.";
        return false;
    }
    return true;
}

bool user_sid_wide(std::string_view sid_text, std::wstring& sid) {
    return utf8_to_wide(sid_text, sid);
}

std::wstring security_sddl(std::wstring_view sid) {
    return L"O:" + std::wstring(sid) + L"D:P(A;;FA;;;SY)(A;;FA;;;" +
        std::wstring(sid) + L")";
}

bool apply_private_acl(std::wstring_view path, std::wstring_view sid,
    std::string& error) {
    const auto sddl = security_sddl(sid);
    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        error = "Could not build ownership journal ACL: " + win32_error(GetLastError());
        return false;
    }
    LocalMemory descriptor_memory(descriptor);
    BOOL dacl_present{};
    BOOL dacl_defaulted{};
    PACL dacl{};
    if (GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl, &dacl_defaulted) == 0 ||
        !dacl_present || dacl == nullptr) {
        error = "Could not extract ownership journal DACL.";
        return false;
    }
    PSID owner{};
    if (ConvertStringSidToSidW(std::wstring(sid).c_str(), &owner) == 0) {
        error = "Could not parse ownership journal owner SID: " + win32_error(GetLastError());
        return false;
    }
    LocalMemory owner_memory(owner);
    const DWORD result = SetNamedSecurityInfoW(const_cast<LPWSTR>(std::wstring(path).c_str()),
        SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            PROTECTED_DACL_SECURITY_INFORMATION,
        owner, nullptr, dacl, nullptr);
    if (result != ERROR_SUCCESS) {
        error = "Could not apply ownership journal ACL: " + win32_error(result);
        return false;
    }
    return true;
}

bool validate_private_acl(std::wstring_view path, std::wstring_view sid,
    std::string& error, DWORD& native_error) {
    native_error = ERROR_SUCCESS;
    const std::wstring path_string(path);
    const DWORD attributes = GetFileAttributesW(path_string.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        native_error = GetLastError();
        error = "Could not inspect ownership journal path: " + win32_error(native_error);
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "Ownership journal paths may not contain a reparse point.";
        return false;
    }
    PSID owner{};
    PACL dacl{};
    PSECURITY_DESCRIPTOR descriptor{};
    const DWORD result = GetNamedSecurityInfoW(const_cast<LPWSTR>(path_string.c_str()),
        SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) {
        native_error = result;
        error = "Could not inspect ownership journal ACL: " + win32_error(result);
        return false;
    }
    LocalMemory descriptor_memory(descriptor);
    PSID expected_owner{};
    if (ConvertStringSidToSidW(std::wstring(sid).c_str(), &expected_owner) == 0) {
        native_error = GetLastError();
        error = "Could not parse expected ownership journal SID: " + win32_error(native_error);
        return false;
    }
    LocalMemory owner_memory(expected_owner);
    if (owner == nullptr || !EqualSid(owner, expected_owner) || dacl == nullptr) {
        error = "Ownership journal owner or DACL is invalid.";
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    ACL_SIZE_INFORMATION acl{};
    if (GetSecurityDescriptorControl(descriptor, &control, &revision) == 0) {
        native_error = GetLastError();
        error = "Could not inspect ownership journal DACL protection: " + win32_error(native_error);
        return false;
    }
    if ((control & SE_DACL_PROTECTED) == 0) {
        error = "Ownership journal DACL is not protected.";
        return false;
    }
    if (GetAclInformation(dacl, &acl, sizeof(acl), AclSizeInformation) == 0) {
        native_error = GetLastError();
        error = "Could not inspect ownership journal ACE count: " + win32_error(native_error);
        return false;
    }
    if (acl.AceCount != 2) {
        error = "Ownership journal DACL is unprotected or has unexpected entries.";
        return false;
    }
    PSID system_sid{};
    if (ConvertStringSidToSidW(L"S-1-5-18", &system_sid) == 0) {
        native_error = GetLastError();
        error = "Could not parse SYSTEM SID: " + win32_error(native_error);
        return false;
    }
    LocalMemory system_memory(system_sid);
    bool found_user{};
    bool found_system{};
    for (DWORD index = 0; index < acl.AceCount; ++index) {
        void* raw_ace{};
        if (GetAce(dacl, index, &raw_ace) == 0) {
            native_error = GetLastError();
            error = "Could not read ownership journal ACE: " + win32_error(native_error);
            return false;
        }
        auto* header = static_cast<ACE_HEADER*>(raw_ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceFlags != 0) {
            error = "Ownership journal contains an unexpected ACE type or inheritance flag.";
            return false;
        }
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
        PSID ace_sid = &ace->SidStart;
        if (EqualSid(ace_sid, expected_owner) && ace->Mask == expected_file_access) {
            found_user = true;
        } else if (EqualSid(ace_sid, system_sid) && ace->Mask == expected_file_access) {
            found_system = true;
        } else {
            error = "Ownership journal grants access to an unexpected principal or mask.";
            return false;
        }
    }
    if (!found_user || !found_system) {
        error = "Ownership journal DACL is missing a required principal.";
        return false;
    }
    return true;
}

bool ensure_directory(std::wstring_view path, std::wstring_view sid,
    std::string& error) {
    const std::wstring path_string(path);
    if (CreateDirectoryW(path_string.c_str(), nullptr) != 0) {
        return apply_private_acl(path, sid, error);
    }
    const DWORD native_error = GetLastError();
    if (native_error != ERROR_ALREADY_EXISTS) {
        error = "Could not create ownership journal directory: " + win32_error(native_error);
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path_string.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "Ownership journal path is not a directory.";
        return false;
    }
    DWORD acl_error{};
    return validate_private_acl(path, sid, error, acl_error);
}

bool default_storage_root(std::wstring& root, std::string& error) {
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0 || size > 32767) {
        error = "LOCALAPPDATA is unavailable for the per-user ownership journal.";
        return false;
    }
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size);
    if (written == 0 || written >= size) {
        error = "Could not read LOCALAPPDATA for the ownership journal.";
        return false;
    }
    value.resize(written);
    root = std::move(value) + L"\\Fang\\RuntimeBroker\\Ownership";
    return true;
}

bool select_storage_root(std::wstring_view configured_root, std::wstring& root,
    std::string& error) {
    if (configured_root.empty()) return default_storage_root(root, error);
    root.assign(configured_root);
    return true;
}

bool secure_directories(std::wstring root, std::wstring_view sid, std::string& error) {
    std::wstring local_app_data;
    if (!default_storage_root(local_app_data, error)) return false;
    const auto local_base = local_app_data.substr(0,
        local_app_data.find(L"\\Fang\\RuntimeBroker\\Ownership"));
    if (root.empty()) root = local_app_data;
    if (local_base.empty() || root.size() <= local_base.size() ||
        _wcsnicmp(root.c_str(), local_base.c_str(), local_base.size()) != 0 ||
        root[local_base.size()] != L'\\') {
        error = "The journal storage root must be a strict descendant of LOCALAPPDATA.";
        return false;
    }
    const DWORD base_attributes = GetFileAttributesW(local_base.c_str());
    if (base_attributes == INVALID_FILE_ATTRIBUTES ||
        (base_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (base_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "LOCALAPPDATA is not an existing directory.";
        return false;
    }

    std::wstring current = local_base + L"\\";
    std::size_t offset = local_base.size() + 1;
    while (offset < root.size()) {
        const auto next = root.find(L'\\', offset);
        const auto component = root.substr(offset,
            next == std::wstring::npos ? std::wstring::npos : next - offset);
        if (!component.empty()) {
            current += component;
            if (!ensure_directory(current, sid, error)) return false;
            current.push_back(L'\\');
        }
        if (next == std::wstring::npos) break;
        offset = next + 1;
    }
    return true;
}

bool journal_path(const std::wstring& root, const CanonicalAvdIdentity& identity,
    std::wstring& path, std::string& error) {
    std::wstring name;
    if (!expected_job_object_name(identity, path_generation, name, error)) return false;
    const auto namespace_separator = name.find_last_of(L'\\');
    const auto generation_dot = name.rfind(L'.');
    if (generation_dot == std::wstring::npos || namespace_separator == std::wstring::npos ||
        namespace_separator >= generation_dot) {
        error = "Could not derive the identity-bound journal filename.";
        return false;
    }
    path = root + L"\\" + name.substr(namespace_separator + 1,
        generation_dot - namespace_separator - 1) + L".fjn";
    return true;
}

std::wstring owner_store_path(const std::wstring& root) {
    return root + L"\\owner-index.fji";
}

std::wstring temporary_path(std::wstring_view final_path);

class StoreLock {
public:
    explicit StoreLock(std::wstring_view sid) {
        const std::wstring sddl = L"D:P(A;;0x00100001;;;SY)(A;;0x00100001;;;" +
            std::wstring(sid) + L")";
        PSECURITY_DESCRIPTOR descriptor{};
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
                SDDL_REVISION_1, &descriptor, nullptr) == 0) return;
        LocalMemory descriptor_memory(descriptor);
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        std::wstring name = L"Global\\Fang.RuntimeBroker.OwnerStore.";
        name.append(sid);
        handle_ = CreateMutexW(&security, FALSE, name.c_str());
        if (handle_ == nullptr) return;
        const DWORD wait = WaitForSingleObject(handle_, 5000);
        acquired_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;
    ~StoreLock() {
        if (acquired_) ReleaseMutex(handle_);
        if (handle_ != nullptr) CloseHandle(handle_);
    }
    bool acquired() const noexcept { return acquired_; }
private:
    HANDLE handle_{};
    bool acquired_{};
};

OwnershipStoreLoadResult read_store_file(std::wstring_view path, std::wstring_view sid,
    std::string_view expected_owner_generation) {
    const std::wstring file(path);
    HANDLE raw_file = CreateFileW(file.c_str(), GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw_file == INVALID_HANDLE_VALUE) {
        const DWORD native_error = GetLastError();
        if (native_error == ERROR_FILE_NOT_FOUND || native_error == ERROR_PATH_NOT_FOUND) {
            return {OwnershipStoreState::Missing, {}, "The authority index does not exist."};
        }
        if (native_error == ERROR_ACCESS_DENIED) {
            return {OwnershipStoreState::AccessDenied, {}, win32_error(native_error)};
        }
        return {OwnershipStoreState::Failed, {}, win32_error(native_error)};
    }
    ScopedHandle file_handle(raw_file);
    std::string acl_error;
    DWORD acl_native_error{};
    if (!validate_private_acl(file, sid, acl_error, acl_native_error)) {
        const auto state = acl_native_error == ERROR_ACCESS_DENIED
            ? OwnershipStoreState::AccessDenied
            : acl_native_error == ERROR_SUCCESS ? OwnershipStoreState::Corrupt
                : OwnershipStoreState::Failed;
        return {state, {}, std::move(acl_error)};
    }
    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file_handle.get(), &file_size) == 0) {
        const DWORD native_error = GetLastError();
        return {native_error == ERROR_ACCESS_DENIED ? OwnershipStoreState::AccessDenied :
            OwnershipStoreState::Failed, {}, win32_error(native_error)};
    }
    if (file_size.QuadPart < 0 || file_size.QuadPart > maximum_journal_size) {
        return {OwnershipStoreState::Corrupt, {}, "Authority index size is out of bounds."};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size.QuadPart));
    DWORD read{};
    if (!bytes.empty() && (ReadFile(file_handle.get(), bytes.data(),
            static_cast<DWORD>(bytes.size()), &read, nullptr) == 0 || read != bytes.size())) {
        const DWORD native_error = GetLastError();
        return {native_error == ERROR_ACCESS_DENIED ? OwnershipStoreState::AccessDenied :
            OwnershipStoreState::Failed, {}, win32_error(native_error)};
    }
    if (bytes.size() < 10 || !std::equal(store_magic.begin(), store_magic.end(), bytes.begin())) {
        return {OwnershipStoreState::Corrupt, {}, "Authority index header is malformed."};
    }
    std::size_t offset = store_magic.size();
    std::uint16_t version{};
    std::string owner_generation;
    std::uint32_t count{};
    if (!take_u16(bytes, offset, version)) {
        return {OwnershipStoreState::Corrupt, {}, "Authority index is truncated."};
    }
    if (version != 1) {
        return {OwnershipStoreState::Unsupported, {}, "Authority index version is unsupported."};
    }
    if (!take_text(bytes, offset, owner_generation)) {
        return {OwnershipStoreState::Corrupt, {}, "Authority index owner generation is malformed."};
    }
    if (owner_generation != expected_owner_generation) {
        return {OwnershipStoreState::Stale, {}, "Authority index belongs to another owner generation."};
    }
    if (!take_u32(bytes, offset, count) || count > 4096) {
        return {OwnershipStoreState::Corrupt, {}, "Authority index entry count is invalid."};
    }
    std::vector<OwnershipIndexEntry> entries;
    entries.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        OwnershipIndexEntry entry;
        std::uint8_t state{};
        if (!take_text(bytes, offset, entry.identity.user_sid) ||
            !take_text(bytes, offset, entry.identity.canonical_avd_location) ||
            !take_text(bytes, offset, entry.runtime_generation_id) ||
            offset >= bytes.size()) {
            return {OwnershipStoreState::Corrupt, {}, "Authority index entry is truncated."};
        }
        state = std::to_integer<std::uint8_t>(bytes[offset++]);
        if (!entry.identity.valid() || !valid_generation(entry.runtime_generation_id) || state > 2) {
            return {OwnershipStoreState::Corrupt, {}, "Authority index entry is inconsistent."};
        }
        entry.state = static_cast<OwnershipIndexEntryState>(state);
        if (std::any_of(entries.begin(), entries.end(), [&](const auto& previous) {
                return previous.identity.key() == entry.identity.key();
            })) {
            return {OwnershipStoreState::Corrupt, {}, "Authority index contains duplicate identities."};
        }
        entries.push_back(std::move(entry));
    }
    if (offset != bytes.size()) {
        return {OwnershipStoreState::Corrupt, {}, "Authority index has trailing data."};
    }
    return {OwnershipStoreState::Initialized, std::move(entries), {}};
}

std::vector<std::byte> encode_store(std::string_view owner_generation,
    const std::vector<OwnershipIndexEntry>& entries) {
    if (!valid_generation(owner_generation) || entries.size() > 4096) return {};
    std::vector<std::byte> bytes(store_magic.begin(), store_magic.end());
    append_u16(bytes, 1);
    append_text(bytes, owner_generation);
    append_u32(bytes, static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        if (!entry.identity.valid() || !valid_generation(entry.runtime_generation_id)) return {};
        append_text(bytes, entry.identity.user_sid);
        append_text(bytes, entry.identity.canonical_avd_location);
        append_text(bytes, entry.runtime_generation_id);
        bytes.push_back(static_cast<std::byte>(entry.state));
    }
    if (bytes.size() > maximum_journal_size) return {};
    return bytes;
}

bool write_store_file(std::wstring_view path, std::wstring_view sid,
    std::string_view owner_generation, const std::vector<OwnershipIndexEntry>& entries,
    std::string& error) {
    const auto bytes = encode_store(owner_generation, entries);
    if (bytes.empty()) {
        error = "Could not encode bounded authority index.";
        return false;
    }
    const std::wstring temp = temporary_path(path);
    if (temp.empty()) {
        error = "Could not create a temporary authority-index path.";
        return false;
    }
    const auto sddl = security_sddl(sid);
    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        error = "Could not create authority-index security: " + win32_error(GetLastError());
        return false;
    }
    LocalMemory descriptor_memory(descriptor);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    ScopedHandle file(CreateFileW(temp.c_str(), GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
        0, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        error = "Could not create temporary authority index: " + win32_error(GetLastError());
        return false;
    }
    DWORD written{};
    if (WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
            &written, nullptr) == 0 || written != bytes.size() || FlushFileBuffers(file.get()) == 0) {
        error = "Could not flush authority index: " + win32_error(GetLastError());
        file.reset();
        DeleteFileW(temp.c_str());
        return false;
    }
    std::string acl_error;
    DWORD acl_error_code{};
    if (!validate_private_acl(temp, sid, acl_error, acl_error_code)) {
        error = std::move(acl_error);
        file.reset();
        DeleteFileW(temp.c_str());
        return false;
    }
    file.reset();
    const std::wstring target(path);
    const DWORD attributes = GetFileAttributesW(target.c_str());
    BOOL replaced{};
    if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        replaced = MoveFileExW(temp.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } else if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!validate_private_acl(target, sid, acl_error, acl_error_code)) {
            error = std::move(acl_error);
            DeleteFileW(temp.c_str());
            return false;
        }
        replaced = ReplaceFileW(target.c_str(), temp.c_str(), nullptr,
            REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    }
    if (replaced == 0) {
        error = "Could not atomically replace authority index: " + win32_error(GetLastError());
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

std::wstring temporary_path(std::wstring_view final_path) {
    std::string error;
    const auto generation = create_runtime_generation_id(error);
    if (generation.empty()) return {};
    std::wstring result(final_path);
    result += L".";
    result.append(generation.begin(), generation.end());
    result += L".tmp";
    return result;
}

JournalLoadResult read_journal(std::wstring_view path, std::wstring_view sid,
    const CanonicalAvdIdentity& requested, std::string_view owner_generation_id,
    std::uint32_t owner_session_id, std::uint32_t owner_process_id) {
    const std::wstring file(path);
    HANDLE raw_file = CreateFileW(file.c_str(), GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw_file == INVALID_HANDLE_VALUE) {
        const DWORD native_error = GetLastError();
        if (native_error == ERROR_FILE_NOT_FOUND || native_error == ERROR_PATH_NOT_FOUND) {
            return {JournalLoadState::Missing, std::nullopt, "Ownership record does not exist."};
        }
        if (native_error == ERROR_ACCESS_DENIED) {
            return {JournalLoadState::AccessDenied, std::nullopt, win32_error(native_error)};
        }
        return {JournalLoadState::Failed, std::nullopt, win32_error(native_error)};
    }
    ScopedHandle file_handle(raw_file);
    std::string error;
    DWORD acl_error{};
    if (!validate_private_acl(file, sid, error, acl_error)) {
        const auto state = acl_error == ERROR_ACCESS_DENIED ? JournalLoadState::AccessDenied :
            acl_error == ERROR_SUCCESS ? JournalLoadState::Corrupt : JournalLoadState::Failed;
        return {state, std::nullopt, std::move(error)};
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file_handle.get(), &size) == 0) {
        const DWORD native_error = GetLastError();
        return {native_error == ERROR_ACCESS_DENIED ? JournalLoadState::AccessDenied :
            JournalLoadState::Failed, std::nullopt, win32_error(native_error)};
    }
    if (size.QuadPart < 0 || size.QuadPart > maximum_journal_size) {
        return {JournalLoadState::Corrupt, std::nullopt,
            "Ownership record size is outside the supported bounds."};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read{};
    if (!bytes.empty() && (ReadFile(file_handle.get(), bytes.data(),
            static_cast<DWORD>(bytes.size()), &read, nullptr) == 0 || read != bytes.size())) {
        const DWORD native_error = GetLastError();
        return {native_error == ERROR_ACCESS_DENIED ? JournalLoadState::AccessDenied :
            JournalLoadState::Failed, std::nullopt, win32_error(native_error)};
    }
    JournalRecord record;
    JournalLoadState failure{};
    if (!decode_record(bytes, record, failure, error)) {
        return {failure, std::nullopt, std::move(error)};
    }
    if (record.metadata.identity.user_sid != requested.user_sid) {
        return {JournalLoadState::IdentityMismatch, std::nullopt,
            "Ownership record user SID does not match the requested identity."};
    }
    if (record.metadata.identity.canonical_avd_location != requested.canonical_avd_location) {
        return {JournalLoadState::IdentityMismatch, std::nullopt,
            "Ownership record canonical AVD identity does not match the requested identity."};
    }
    if (record.metadata.owner_generation_id != owner_generation_id ||
        record.metadata.owner_session_id != owner_session_id ||
        record.metadata.owner_process_id != owner_process_id) {
        return {JournalLoadState::Stale, std::nullopt,
            "Ownership record belongs to a different supervisor generation or process."};
    }
    // Even a tombstone returns the recorded generation so the recovery layer
    // can independently reopen and query the corresponding native Job.
    return {JournalLoadState::Record, std::move(record.metadata), {}};
}

bool write_journal(std::wstring_view path, std::wstring_view sid,
    const RuntimeSessionMetadata& metadata, bool tombstone, std::string& error) {
    const auto bytes = encode_record(metadata, tombstone);
    if (bytes.empty()) {
        error = "Could not encode the bounded ownership journal record.";
        return false;
    }
    const auto temporary = temporary_path(path);
    if (temporary.empty()) {
        error = "Could not create a unique temporary journal name.";
        return false;
    }
    const auto sddl = security_sddl(sid);
    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        error = "Could not create temporary journal security: " + win32_error(GetLastError());
        return false;
    }
    LocalMemory descriptor_memory(descriptor);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    ScopedHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
        0, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        const DWORD native_error = GetLastError();
        std::string path_text;
        wide_to_utf8(temporary, path_text);
        error = "Could not create temporary journal file [" + path_text + "]: " +
            win32_error(native_error);
        return false;
    }
    DWORD written{};
    if (WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
            nullptr) == 0 || written != bytes.size() || FlushFileBuffers(file.get()) == 0) {
        error = "Could not durably write the temporary journal file: " + win32_error(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    DWORD acl_error{};
    if (!validate_private_acl(temporary, sid, error, acl_error)) {
        file.reset();
        DeleteFileW(temporary.c_str());
        return false;
    }
    file.reset();
    const std::wstring final_path(path);
    const DWORD attributes = GetFileAttributesW(final_path.c_str());
    BOOL replaced{};
    if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        replaced = MoveFileExW(temporary.c_str(), final_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } else if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!validate_private_acl(final_path, sid, error, acl_error)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        replaced = ReplaceFileW(final_path.c_str(), temporary.c_str(), nullptr,
            REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    }
    if (replaced == 0) {
        error = "Could not atomically replace the ownership journal: " + win32_error(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

}

NativeOwnershipJournal::NativeOwnershipJournal(std::string user_sid,
        std::uint32_t owner_session_id, std::uint32_t owner_process_id,
    std::string owner_generation_id, std::wstring storage_root)
    : user_sid_(std::move(user_sid)), owner_session_id_(owner_session_id),
      owner_process_id_(owner_process_id), owner_generation_id_(std::move(owner_generation_id)),
      storage_root_(std::move(storage_root)) {}

JournalLoadResult NativeOwnershipJournal::load(const CanonicalAvdIdentity& identity) {
    if (!identity.valid() || identity.user_sid != user_sid_ || owner_process_id_ == 0 ||
        !valid_generation(owner_generation_id_)) {
        return {JournalLoadState::IdentityMismatch, std::nullopt,
            "Journal configuration or requested identity is invalid."};
    }
    std::string error;
    std::wstring root;
    if (!select_storage_root(storage_root_, root, error)) {
        return {JournalLoadState::Failed, std::nullopt, std::move(error)};
    }
    std::wstring sid;
    if (!user_sid_wide(user_sid_, sid)) {
        error = "Could not convert the configured journal user SID to UTF-16.";
        return {JournalLoadState::IdentityMismatch, std::nullopt, "Configured user SID is invalid."};
    }
    if (!secure_directories(root, sid, error)) {
        if (error.empty()) error = "Could not validate or create the per-user journal directory.";
        const auto state = error.find("Win32 error 5") != std::string::npos
            ? JournalLoadState::AccessDenied : JournalLoadState::Failed;
        return {state, std::nullopt, std::move(error)};
    }
    std::wstring path;
    if (!journal_path(root, identity, path, error)) {
        return {JournalLoadState::Failed, std::nullopt, std::move(error)};
    }
    return read_journal(path, sid, identity, owner_generation_id_,
        owner_session_id_, owner_process_id_);
}

bool NativeOwnershipJournal::write_pending(const RuntimeSessionMetadata& source,
        std::string& error) {
    error.clear();
    if (!source.identity.valid() || source.identity.user_sid != user_sid_ ||
        source.owner_process_id != owner_process_id_ ||
        source.owner_session_id != owner_session_id_ ||
        source.owner_generation_id != owner_generation_id_) {
        error = "Pending ownership record does not match this owner identity/generation.";
        return false;
    }
    std::wstring root;
    if (!select_storage_root(storage_root_, root, error)) return false;
    std::wstring sid;
    if (!user_sid_wide(user_sid_, sid)) {
        error = "Could not convert the configured journal user SID to UTF-16.";
        return false;
    }
    if (!secure_directories(root, sid, error)) {
        if (error.empty()) error = "Could not validate or create the per-user journal directory.";
        return false;
    }
    std::wstring path;
    if (!journal_path(root, source.identity, path, error)) return false;
    const auto existing = read_journal(path, sid, source.identity, owner_generation_id_,
        owner_session_id_, owner_process_id_);
    if (existing.state != JournalLoadState::Missing) {
        if (existing.state != JournalLoadState::Record || !existing.record.has_value() ||
            existing.record->generation_id != source.generation_id ||
            static_cast<std::uint8_t>(source.phase) <
                static_cast<std::uint8_t>(existing.record->phase)) {
            error = "Pending write refuses to overwrite an ambiguous or different ownership generation.";
            return false;
        }
    }
    if (!write_journal(path, sid, source, false, error)) {
        if (error.empty()) error = "The ownership journal write failed without a native diagnostic.";
        return false;
    }
    return true;
}

bool NativeOwnershipJournal::mark_empty_verified(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::string& error) {
    // Stage 2B hardening: the production supervisor never calls this overload
    // in isolation. A journal tombstone alone must never authorize an empty
    // runtime; only mark_identity_empty() with a native-backed EmptyVerified
    // RecoveryDecision completes the trusted transition. This overload is kept
    // only for the legacy IOwnershipJournal interface and fails closed so a
    // caller cannot manufacture trusted empty state by writing a tombstone.
    static_cast<void>(identity);
    static_cast<void>(generation_id);
    error = "Direct tombstone writes are refused: finalize empty only through "
        "the supervisor with native-backed EmptyVerified evidence.";
    return false;
}

bool NativeOwnershipJournal::write_verified_empty_tombstone(
        const CanonicalAvdIdentity& identity, std::string_view generation_id,
        std::string& error) {
    error.clear();
    // Supervisor-only helper. The caller (OwnershipSupervisor::finalize_empty)
    // must already hold a native-backed EmptyVerified RecoveryDecision for
    // this exact generation; this function only performs the durable write.
    const auto loaded = load(identity);
    if (loaded.state != JournalLoadState::Record || !loaded.record.has_value() ||
        loaded.record->generation_id != generation_id) {
        error = "Cannot finalize an absent, invalid, or different ownership generation.";
        return false;
    }
    auto metadata = *loaded.record;
    if (metadata.phase >= RuntimeLaunchPhase::ProcessCreationAttempted) {
        metadata.phase = RuntimeLaunchPhase::StopRequested;
    }
    std::wstring root;
    if (!select_storage_root(storage_root_, root, error)) return false;
    std::wstring sid;
    if (!user_sid_wide(user_sid_, sid)) {
        error = "Could not convert the configured journal user SID to UTF-16.";
        return false;
    }
    if (!secure_directories(root, sid, error)) {
        if (error.empty()) error = "Could not validate or create the per-user journal directory.";
        return false;
    }
    std::wstring path;
    if (!journal_path(root, identity, path, error)) return false;
    return write_journal(path, sid, metadata, true, error);
}

OwnershipStoreLoadResult NativeOwnershipJournal::load_store() {
    if (owner_process_id_ == 0 || !valid_generation(owner_generation_id_)) {
        return {OwnershipStoreState::Failed, {}, "Invalid supervisor owner identity."};
    }
    std::wstring sid;
    if (!user_sid_wide(user_sid_, sid)) {
        return {OwnershipStoreState::Failed, {}, "Invalid supervisor user SID."};
    }
    StoreLock lock(sid);
    if (!lock.acquired()) {
        return {OwnershipStoreState::Failed, {}, "Could not acquire the authority-store mutex."};
    }
    std::string error;
    std::wstring root;
    if (!select_storage_root(storage_root_, root, error) || !secure_directories(root, sid, error)) {
        const auto state = error.find("Win32 error 5") != std::string::npos
            ? OwnershipStoreState::AccessDenied : OwnershipStoreState::Failed;
        return {state, {}, error.empty() ? "Could not access authority-store directory." :
            std::move(error)};
    }
    return read_store_file(owner_store_path(root), sid, owner_generation_id_);
}

bool NativeOwnershipJournal::initialize_store_explicit(std::string& error) {
    error.clear();
    if (owner_process_id_ == 0 || !valid_generation(owner_generation_id_)) {
        error = "Invalid supervisor owner identity.";
        return false;
    }
    std::wstring sid;
    if (!user_sid_wide(user_sid_, sid)) {
        error = "Invalid supervisor user SID.";
        return false;
    }
    StoreLock lock(sid);
    if (!lock.acquired()) {
        error = "Could not acquire the authority-store mutex.";
        return false;
    }
    std::wstring root;
    if (!select_storage_root(storage_root_, root, error) || !secure_directories(root, sid, error)) {
        if (error.empty()) error = "Could not initialize authority-store directory.";
        return false;
    }
    const auto path = owner_store_path(root);
    const auto existing = read_store_file(path, sid, owner_generation_id_);
    if (existing.state == OwnershipStoreState::Initialized) return true;
    if (existing.state != OwnershipStoreState::Missing) {
        error = existing.error.empty()
            ? "Authority-store initialization refuses to overwrite ambiguous state."
            : existing.error;
        return false;
    }
    return write_store_file(path, sid, owner_generation_id_, {}, error);
}

bool NativeOwnershipJournal::claim_identity(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::string& error) {
    error.clear();
    if (!identity.valid() || identity.user_sid != user_sid_ || !valid_generation(generation_id)) {
        error = "Invalid identity or runtime generation for reservation.";
        return false;
    }
    std::wstring sid;
    if (!user_sid_wide(user_sid_, sid)) {
        error = "Invalid supervisor user SID.";
        return false;
    }
    StoreLock lock(sid);
    if (!lock.acquired()) {
        error = "Could not acquire the authority-store mutex.";
        return false;
    }
    std::wstring root;
    if (!select_storage_root(storage_root_, root, error) || !secure_directories(root, sid, error)) return false;
    const auto path = owner_store_path(root);
    auto store = read_store_file(path, sid, owner_generation_id_);
    if (store.state != OwnershipStoreState::Initialized) {
        error = store.error.empty()
            ? "The authority store is missing or ambiguous; explicit initialization is required."
            : store.error;
        return false;
    }
    const auto entry = std::find_if(store.entries.begin(), store.entries.end(),
        [&](const auto& value) { return value.identity.key() == identity.key(); });
    if (entry != store.entries.end()) {
        if (entry->state != OwnershipIndexEntryState::EmptyVerified) {
            error = "This identity already has a pending or owned runtime generation.";
            return false;
        }
        entry->runtime_generation_id = generation_id;
        entry->state = OwnershipIndexEntryState::Pending;
    } else {
        store.entries.push_back({identity, std::string(generation_id),
            OwnershipIndexEntryState::Pending});
    }
    return write_store_file(path, sid, owner_generation_id_, store.entries, error);
}

bool NativeOwnershipJournal::mark_identity_owned(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::string& error) {
    error.clear();
    std::wstring sid;
    if (!user_sid_wide(user_sid_, sid)) {
        error = "Invalid supervisor user SID.";
        return false;
    }
    StoreLock lock(sid);
    if (!lock.acquired()) {
        error = "Could not acquire the authority-store mutex.";
        return false;
    }
    std::wstring root;
    if (!select_storage_root(storage_root_, root, error) || !secure_directories(root, sid, error)) return false;
    const auto path = owner_store_path(root);
    auto store = read_store_file(path, sid, owner_generation_id_);
    if (store.state != OwnershipStoreState::Initialized) {
        error = store.error.empty() ? "Authority store is not initialized." : store.error;
        return false;
    }
    const auto entry = std::find_if(store.entries.begin(), store.entries.end(),
        [&](const auto& value) {
            return value.identity.key() == identity.key() &&
                value.runtime_generation_id == generation_id;
        });
    if (entry == store.entries.end() || entry->state != OwnershipIndexEntryState::Pending) {
        error = "No matching pending identity reservation exists.";
        return false;
    }
    entry->state = OwnershipIndexEntryState::Owned;
    return write_store_file(path, sid, owner_generation_id_, store.entries, error);
}

bool NativeOwnershipJournal::mark_identity_empty(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, const RecoveryDecision& verified_decision,
        std::string& error) {
    error.clear();
    if (!verified_decision.replacement_launch_permitted()) {
        error = "Only a native-backed EmptyVerified decision may finalize the authority index.";
        return false;
    }
    std::wstring sid;
    if (!user_sid_wide(user_sid_, sid)) {
        error = "Invalid supervisor user SID.";
        return false;
    }
    StoreLock lock(sid);
    if (!lock.acquired()) {
        error = "Could not acquire the authority-store mutex.";
        return false;
    }
    std::wstring root;
    if (!select_storage_root(storage_root_, root, error) || !secure_directories(root, sid, error)) return false;
    const auto path = owner_store_path(root);
    auto store = read_store_file(path, sid, owner_generation_id_);
    if (store.state != OwnershipStoreState::Initialized) {
        error = store.error.empty() ? "Authority store is not initialized." : store.error;
        return false;
    }
    const auto entry = std::find_if(store.entries.begin(), store.entries.end(),
        [&](const auto& value) {
            return value.identity.key() == identity.key() &&
                value.runtime_generation_id == generation_id;
        });
    if (entry == store.entries.end() || entry->state != OwnershipIndexEntryState::Owned) {
        error = "No matching owned generation exists for empty-state finalization.";
        return false;
    }
    entry->state = OwnershipIndexEntryState::EmptyVerified;
    return write_store_file(path, sid, owner_generation_id_, store.entries, error);
}

bool NativeOwnershipJournal::write_next_generation(const RuntimeSessionMetadata& metadata,
        std::string_view previous_generation_id,
        const RecoveryDecision& verified_previous, std::string& error) {
    error.clear();
    if (!verified_previous.replacement_launch_permitted() ||
        !metadata.identity.valid() || metadata.identity.user_sid != user_sid_ ||
        metadata.owner_generation_id != owner_generation_id_ ||
        metadata.owner_session_id != owner_session_id_ ||
        metadata.owner_process_id != owner_process_id_) {
        error = "A new generation requires a native-backed empty decision from this owner.";
        return false;
    }
    const auto previous = load(metadata.identity);
    if (previous.state != JournalLoadState::Record || !previous.record.has_value() ||
        previous.record->generation_id != previous_generation_id ||
        (previous.record->phase != RuntimeLaunchPhase::StopRequested &&
         previous.record->phase != RuntimeLaunchPhase::JobCreated)) {
        error = "The prior journal generation is not the matching verified-empty tombstone.";
        return false;
    }
    std::wstring sid;
    if (!user_sid_wide(user_sid_, sid)) {
        error = "Invalid supervisor user SID.";
        return false;
    }
    std::wstring root;
    if (!select_storage_root(storage_root_, root, error) || !secure_directories(root, sid, error)) {
        if (error.empty()) error = "Could not access the per-user ownership journal directory.";
        return false;
    }
    std::wstring path;
    if (!journal_path(root, metadata.identity, path, error)) return false;
    return write_journal(path, sid, metadata, false, error);
}

}
