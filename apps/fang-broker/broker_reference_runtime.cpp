#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "broker_reference_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fang::broker {
namespace {

constexpr std::size_t maximum_properties_size = 64 * 1024;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() { if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    HANDLE get() const noexcept { return value_; }
    HANDLE release() noexcept { return std::exchange(value_, nullptr); }
    void reset() noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = nullptr;
    }
private:
    HANDLE value_{};
};

std::uint64_t filetime_value(const FILETIME& value) noexcept {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

std::wstring quote_argument(std::wstring_view argument) {
    std::wstring result = L"\"";
    std::size_t backslashes{};
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring build_command_line(const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments) {
    std::wstring command = quote_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    return command;
}

void append_bounded(std::string& destination, const char* data, std::size_t size,
    std::size_t limit) {
    if (destination.size() >= limit) return;
    destination.append(data, std::min(size, limit - destination.size()));
}

std::wstring environment_value(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0 || size > 32768) return {};
    std::wstring value(size, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), size);
    if (copied == 0 || copied >= size) return {};
    value.resize(copied);
    return value;
}

std::wstring trim_wide(std::wstring_view value) {
    constexpr std::wstring_view whitespace = L" \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::wstring_view::npos) return {};
    const auto last = value.find_last_not_of(whitespace);
    return std::wstring(value.substr(first, last - first + 1));
}

std::string trim_ascii(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(whitespace);
    return std::string(value.substr(first, last - first + 1));
}

bool read_properties(const std::filesystem::path& path,
    std::unordered_map<std::string, std::string>& properties, std::string& error) {
    std::error_code file_error;
    const auto size = std::filesystem::file_size(path, file_error);
    if (file_error || size > maximum_properties_size) {
        error = "Configuration file is unavailable or exceeds the 64 KiB limit: " +
            path.string();
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not read configuration file: " + path.string();
        return false;
    }
    std::string line;
    while (std::getline(input, line)) {
        auto value = trim_ascii(line);
        if (value.empty() || value.front() == '#' || value.front() == ';') continue;
        const auto separator = value.find('=');
        if (separator == std::string::npos) continue;
        auto key = trim_ascii(std::string_view(value).substr(0, separator));
        auto field = trim_ascii(std::string_view(value).substr(separator + 1));
        if (key.empty()) continue;
        if (!properties.emplace(std::move(key), std::move(field)).second) {
            error = "Configuration file contains a duplicate key: " + path.string();
            return false;
        }
    }
    if (input.bad()) {
        error = "Failed while reading configuration file: " + path.string();
        return false;
    }
    return true;
}

bool required_property(const std::unordered_map<std::string, std::string>& properties,
    std::string_view key, std::string_view expected, std::string_view source,
    std::string& error) {
    const auto found = properties.find(std::string(key));
    if (found == properties.end() || found->second != expected) {
        error = std::string(source) + " must declare " + std::string(key) + "=" +
            std::string(expected) + ".";
        return false;
    }
    return true;
}

bool canonical_existing_path(const std::filesystem::path& input, bool directory,
    std::filesystem::path& output, std::string& error) {
    const DWORD flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(input.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "Could not open configured path for identity verification: " +
            std::string(input.string());
        return false;
    }
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(),
        static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    const DWORD path_error = length == 0 || length >= buffer.size()
        ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(handle);
    if (length == 0 || length >= buffer.size()) {
        error = "Could not canonicalize configured path; Win32 error " +
            std::to_string(path_error) + ".";
        return false;
    }
    std::wstring canonical(buffer.data(), length);
    if (canonical.starts_with(L"\\\\?\\UNC\\")) {
        canonical = L"\\\\" + canonical.substr(8);
    } else if (canonical.starts_with(L"\\\\?\\")) {
        canonical.erase(0, 4);
    }
    output = std::filesystem::path(std::move(canonical));
    return true;
}

bool path_is_below(const std::filesystem::path& parent,
    const std::filesystem::path& child) {
    std::wstring parent_text = parent.native();
    std::wstring child_text = child.native();
    if (parent_text.empty() || child_text.size() <= parent_text.size()) return false;
    if (parent_text.back() != L'\\') parent_text.push_back(L'\\');
    if (child_text.size() < parent_text.size() ||
        CompareStringOrdinal(parent_text.data(), static_cast<int>(parent_text.size()),
            child_text.data(), static_cast<int>(parent_text.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    return child_text.size() > parent_text.size();
}

bool utf8_to_wide(std::string_view input, std::wstring& output) {
    if (input.empty() || input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), output.data(), count) == count;
}

std::string wide_to_utf8(std::wstring_view input) {
    if (input.empty() || input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string output(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), output.data(), count, nullptr, nullptr) != count) return {};
    return output;
}

struct WindowCloseContext {
    HANDLE job{};
    const std::vector<JobMembershipResult::ProcessIdentity>* members{};
    bool sent{};
    std::string error;
};

BOOL CALLBACK close_verified_job_window(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<WindowCloseContext*>(parameter);
    if (GetWindow(window, GW_OWNER) != nullptr || !IsWindowVisible(window)) return TRUE;
    DWORD process_id{};
    GetWindowThreadProcessId(window, &process_id);
    const auto expected = std::find_if(context->members->begin(), context->members->end(),
        [&](const auto& member) { return member.process_id == process_id; });
    if (expected == context->members->end()) return TRUE;

    ScopedHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
        FALSE, process_id));
    if (process.get() == nullptr) {
        const DWORD native_error = GetLastError();
        if (native_error != ERROR_INVALID_PARAMETER) {
            context->error = "Could not reopen the verified Job window process; Win32 error " +
                std::to_string(native_error) + ".";
        }
        return TRUE;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user) == 0) {
        context->error = "Could not revalidate the Job window process creation time; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return TRUE;
    }
    if (filetime_value(created) != expected->creation_time) return TRUE;
    BOOL in_job{};
    if (IsProcessInJob(process.get(), context->job, &in_job) == 0) {
        context->error = "Could not revalidate exact Job membership before WM_CLOSE; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return TRUE;
    }
    if (in_job == FALSE || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return TRUE;
    if (PostMessageW(window, WM_CLOSE, 0, 0) == 0) {
        context->error = "Could not post WM_CLOSE to the verified Job window; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return TRUE;
    }
    context->sent = true;
    return FALSE;
}

}

bool valid_reference_avd_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '_' || value == '-';
    });
}

bool valid_emulator_port(std::uint16_t port) noexcept {
    return port >= 1024 && port <= 65534 && (port % 2 == 0);
}

std::optional<ReferenceRuntimeConfig> resolve_reference_runtime_config(
        std::string_view avd_name, std::string& error) {
    error.clear();
    if (!valid_reference_avd_name(avd_name)) {
        error = "AVD name must contain only ASCII letters, digits, underscore, or hyphen.";
        return std::nullopt;
    }

    std::wstring sdk_text = environment_value(L"ANDROID_SDK_ROOT");
    if (sdk_text.empty()) sdk_text = environment_value(L"ANDROID_HOME");
    if (sdk_text.empty()) {
        sdk_text = environment_value(L"LOCALAPPDATA");
        if (!sdk_text.empty()) sdk_text = (std::filesystem::path(sdk_text) / L"Android" / L"Sdk").wstring();
    }
    if (sdk_text.empty()) {
        error = "Set ANDROID_SDK_ROOT or ANDROID_HOME to the installed Android SDK directory.";
        return std::nullopt;
    }

    ReferenceRuntimeConfig config;
    config.avd_name.assign(avd_name);
    std::string path_error;
    if (!canonical_existing_path(sdk_text, true, config.sdk_root, path_error)) {
        error = "Android SDK root is unavailable: " + path_error;
        return std::nullopt;
    }
    if (!canonical_existing_path(config.sdk_root / L"emulator" / L"emulator.exe",
            false, config.emulator_executable, path_error) ||
        !canonical_existing_path(config.sdk_root / L"platform-tools" / L"adb.exe",
            false, config.adb_executable, path_error)) {
        error = "The installed SDK is missing Emulator or ADB: " + path_error;
        return std::nullopt;
    }

    std::wstring avd_root_text = environment_value(L"ANDROID_AVD_HOME");
    if (avd_root_text.empty()) {
        const auto profile = environment_value(L"USERPROFILE");
        if (!profile.empty()) avd_root_text = (std::filesystem::path(profile) / L".android" / L"avd").wstring();
    }
    std::filesystem::path avd_root;
    if (avd_root_text.empty() || !canonical_existing_path(avd_root_text, true, avd_root, path_error)) {
        error = "The configured Android AVD directory is unavailable: " + path_error;
        return std::nullopt;
    }

    std::wstring avd_name_wide;
    if (!utf8_to_wide(avd_name, avd_name_wide)) {
        error = "AVD name is not valid UTF-8.";
        return std::nullopt;
    }
    const auto descriptor_path = avd_root / (avd_name_wide + L".ini");
    std::filesystem::path canonical_descriptor;
    if (!canonical_existing_path(descriptor_path, false, canonical_descriptor, path_error) ||
        !path_is_below(avd_root, canonical_descriptor)) {
        error = "AVD descriptor is missing or resolves outside the configured AVD directory.";
        return std::nullopt;
    }
    std::unordered_map<std::string, std::string> descriptor;
    if (!read_properties(canonical_descriptor, descriptor, error)) return std::nullopt;
    const auto avd_path_entry = descriptor.find("path");
    if (avd_path_entry == descriptor.end() || avd_path_entry->second.empty()) {
        error = "AVD descriptor does not contain a resolved path.";
        return std::nullopt;
    }
    std::wstring avd_path_text;
    if (!utf8_to_wide(avd_path_entry->second, avd_path_text) ||
        !canonical_existing_path(avd_path_text, true, config.avd_directory, path_error) ||
        !path_is_below(avd_root, config.avd_directory)) {
        error = "Resolved AVD path is invalid or outside the configured AVD directory: " + path_error;
        return std::nullopt;
    }
    config.canonical_avd_location = wide_to_utf8(config.avd_directory.native());
    if (config.canonical_avd_location.empty()) {
        error = "Resolved AVD directory could not be represented as a UTF-8 identity.";
        return std::nullopt;
    }
    if (!canonical_existing_path(config.avd_directory / L"config.ini", false,
            config.avd_config_file, path_error)) {
        error = "AVD config.ini is unavailable: " + path_error;
        return std::nullopt;
    }
    std::unordered_map<std::string, std::string> avd_config;
    if (!read_properties(config.avd_config_file, avd_config, error) ||
        !required_property(avd_config, "abi.type", "x86_64", "AVD config.ini", error)) {
        return std::nullopt;
    }
    const auto image_entry = avd_config.find("image.sysdir.1");
    if (image_entry == avd_config.end() || image_entry->second.empty() ||
        image_entry->second.find("..") != std::string::npos) {
        error = "AVD config.ini has a missing or unsafe system-image path.";
        return std::nullopt;
    }
    std::wstring image_relative;
    if (!utf8_to_wide(image_entry->second, image_relative)) {
        error = "AVD system-image path is not valid UTF-8.";
        return std::nullopt;
    }
    std::filesystem::path image_candidate(image_relative);
    if (image_candidate.is_absolute()) {
        error = "AVD system-image path must be relative to the configured SDK.";
        return std::nullopt;
    }
    if (!canonical_existing_path(config.sdk_root / image_candidate, true,
            config.system_image_directory, path_error) ||
        !path_is_below(config.sdk_root / L"system-images", config.system_image_directory)) {
        error = "AVD system-image path is missing or resolves outside the SDK system-images directory.";
        return std::nullopt;
    }
    std::unordered_map<std::string, std::string> image_properties;
    if (!read_properties(config.system_image_directory / L"source.properties",
            image_properties, error) ||
        !required_property(image_properties, "AndroidVersion.ApiLevel", "36",
            "Android system image", error) ||
        !required_property(image_properties, "SystemImage.Abi", "x86_64",
            "Android system image", error) ||
        !required_property(image_properties, "SystemImage.TagId", "default",
            "Android system image", error)) {
        return std::nullopt;
    }
    const auto graphics = avd_config.find("hw.gpu.mode");
    if (graphics != avd_config.end()) config.graphics_mode = graphics->second;
    if (!valid_emulator_port(config.emulator_port)) {
        error = "Configured emulator port " + std::to_string(config.emulator_port) + " is invalid.";
        return std::nullopt;
    }
    return config;
}

std::vector<std::wstring> reference_emulator_arguments(
        const ReferenceRuntimeConfig& config) {
    std::wstring avd_name_wide;
    if (!utf8_to_wide(config.avd_name, avd_name_wide)) {
        avd_name_wide.assign(config.avd_name.begin(), config.avd_name.end());
    }

    std::vector<std::wstring> args = {
        L"-avd", avd_name_wide,
        L"-no-snapshot",
        L"-no-audio",
        L"-no-boot-anim",
        L"-port", std::to_wstring(config.emulator_port),
        L"-share-vid"
    };

    if (!config.graphics_mode.empty()) {
        std::wstring graphics_wide;
        if (!utf8_to_wide(config.graphics_mode, graphics_wide)) {
            graphics_wide.assign(config.graphics_mode.begin(), config.graphics_mode.end());
        }
        args.push_back(L"-gpu");
        args.push_back(graphics_wide);
    }

    return args;
}

ReferenceRuntimeProcess::~ReferenceRuntimeProcess() {
    if (thread_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(thread_handle_));
    if (process_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(process_handle_));
}

std::shared_ptr<ReferenceRuntimeProcess> ReferenceRuntimeProcess::create_suspended(
        const ReferenceRuntimeConfig& config, INamedJobObject& abstraction,
        std::string& error) {
    error.clear();
    if (!valid_emulator_port(config.emulator_port)) {
        error = "Emulator port " + std::to_string(config.emulator_port) + " is invalid.";
        return nullptr;
    }
    auto* job = dynamic_cast<NativeJobObject*>(&abstraction);
    if (job == nullptr || job->native_handle() == nullptr) {
        error = "The reference runtime launcher did not receive a validated native Job.";
        return nullptr;
    }
    auto runtime = std::shared_ptr<ReferenceRuntimeProcess>(new ReferenceRuntimeProcess());
    SIZE_T attribute_bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    const DWORD size_error = GetLastError();
    if (attribute_bytes == 0 || size_error != ERROR_INSUFFICIENT_BUFFER) {
        error = "Windows does not support the Job-list process-creation attribute; Win32 error " +
            std::to_string(size_error) + ".";
        return nullptr;
    }
    auto attributes_storage = std::make_unique<std::byte[]>(attribute_bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_storage.get());
    if (InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes) == 0) {
        error = "Could not initialize the Job-list process-creation attribute; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return nullptr;
    }
    struct AttributeListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeListGuard() { if (value != nullptr) DeleteProcThreadAttributeList(value); }
    } attribute_guard{attributes};
    HANDLE job_handle = static_cast<HANDLE>(job->native_handle());
    HANDLE job_list[] = {job_handle};
    if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
            job_list, sizeof(job_list), nullptr, nullptr) == 0) {
        error = "Could not bind the Emulator to its Job at process creation; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return nullptr;
    }

    const auto arguments = reference_emulator_arguments(config);
    auto command_line = build_command_line(config.emulator_executable, arguments);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_SHOWNORMAL;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION information{};
    if (CreateProcessW(config.emulator_executable.c_str(), command_line.data(),
            nullptr, nullptr, FALSE, CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, config.emulator_executable.parent_path().c_str(),
            &startup.StartupInfo, &information) == 0) {
        error = "Could not create the suspended Emulator in its Job; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return nullptr;
    }
    runtime->process_handle_ = information.hProcess;
    runtime->thread_handle_ = information.hThread;
    runtime->process_id_ = information.dwProcessId;

    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(information.hProcess, &created, &exited, &kernel, &user) == 0) {
        error = "Emulator was created inside the Job, but its creation time could not be read; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return runtime;
    }
    runtime->creation_time_ = filetime_value(created);
    return runtime;
}

bool ReferenceRuntimeProcess::resume(std::string& error) {
    error.clear();
    if (process_handle_ == nullptr || thread_handle_ == nullptr) {
        error = "The suspended Emulator process or primary thread handle is unavailable.";
        return false;
    }
    if (resumed_) {
        error = "The Emulator primary thread was already resumed.";
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const DWORD previous_count = ResumeThread(static_cast<HANDLE>(thread_handle_));
        if (previous_count == static_cast<DWORD>(-1)) {
            error = "Could not resume the Emulator in its verified Job; Win32 error " +
                std::to_string(GetLastError()) + ".";
            return false;
        }
        if (previous_count == 1) {
            resumed_ = true;
            return close_primary_thread(error);
        }
        if (previous_count == 0) {
            resumed_ = true;
            error = "The Emulator primary thread was already running in its verified Job.";
            return false;
        }
    }
    error = "The Emulator primary thread remained suspended after bounded resume attempts; it remains contained in its Job.";
    return false;
}

bool ReferenceRuntimeProcess::is_running(std::string& error) const {
    error.clear();
    if (process_handle_ == nullptr) return false;
    const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(process_handle_), 0);
    if (wait == WAIT_TIMEOUT) return true;
    if (wait == WAIT_OBJECT_0) return false;
    error = "Could not inspect the Emulator root process; Win32 error " +
        std::to_string(GetLastError()) + ".";
    return false;
}

bool ReferenceRuntimeProcess::exit_code(std::uint32_t& value, std::string& error) const {
    error.clear();
    DWORD native_exit_code{};
    if (process_handle_ == nullptr ||
        GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &native_exit_code) == 0) {
        error = "Could not query the Emulator root exit code; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return false;
    }
    value = native_exit_code;
    return true;
}

bool ReferenceRuntimeProcess::close_primary_thread(std::string& error) {
    error.clear();
    if (thread_handle_ == nullptr) return true;
    if (CloseHandle(static_cast<HANDLE>(thread_handle_)) == 0) {
        error = "Could not close the Emulator primary thread handle; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return false;
    }
    thread_handle_ = nullptr;
    return true;
}

ReferenceCommandResult run_reference_adb(const ReferenceRuntimeConfig& config,
        const std::vector<std::wstring>& arguments, std::uint32_t timeout_ms) {
    ReferenceCommandResult result;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE raw_read{};
    HANDLE raw_write{};
    if (CreatePipe(&raw_read, &raw_write, &security, 0) == 0) {
        result.native_error = GetLastError();
        result.standard_error = "Could not create bounded ADB output pipes.";
        return result;
    }
    ScopedHandle read_pipe(raw_read);
    ScopedHandle write_pipe(raw_write);
    if (SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0) == 0) {
        result.native_error = GetLastError();
        result.standard_error = "Could not protect the ADB output pipe from inheritance.";
        return result;
    }
    ScopedHandle null_input(CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (null_input.get() == INVALID_HANDLE_VALUE) {
        result.native_error = GetLastError();
        result.standard_error = "Could not open NUL for the ADB helper.";
        return result;
    }

    auto command_line = build_command_line(config.adb_executable, arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input.get();
    startup.hStdOutput = write_pipe.get();
    startup.hStdError = write_pipe.get();
    PROCESS_INFORMATION information{};
    if (CreateProcessW(config.adb_executable.c_str(), command_line.data(), nullptr,
            nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
            config.adb_executable.parent_path().c_str(), &startup, &information) == 0) {
        result.native_error = GetLastError();
        result.standard_error = "Could not start the bounded ADB helper.";
        return result;
    }
    ScopedHandle process(information.hProcess);
    ScopedHandle thread(information.hThread);
    write_pipe.reset();
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    std::array<char, 4096> buffer{};
    bool finished{};
    while (GetTickCount64() < deadline) {
        DWORD available{};
        if (PeekNamedPipe(read_pipe.get(), nullptr, 0, nullptr, &available, nullptr) != 0 &&
            available > 0) {
            DWORD received{};
            if (ReadFile(read_pipe.get(), buffer.data(),
                    std::min<DWORD>(available, static_cast<DWORD>(buffer.size())),
                    &received, nullptr) != 0) {
                append_bounded(result.standard_output, buffer.data(), received,
                    config.diagnostic_limit_bytes);
            }
        }
        const DWORD wait = WaitForSingleObject(process.get(), 0);
        if (wait == WAIT_OBJECT_0) {
            finished = true;
            break;
        }
        if (wait == WAIT_FAILED) {
            result.native_error = GetLastError();
            result.standard_error += "Could not wait for the ADB helper process.";
            break;
        }
        Sleep(20);
    }
    if (!finished && result.native_error == ERROR_SUCCESS) {
        result.timed_out = true;
        result.standard_error = "ADB helper exceeded its bounded timeout.";
        TerminateProcess(process.get(), ERROR_TIMEOUT);
        WaitForSingleObject(process.get(), 1000);
        return result;
    }
    DWORD available{};
    while (PeekNamedPipe(read_pipe.get(), nullptr, 0, nullptr, &available, nullptr) != 0 &&
            available > 0) {
        DWORD received{};
        if (ReadFile(read_pipe.get(), buffer.data(),
                std::min<DWORD>(available, static_cast<DWORD>(buffer.size())),
                &received, nullptr) == 0 || received == 0) break;
        append_bounded(result.standard_output, buffer.data(), received,
            config.diagnostic_limit_bytes);
    }
    DWORD native_exit_code{};
    if (GetExitCodeProcess(process.get(), &native_exit_code) == 0) {
        result.native_error = GetLastError();
        result.standard_error += " Could not read the ADB helper exit code.";
        return result;
    }
    result.exit_code = native_exit_code;
    result.succeeded = result.exit_code == 0 && result.native_error == ERROR_SUCCESS;
    return result;
}

namespace {

struct VerifiedWindowSearch {
    HANDLE job{};
    const std::vector<JobMembershipResult::ProcessIdentity>* members{};
    bool sent{};
    std::string error;
};

BOOL CALLBACK post_close_to_verified_member(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<VerifiedWindowSearch*>(parameter);
    if (GetWindow(window, GW_OWNER) != nullptr || !IsWindowVisible(window)) return TRUE;
    DWORD process_id{};
    GetWindowThreadProcessId(window, &process_id);
    const auto member = std::find_if(search->members->begin(), search->members->end(),
        [&](const auto& candidate) { return candidate.process_id == process_id; });
    if (member == search->members->end()) return TRUE;

    ScopedHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
        FALSE, process_id));
    if (process.get() == nullptr) {
        const DWORD native_error = GetLastError();
        if (native_error != ERROR_INVALID_PARAMETER) {
            search->error = "Could not reopen the verified Job window process; Win32 error " +
                std::to_string(native_error) + ".";
        }
        return TRUE;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user) == 0) {
        search->error = "Could not revalidate Job window creation time; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return TRUE;
    }
    if (filetime_value(created) != member->creation_time) return TRUE;
    BOOL in_job{};
    if (IsProcessInJob(process.get(), search->job, &in_job) == 0) {
        search->error = "Could not revalidate exact Job membership before WM_CLOSE; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return TRUE;
    }
    if (in_job == FALSE || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return TRUE;
    if (PostMessageW(window, WM_CLOSE, 0, 0) == 0) {
        search->error = "Could not post WM_CLOSE to the verified Job window; Win32 error " +
            std::to_string(GetLastError()) + ".";
        return TRUE;
    }
    search->sent = true;
    return FALSE;
}

}

bool post_graceful_close_to_job_window(INamedJobObject& abstraction,
        const std::vector<JobMembershipResult::ProcessIdentity>& verified_members,
        std::string& error) {
    error.clear();
    auto* job = dynamic_cast<NativeJobObject*>(&abstraction);
    if (job == nullptr || job->native_handle() == nullptr) {
        error = "No validated native Job handle is available for graceful shutdown.";
        return false;
    }
    VerifiedWindowSearch search{static_cast<HANDLE>(job->native_handle()), &verified_members};
    EnumWindows(post_close_to_verified_member, reinterpret_cast<LPARAM>(&search));
    if (!search.error.empty()) {
        error = std::move(search.error);
        return false;
    }
    if (!search.sent) {
        error = "No visible top-level window belonging to a freshly verified live Job member was found.";
        return false;
    }
    return true;
}

}