#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "video_frame.hpp"
#include "video_transport.hpp"
#include "supervisor_runtime_client.hpp"
#include "broker_ownership.hpp"
#include "broker_native_journal.hpp"
#include "broker_reference_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fang::video_smoke {

// Configuration for the video-only smoke test
struct VideoSmokeConfig {
    std::string avd_name;
    std::uint16_t emulator_port{5554};
    std::uint32_t readiness_timeout_ms{240000}; // Bounded to 240 seconds
    std::uint32_t max_observation_ms{60000};    // Bounded to 60 seconds
    std::uint32_t max_accepted_frames{100};     // Bounded to 100 frames
    std::uint32_t shutdown_timeout_ms{45000};   // Bounded to 45 seconds
};

// Validate configuration parameters
bool validate_video_smoke_config(const VideoSmokeConfig& config, std::string& error);

// Status of each lifecycle stage
enum class SmokeStageStatus {
    Pass,
    Fail,
    Blocked,
    NotRun
};

const char* stage_status_string(SmokeStageStatus status) noexcept;

struct SmokeStageResult {
    std::string name;
    SmokeStageStatus status{SmokeStageStatus::NotRun};
    std::uint64_t elapsed_ms{0};
    std::string details;
};

// State transitions observed during supervisor readiness wait
struct StateTransition {
    std::string state;
    std::uint64_t timestamp_ms{0};
};

// Explicit unavailable observation record (Phase 4 requirement)
struct UnavailableField {
    std::string field_name;
    std::string reason;
};

// Comprehensive evidence report schema (Phase 4 requirement)
struct VideoSmokeReport {
    std::string schema_version{"1.0"};
    std::string run_id;
    std::string run_timestamp;

    // Target configuration
    std::string avd_name;
    std::uint16_t emulator_port{0};
    std::string expected_serial;
    std::string expected_mapping;
    std::string emulator_version;
    std::string emulator_build_id;

    // Preflight checks
    bool port_free{false};
    bool serial_not_active{false};
    bool mapping_initially_absent{false};
    bool journal_initially_clean{false};

    // Lifecycle evidence
    std::string generation_id;
    std::uint32_t owned_process_id{0};
    std::uint64_t process_creation_time{0};
    bool containment_verified{false};
    std::uint32_t active_processes_in_job{0};
    std::vector<StateTransition> state_transitions;
    std::uint64_t readiness_elapsed_ms{0};
    bool guest_ready_observed{false};

    // Mapping and header evidence
    bool mapping_open_succeeded{false};
    std::string mapping_open_error;
    std::optional<std::uint32_t> header_width;
    std::optional<std::uint32_t> header_height;
    std::optional<std::uint32_t> header_reported_fps; // Header metadata field; NOT measured rendering rate
    std::optional<std::uint32_t> initial_frame_counter;
    std::optional<std::uint32_t> final_frame_counter;

    // Frame observation metrics
    std::uint64_t observation_duration_ms{0};
    std::size_t accepted_frames{0};
    std::size_t dropped_frames{0};
    std::size_t torn_frames{0};
    std::size_t counter_discontinuities{0};
    bool observation_stopped_by_frame_limit{false};
    bool observation_stopped_by_deadline{false};

    // Viewport rendering observation
    bool viewport_rendering_confirmed{false};
    std::string viewport_rendering_note{"Headless CLI test seam; native Win32 GUI viewport is not instantiated."};

    // Shutdown and cleanup evidence
    bool shutdown_requested{false};
    std::uint64_t shutdown_elapsed_ms{0};
    bool process_exit_confirmed{false};
    std::string process_exit_detail;
    bool containment_empty_verified{false};
    std::string journal_final_state;
    bool mapping_released_verified{false};
    bool cleanup_verified{false};
    std::string cleanup_failure_reason;

    // Explicit unavailable observations
    std::vector<UnavailableField> unavailable_fields;

    // Overall outcome
    std::string outcome; // PASS, PARTIAL, FAIL, BLOCKED, INCOMPLETE
    std::string failure_reason;
};

// Format report to JSON
std::string format_json_report(const VideoSmokeReport& report);

// Validate that a JSON string conforms to the report schema
bool validate_report_schema(std::string_view json_text, std::string& error);

// Safe, collision-resistant report persistence
struct ReportPersistenceOptions {
    std::string output_path;
    bool allow_overwrite{false};
};

struct ReportPersistenceResult {
    bool succeeded{false};
    std::string resolved_path;
    std::string error;
};

ReportPersistenceResult persist_json_report(
    const VideoSmokeReport& report,
    const ReportPersistenceOptions& options);

// Process exit verification by identity (PID + 64-bit creation timestamp)
enum class ProcessExitStatus {
    ConfirmedExited,
    StillRunning,
    RecycledPidExited,
    VerificationUnavailable
};

struct ProcessExitVerificationResult {
    ProcessExitStatus status{ProcessExitStatus::VerificationUnavailable};
    std::string detail;
};

ProcessExitVerificationResult verify_process_exit_by_identity(
    std::uint32_t process_id,
    std::uint64_t expected_creation_time);

// Unified post-launch cleanup execution
struct PostLaunchCleanupResult {
    bool shutdown_requested{false};
    bool shutdown_succeeded{false};
    std::uint64_t shutdown_elapsed_ms{0};
    ProcessExitVerificationResult process_exit;
    bool containment_empty_verified{false};
    std::string journal_final_state;
    bool mapping_released_verified{false};
    bool cleanup_fully_verified{false};
    std::string failure_reason;
};

PostLaunchCleanupResult execute_post_launch_cleanup(
    std::function<bool()> request_shutdown_fn,
    std::function<fang::studio::SupervisorRuntimeSnapshot()> snapshot_fn,
    const std::function<fang::broker::JournalLoadResult()>& load_journal_fn,
    const std::wstring& wide_mapping,
    std::uint32_t process_id,
    std::uint64_t process_creation_time,
    std::uint32_t shutdown_timeout_ms,
    std::function<ProcessExitVerificationResult(std::uint32_t, std::uint64_t)> verify_proc_exit_fn = nullptr,
    std::function<bool(const std::wstring&)> check_mapping_released_fn = nullptr,
    std::function<std::uint64_t()> now_ms_fn = nullptr,
    std::function<void(std::uint32_t)> sleep_ms_fn = nullptr);

void apply_cleanup_result_to_report(
    const PostLaunchCleanupResult& cleanup,
    VideoSmokeReport& report);

// Bounded video observation engine: monitors consumer until frame count or deadline reached
struct ObservationLimits {
    std::uint32_t max_frames{100};
    std::uint32_t max_duration_ms{60000};
};

struct ObservationResult {
    bool succeeded{false};
    bool reached_frame_limit{false};
    bool reached_deadline{false};
    std::uint64_t duration_ms{0};
    std::size_t accepted_frames{0};
    std::size_t dropped_frames{0};
    std::size_t torn_frames{0};
    std::size_t counter_discontinuities{0};
    std::uint32_t final_sequence{0};
    std::string error;
};

ObservationResult run_video_observation(
    fang::studio::IVideoTransport& transport,
    fang::studio::StudioVideoConsumer& consumer,
    ObservationLimits limits,
    std::function<std::uint64_t()> now_ms_fn = nullptr,
    std::function<void(std::uint32_t)> sleep_ms_fn = nullptr);

// Supervisor lifecycle waiter: monitors supervisor snapshot until Ready + guest_ready or timeout
struct ReadinessWaitResult {
    bool succeeded{false};
    bool timed_out{false};
    std::uint64_t elapsed_ms{0};
    std::string generation_id;
    std::uint32_t process_id{0};
    std::uint64_t process_creation_time{0};
    std::uint32_t active_processes{0};
    bool containment_verified{false};
    std::vector<StateTransition> transitions;
    std::string error;
};

ReadinessWaitResult wait_for_readiness(
    std::function<fang::studio::SupervisorRuntimeSnapshot()> snapshot_fn,
    std::uint32_t timeout_ms,
    std::function<std::uint64_t()> now_ms_fn = nullptr,
    std::function<void(std::uint32_t)> sleep_ms_fn = nullptr);

// Shutdown waiter: monitors supervisor snapshot until Stopped or timeout
struct ShutdownWaitResult {
    bool succeeded{false};
    bool timed_out{false};
    std::uint64_t elapsed_ms{0};
    std::string error;
};

ShutdownWaitResult wait_for_shutdown(
    std::function<fang::studio::SupervisorRuntimeSnapshot()> snapshot_fn,
    std::uint32_t timeout_ms,
    std::function<std::uint64_t()> now_ms_fn = nullptr,
    std::function<void(std::uint32_t)> sleep_ms_fn = nullptr);

// CLI argument parsing
bool parse_video_smoke_options(int argc, wchar_t* argv[], VideoSmokeConfig& config,
    std::string& output_json_path, bool& allow_overwrite, std::string& error);

// Helper for generating UUID string for run IDs
std::string generate_run_uuid();

// Helper for formatting current timestamp as ISO 8601 UTC
std::string current_iso8601_timestamp();

} // namespace fang::video_smoke
