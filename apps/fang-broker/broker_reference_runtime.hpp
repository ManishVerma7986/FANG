#pragma once

#include "broker_native_job.hpp"
#include "broker_protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fang::broker {

struct ReferenceRuntimeConfig {
    std::uint16_t configuration_version{reference_runtime_configuration_version};
    std::string avd_name;
    std::string canonical_avd_location;
    std::filesystem::path sdk_root;
    std::filesystem::path emulator_executable;
    std::filesystem::path adb_executable;
    std::filesystem::path avd_directory;
    std::filesystem::path avd_config_file;
    std::filesystem::path system_image_directory;
    std::string system_image_package{"system-images;android-36;default;x86_64"};
    std::string abi{"x86_64"};
    std::string graphics_mode;
    std::uint16_t emulator_port{5554};
    std::uint32_t readiness_timeout_ms{240000};
    std::uint32_t shutdown_timeout_ms{30000};
    std::uint32_t polling_interval_ms{500};
    std::uint32_t adb_command_timeout_ms{4000};
    std::uint32_t diagnostic_limit_bytes{32768};
};

struct ReferenceCommandResult {
    bool succeeded{};
    bool timed_out{};
    std::uint32_t exit_code{};
    std::uint32_t native_error{};
    std::string standard_output;
    std::string standard_error;
};

class ReferenceRuntimeProcess final {
public:
    ReferenceRuntimeProcess(const ReferenceRuntimeProcess&) = delete;
    ReferenceRuntimeProcess& operator=(const ReferenceRuntimeProcess&) = delete;
    ~ReferenceRuntimeProcess();

    static std::shared_ptr<ReferenceRuntimeProcess> create_suspended(
        const ReferenceRuntimeConfig& config, INamedJobObject& job, std::string& error);
    bool resume(std::string& error);
    bool is_running(std::string& error) const;
    bool exit_code(std::uint32_t& value, std::string& error) const;
    bool close_primary_thread(std::string& error);
    bool resumed() const noexcept { return resumed_; }
    std::uint32_t process_id() const noexcept { return process_id_; }
    std::uint64_t creation_time() const noexcept { return creation_time_; }
    std::uint32_t exit_code_value() const noexcept { return exit_code_; }

private:
    ReferenceRuntimeProcess() = default;
    void* process_handle_{};
    void* thread_handle_{};
    std::uint32_t process_id_{};
    std::uint64_t creation_time_{};
    std::uint32_t exit_code_{};
    bool resumed_{};
};

bool valid_reference_avd_name(std::string_view name) noexcept;
bool valid_emulator_port(std::uint16_t port) noexcept;
std::optional<ReferenceRuntimeConfig> resolve_reference_runtime_config(
    std::string_view avd_name, std::string& error);
std::vector<std::wstring> reference_emulator_arguments(
    const ReferenceRuntimeConfig& config);
ReferenceCommandResult run_reference_adb(const ReferenceRuntimeConfig& config,
    const std::vector<std::wstring>& arguments, std::uint32_t timeout_ms);
bool post_graceful_close_to_job_window(INamedJobObject& job,
    const std::vector<JobMembershipResult::ProcessIdentity>& verified_members,
    std::string& error);

}