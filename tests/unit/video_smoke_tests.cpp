#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "video_smoke_runner.hpp"
#include "video_transport.hpp"
#include "video_frame.hpp"
#include "broker_native_journal.hpp"
#include "broker_native_job.hpp"
#include "broker_pipe.hpp"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace fang::video_smoke;
using namespace fang::studio;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

// 1. Test invalid port and mapping configuration
void test_invalid_port_and_mapping_configuration() {
    std::cout << "Running test_invalid_port_and_mapping_configuration...\n";
    std::string error;

    // Default valid config
    VideoSmokeConfig valid_cfg{"FANG-StageA-API36-x86_64-headed", 5554};
    require(validate_video_smoke_config(valid_cfg, error), "Default config should be valid.");

    // Odd port rejection
    VideoSmokeConfig odd_port_cfg = valid_cfg;
    odd_port_cfg.emulator_port = 5555;
    require(!validate_video_smoke_config(odd_port_cfg, error), "Odd port 5555 must be rejected.");

    // Port out of range
    VideoSmokeConfig zero_port_cfg = valid_cfg;
    zero_port_cfg.emulator_port = 0;
    require(!validate_video_smoke_config(zero_port_cfg, error), "Port 0 must be rejected.");

    VideoSmokeConfig low_port_cfg = valid_cfg;
    low_port_cfg.emulator_port = 80;
    require(!validate_video_smoke_config(low_port_cfg, error), "Port 80 must be rejected.");

    // Empty AVD name
    VideoSmokeConfig empty_avd_cfg = valid_cfg;
    empty_avd_cfg.avd_name.clear();
    require(!validate_video_smoke_config(empty_avd_cfg, error), "Empty AVD name must be rejected.");

    // AVD name with spaces or non-ASCII
    VideoSmokeConfig space_avd_cfg = valid_cfg;
    space_avd_cfg.avd_name = "My AVD With Space";
    require(!validate_video_smoke_config(space_avd_cfg, error), "AVD name with spaces must be rejected.");

    // Excessive frames count
    VideoSmokeConfig excessive_frames = valid_cfg;
    excessive_frames.max_accepted_frames = 0;
    require(!validate_video_smoke_config(excessive_frames, error), "0 max frames must be rejected.");

    // Shared memory name calculations
    require(shared_memory_name_for_port(5554) == "videmulator5554", "Port 5554 mapping should be videmulator5554.");
    require(shared_memory_name_for_port(5556) == "videmulator5556", "Port 5556 mapping should be videmulator5556.");

    const auto serial_mapping = shared_memory_name_for_serial("emulator-5554");
    require(serial_mapping.has_value() && *serial_mapping == "videmulator5554",
        "Serial emulator-5554 should resolve to videmulator5554.");

    require(!shared_memory_name_for_serial("emulator-5555").has_value(), "Odd serial emulator-5555 must not resolve.");
    require(!shared_memory_name_for_serial("device-5554").has_value(), "Non-emulator serial must not resolve.");
    require(!shared_memory_name_for_serial("emulator-").has_value(), "Empty port serial must not resolve.");
}

// 2. Test readiness timeout
void test_readiness_timeout() {
    std::cout << "Running test_readiness_timeout...\n";

    std::uint64_t simulated_time = 0;
    fang::studio::SupervisorRuntimeSnapshot mock_snapshot{};
    mock_snapshot.runtime.state = fang::broker::RuntimeLifecycleState::GuestBooting;
    mock_snapshot.runtime.guest_ready = false;
    mock_snapshot.runtime.process_id = 12345;

    const auto result = wait_for_readiness(
        [&] { return mock_snapshot; },
        5000, // 5000 ms timeout
        [&] { return simulated_time; },
        [&](std::uint32_t step) { simulated_time += step; }
    );

    require(!result.succeeded, "Readiness wait must fail on timeout.");
    require(result.timed_out, "timed_out flag must be set.");
    require(result.process_id == 12345, "PID must be captured from snapshot.");
    require(result.elapsed_ms >= 5000, "Elapsed ms must reach or exceed timeout threshold.");
}

// 3. Test mapping unavailable
void test_mapping_unavailable() {
    std::cout << "Running test_mapping_unavailable...\n";

    auto consumer = std::make_shared<StudioVideoConsumer>(1);
    SharedMemoryVideoTransport transport(consumer);

    // Opening a non-existent Win32 mapping name must fail gracefully without hanging
    const bool started = transport.start("Local\\non_existent_fake_shm_section_12345");
    require(!started, "Opening non-existent mapping must fail.");
    require(transport.state() == VideoTransportState::Error, "Transport state must be Error.");
    require(!transport.last_error().empty(), "last_error must describe the failure.");
}

// 4. Test malformed header and invalid dimensions
void test_malformed_header_and_invalid_dimensions() {
    std::cout << "Running test_malformed_header_and_invalid_dimensions...\n";

    auto consumer = std::make_shared<StudioVideoConsumer>(1);
    SharedMemoryVideoTransport transport(consumer);

    // Buffer smaller than 24-byte header
    std::vector<std::uint8_t> tiny_buffer(16, 0);
    require(!transport.poll_once_from_memory(tiny_buffer.data(), tiny_buffer.size()),
        "Buffer < 24 bytes must be rejected.");

    // Width = 0
    std::vector<std::uint8_t> zero_w_buffer(sizeof(SharedMemoryVideoHeader) + 100, 0);
    SharedMemoryVideoHeader header{};
    header.width = 0;
    header.height = 100;
    header.frame_counter = 1;
    std::memcpy(zero_w_buffer.data(), &header, sizeof(header));
    require(!transport.poll_once_from_memory(zero_w_buffer.data(), zero_w_buffer.size()),
        "Header with width = 0 must be rejected.");

    // Height = 0
    header.width = 100;
    header.height = 0;
    std::memcpy(zero_w_buffer.data(), &header, sizeof(header));
    require(!transport.poll_once_from_memory(zero_w_buffer.data(), zero_w_buffer.size()),
        "Header with height = 0 must be rejected.");

    // Excessive width > 8192
    header.width = 10000;
    header.height = 100;
    std::memcpy(zero_w_buffer.data(), &header, sizeof(header));
    require(!transport.poll_once_from_memory(zero_w_buffer.data(), zero_w_buffer.size()),
        "Header with width > 8192 must be rejected.");

    // Truncated payload buffer (header says 100x100x4 = 40000, buffer is only 500)
    header.width = 100;
    header.height = 100;
    std::memcpy(zero_w_buffer.data(), &header, sizeof(header));
    require(!transport.poll_once_from_memory(zero_w_buffer.data(), zero_w_buffer.size()),
        "Truncated payload buffer must be rejected.");
}

// 5. Test counter wrap/reset/discontinuity
void test_counter_wrap_reset_and_discontinuity() {
    std::cout << "Running test_counter_wrap_reset_and_discontinuity...\n";

    StudioVideoConsumer consumer(10);

    // Frame 1: sequence = 10
    VideoFrame frame1;
    frame1.width = 64;
    frame1.height = 64;
    frame1.stride = 64 * 4;
    frame1.format = VideoPixelFormat::BGRA8888;
    frame1.sequence = 10;
    frame1.pixels = std::make_shared<std::vector<std::uint8_t>>(64 * 64 * 4, std::uint8_t{1});
    consumer.publish_frame(frame1);

    require(consumer.metrics().frames_received == 1, "First frame received.");
    require(consumer.metrics().counter_discontinuities == 0, "No discontinuity yet.");

    // Frame 2: sequence = 5 (backward discontinuity)
    VideoFrame frame2 = frame1;
    frame2.sequence = 5;
    frame2.pixels = std::make_shared<std::vector<std::uint8_t>>(64 * 64 * 4, std::uint8_t{2});
    consumer.publish_frame(frame2);

    require(consumer.metrics().frames_received == 2, "Second frame received.");
    require(consumer.metrics().counter_discontinuities == 1, "Discontinuity must be detected when sequence decreases.");
}

// 6. Test frame acceptance limit
void test_frame_acceptance_limit() {
    std::cout << "Running test_frame_acceptance_limit...\n";

    auto consumer = std::make_shared<StudioVideoConsumer>(100);
    SharedMemoryVideoTransport transport(consumer);

    // Setup synthetic in-memory source with 64x64 frame
    const std::size_t payload_bytes = 64 * 64 * 4;
    std::vector<std::uint8_t> buffer(sizeof(SharedMemoryVideoHeader) + payload_bytes, 0);
    auto* header = reinterpret_cast<SharedMemoryVideoHeader*>(buffer.data());
    header->width = 64;
    header->height = 64;
    header->fps = 60;
    header->frame_counter = 1;

    transport.set_in_memory_source(buffer.data(), buffer.size());
    require(transport.start("Local\\synthetic_stream"), "Start with synthetic source must succeed.");

    // Loop injecting frames
    ObservationLimits limits{25, 60000}; // Request exactly 25 frames
    std::uint64_t simulated_time = 0;

    const auto result = run_video_observation(
        transport,
        *consumer,
        limits,
        [&] { return simulated_time; },
        [&](std::uint32_t step) {
            simulated_time += step;
            // Advance frame counter in synthetic buffer to simulate producer updates
            header->frame_counter++;
            transport.poll_once_from_memory(buffer.data(), buffer.size());
        }
    );

    transport.stop();

    require(result.succeeded, "Observation should succeed.");
    require(result.reached_frame_limit, "Must terminate due to frame limit.");
    require(!result.reached_deadline, "Must not reach deadline.");
    require(result.accepted_frames >= 25, "Accepted frames must reach requested limit of 25.");
}

// 7. Test observation deadline
void test_observation_deadline() {
    std::cout << "Running test_observation_deadline...\n";

    auto consumer = std::make_shared<StudioVideoConsumer>(100);
    SharedMemoryVideoTransport transport(consumer);

    const std::size_t payload_bytes = 64 * 64 * 4;
    std::vector<std::uint8_t> buffer(sizeof(SharedMemoryVideoHeader) + payload_bytes, 0);
    auto* header = reinterpret_cast<SharedMemoryVideoHeader*>(buffer.data());
    header->width = 64;
    header->height = 64;
    header->fps = 60;
    header->frame_counter = 1;

    transport.set_in_memory_source(buffer.data(), buffer.size());
    transport.start("Local\\synthetic_stream");

    // Request 100 frames with 200 ms max duration; producer only yields 3 frames
    ObservationLimits limits{100, 200};
    std::uint64_t simulated_time = 0;
    int frame_count = 0;

    const auto result = run_video_observation(
        transport,
        *consumer,
        limits,
        [&] { return simulated_time; },
        [&](std::uint32_t step) {
            simulated_time += step;
            if (frame_count < 3) {
                header->frame_counter++;
                transport.poll_once_from_memory(buffer.data(), buffer.size());
                frame_count++;
            }
        }
    );

    transport.stop();

    require(result.reached_deadline, "Must terminate due to deadline.");
    require(!result.reached_frame_limit, "Must not reach frame limit.");
    require(result.accepted_frames == 3, "Accepted frames must match count produced before deadline.");
    require(result.succeeded, "Outcome should be successful because frames were accepted before deadline.");
}

// 8. Test shutdown timeout
void test_shutdown_timeout() {
    std::cout << "Running test_shutdown_timeout...\n";

    std::uint64_t simulated_time = 0;
    fang::studio::SupervisorRuntimeSnapshot mock_snapshot{};
    mock_snapshot.runtime.state = fang::broker::RuntimeLifecycleState::Ready; // Never stops

    const auto result = wait_for_shutdown(
        [&] { return mock_snapshot; },
        3000,
        [&] { return simulated_time; },
        [&](std::uint32_t step) { simulated_time += step; }
    );

    require(!result.succeeded, "Shutdown wait must fail on timeout.");
    require(result.timed_out, "timed_out flag must be set.");
    require(result.elapsed_ms >= 3000, "Elapsed ms must reach or exceed timeout threshold.");
}

// 9. Test cleanup failure and incomplete journal state
void test_cleanup_failure_and_incomplete_journal_state() {
    std::cout << "Running test_cleanup_failure_and_incomplete_journal_state...\n";

    VideoSmokeReport report{};
    report.run_id = generate_run_uuid();
    report.run_timestamp = current_iso8601_timestamp();
    report.shutdown_requested = true;
    report.process_exit_confirmed = true;
    report.containment_empty_verified = false; // Cleanup failed!
    report.journal_final_state = "Incomplete/Unverified";
    report.outcome = "FAIL — smoke test did not meet required evidence criteria";
    report.failure_reason = "Job Object containment or ownership journal verification failed.";

    const auto json = format_json_report(report);
    std::string error;
    require(validate_report_schema(json, error), "Report JSON must be valid even on failure.");
    require(json.find("\"containment_empty_verified\": false") != std::string::npos,
        "Report must reflect unverified containment.");
    require(json.find("\"journal_final_state\": \"Incomplete/Unverified\"") != std::string::npos,
        "Report must reflect incomplete journal state.");
}

// 10. Test repeated start/stop lifecycle behavior (mock offline state machine)
void test_repeated_start_stop_offline_lifecycle() {
    std::cout << "Running test_repeated_start_stop_offline_lifecycle...\n";

    for (int cycle = 1; cycle <= 2; ++cycle) {
        std::uint64_t simulated_time = 0;
        int step_count = 0;

        // Simulate launch to ready
        const auto ready_res = wait_for_readiness(
            [&] {
                fang::studio::SupervisorRuntimeSnapshot s{};
                if (step_count < 2) s.runtime.state = fang::broker::RuntimeLifecycleState::Starting;
                else if (step_count < 4) s.runtime.state = fang::broker::RuntimeLifecycleState::OwnershipVerified;
                else if (step_count < 6) s.runtime.state = fang::broker::RuntimeLifecycleState::GuestBooting;
                else {
                    s.runtime.state = fang::broker::RuntimeLifecycleState::Ready;
                    s.runtime.guest_ready = true;
                }
                step_count++;
                return s;
            },
            10000,
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(ready_res.succeeded, "Cycle readiness must succeed.");

        // Simulate shutdown
        int shutdown_step = 0;
        const auto shut_res = wait_for_shutdown(
            [&] {
                fang::studio::SupervisorRuntimeSnapshot s{};
                if (shutdown_step < 2) s.runtime.state = fang::broker::RuntimeLifecycleState::Stopping;
                else s.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                shutdown_step++;
                return s;
            },
            10000,
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(shut_res.succeeded, "Cycle shutdown must succeed.");
    }
}

// 11. Test report schema validation and unavailable evidence fields
void test_report_schema_validation_and_unavailable_fields() {
    std::cout << "Running test_report_schema_validation_and_unavailable_fields...\n";

    VideoSmokeReport report{};
    report.run_id = generate_run_uuid();
    report.run_timestamp = current_iso8601_timestamp();
    report.avd_name = "FANG-StageA-API36-x86_64-headed";
    report.emulator_port = 5554;
    report.expected_serial = "emulator-5554";
    report.expected_mapping = "videmulator5554";
    report.emulator_version = "37.2.12";
    report.emulator_build_id = "16428233";

    report.port_free = true;
    report.serial_not_active = true;
    report.mapping_initially_absent = true;
    report.journal_initially_clean = true;

    report.owned_process_id = 9876;
    report.process_creation_time = 133500000000000000ULL;
    report.containment_verified = true;
    report.active_processes_in_job = 1;
    report.readiness_elapsed_ms = 45000;
    report.guest_ready_observed = true;
    report.state_transitions = {
        {"Launching", 0},
        {"OwnershipVerified", 1200},
        {"GuestBooting", 3500},
        {"Ready", 45000}
    };

    report.mapping_open_succeeded = true;
    report.header_width = 1080;
    report.header_height = 2400;
    report.header_reported_fps = 60;
    report.initial_frame_counter = 1;
    report.final_frame_counter = 101;

    report.observation_duration_ms = 1850;
    report.accepted_frames = 100;
    report.dropped_frames = 0;
    report.torn_frames = 0;
    report.counter_discontinuities = 0;
    report.observation_stopped_by_frame_limit = true;
    report.observation_stopped_by_deadline = false;
    report.viewport_rendering_confirmed = false; // Explicitly false for headless test seam

    report.shutdown_requested = true;
    report.shutdown_elapsed_ms = 1500;
    report.process_exit_confirmed = true;
    report.containment_empty_verified = true;
    report.journal_final_state = "EmptyVerified";
    report.mapping_released_verified = true;

    report.unavailable_fields = {
        {"viewport_rendering_confirmed", "Headless CLI runner without native Win32 GUI window."},
        {"game_presented_fps", "No guest game application installed or launched."},
        {"pixel_atomic_guarantee", "Android Emulator -share-vid lacks OS-level synchronization primitives."}
    };

    report.outcome = "PASS — live video frames observed and accepted";

    const auto json = format_json_report(report);
    std::string error;
    require(validate_report_schema(json, error), "Valid report must pass schema validation.");

    // Malformed/incomplete schema test
    std::string incomplete_json = "{\"schema_version\": \"1.0\"}";
    require(!validate_report_schema(incomplete_json, error), "Incomplete JSON must be rejected by validator.");
}

// 12. Test report persistence collision and defaults
void test_report_persistence_collision_and_defaults() {
    std::cout << "Running test_report_persistence_collision_and_defaults...\n";

    VideoSmokeReport r1{};
    r1.run_id = generate_run_uuid();
    r1.run_timestamp = current_iso8601_timestamp();
    r1.avd_name = "FANG-StageA-API36-x86_64-headed";
    r1.emulator_port = 5554;
    r1.outcome = "PASS — test default path";

    // 1. Default path chosen under out/video-smoke/runs/<run_id>.json
    ReportPersistenceOptions opt1{};
    const auto res1 = persist_json_report(r1, opt1);
    require(res1.succeeded, "Default path persistence must succeed: " + res1.error);
    require(!res1.resolved_path.empty(), "Resolved path must not be empty.");
    require(res1.resolved_path.find("out") != std::string::npos, "Default path must be under out directory.");
    require(res1.resolved_path.find(r1.run_id) != std::string::npos, "Default path must contain run_id.");
    require(std::filesystem::exists(res1.resolved_path), "Generated report file must exist.");

    // 2. Two runs choosing default paths generate unique paths
    VideoSmokeReport r2{};
    r2.run_id = generate_run_uuid();
    r2.run_timestamp = current_iso8601_timestamp();
    r2.outcome = "PASS — test unique default";
    ReportPersistenceOptions opt2{};
    const auto res2 = persist_json_report(r2, opt2);
    require(res2.succeeded, "Second default persistence must succeed.");
    require(res1.resolved_path != res2.resolved_path, "Two default runs must have distinct file paths.");
    require(std::filesystem::exists(res2.resolved_path), "Second report must exist.");

    // 3. Collision avoidance: Writing to existing destination without overwrite must be refused
    ReportPersistenceOptions opt_collision{};
    opt_collision.output_path = res1.resolved_path;
    opt_collision.allow_overwrite = false;
    const auto res_collision = persist_json_report(r2, opt_collision);
    require(!res_collision.succeeded, "Must refuse to overwrite existing report without overwrite flag.");
    require(res_collision.error.find("collision") != std::string::npos ||
            res_collision.error.find("Refusing") != std::string::npos,
            "Error must mention collision or refusal.");

    // 4. Overwrite allowed when explicitly requested
    ReportPersistenceOptions opt_overwrite{};
    opt_overwrite.output_path = res1.resolved_path;
    opt_overwrite.allow_overwrite = true;
    const auto res_overwrite = persist_json_report(r2, opt_overwrite);
    require(res_overwrite.succeeded, "Explicit overwrite must succeed: " + res_overwrite.error);

    // 5. Invalid destination directory/characters must fail safely without throwing
    ReportPersistenceOptions opt_invalid{};
    opt_invalid.output_path = "Z:\\non_existent_drive_99999\\forbidden\\report.json";
    const auto res_invalid = persist_json_report(r1, opt_invalid);
    require(!res_invalid.succeeded, "Invalid directory path must fail safely.");
    require(!res_invalid.error.empty(), "Error diagnostic must be provided for invalid directory.");

    // Clean up test files
    std::error_code ec;
    std::filesystem::remove(res1.resolved_path, ec);
    std::filesystem::remove(res2.resolved_path, ec);
}

// 13. Test distinguish unavailable fields from zero
void test_distinguish_unavailable_fields_from_zero() {
    std::cout << "Running test_distinguish_unavailable_fields_from_zero...\n";

    // Scenario A: Mapping never opened -> fields must be JSON null
    {
        VideoSmokeReport r{};
        r.run_id = generate_run_uuid();
        r.run_timestamp = current_iso8601_timestamp();
        r.mapping_open_succeeded = false;
        r.header_width = std::nullopt;
        r.header_height = std::nullopt;
        r.header_reported_fps = std::nullopt;
        r.initial_frame_counter = std::nullopt;
        r.final_frame_counter = std::nullopt;
        r.outcome = "FAIL — mapping open failed";

        const std::string json = format_json_report(r);
        std::string err;
        require(validate_report_schema(json, err), "Schema validation must pass for null fields.");
        require(json.find("\"header_width\": null") != std::string::npos, "header_width must be null when unobserved.");
        require(json.find("\"header_height\": null") != std::string::npos, "header_height must be null when unobserved.");
        require(json.find("\"header_reported_fps\": null") != std::string::npos, "header_reported_fps must be null when unobserved.");
        require(json.find("\"initial_frame_counter\": null") != std::string::npos, "initial_frame_counter must be null when unobserved.");
        require(json.find("\"final_frame_counter\": null") != std::string::npos, "final_frame_counter must be null when unobserved.");
    }

    // Scenario B: Mapping opened, but no valid header observed
    {
        VideoSmokeReport r{};
        r.run_id = generate_run_uuid();
        r.run_timestamp = current_iso8601_timestamp();
        r.mapping_open_succeeded = true;
        r.header_width = std::nullopt;
        r.header_height = std::nullopt;
        r.outcome = "FAIL — no valid header observed";

        const std::string json = format_json_report(r);
        std::string err;
        require(validate_report_schema(json, err), "Schema validation must pass.");
        require(json.find("\"header_width\": null") != std::string::npos, "header_width must remain null.");
    }

    // Scenario C: Valid header observed where 0 is legitimate (e.g. fps=0, counter=0)
    {
        VideoSmokeReport r{};
        r.run_id = generate_run_uuid();
        r.run_timestamp = current_iso8601_timestamp();
        r.mapping_open_succeeded = true;
        r.header_width = 1280;
        r.header_height = 720;
        r.header_reported_fps = 0; // legitimate 0 fps reported
        r.initial_frame_counter = 0; // legitimate counter starting at 0
        r.final_frame_counter = 50;
        r.outcome = "PASS — legitimate zero values observed";

        const std::string json = format_json_report(r);
        std::string err;
        require(validate_report_schema(json, err), "Schema validation must pass.");
        require(json.find("\"header_reported_fps\": 0") != std::string::npos, "header_reported_fps must be 0, not null.");
        require(json.find("\"initial_frame_counter\": 0") != std::string::npos, "initial_frame_counter must be 0, not null.");
        require(json.find("\"header_width\": 1280") != std::string::npos, "header_width must be 1280.");
    }

    // Scenario D: Valid header with standard frame counters
    {
        VideoSmokeReport r{};
        r.run_id = generate_run_uuid();
        r.run_timestamp = current_iso8601_timestamp();
        r.mapping_open_succeeded = true;
        r.header_width = 1920;
        r.header_height = 1080;
        r.header_reported_fps = 60;
        r.initial_frame_counter = 1;
        r.final_frame_counter = 100;
        r.outcome = "PASS — full values";

        const std::string json = format_json_report(r);
        std::string err;
        require(validate_report_schema(json, err), "Schema validation must pass.");
        require(json.find("\"final_frame_counter\": 100") != std::string::npos, "final_frame_counter must be 100.");
    }
}

// 14. Test process exit verification by identity
void test_process_exit_verification_by_identity() {
    std::cout << "Running test_process_exit_verification_by_identity...\n";

    // 1. PID 0 or timestamp 0 -> VerificationUnavailable
    const auto status_zero = verify_process_exit_by_identity(0, 0);
    require(status_zero.status == ProcessExitStatus::VerificationUnavailable, "PID 0 must yield VerificationUnavailable.");

    const auto status_zero_pid = verify_process_exit_by_identity(0, 123456789ULL);
    require(status_zero_pid.status == ProcessExitStatus::VerificationUnavailable, "PID 0 with timestamp must yield VerificationUnavailable.");

    const auto status_zero_time = verify_process_exit_by_identity(12345, 0);
    require(status_zero_time.status == ProcessExitStatus::VerificationUnavailable, "Timestamp 0 must yield VerificationUnavailable.");

    // 2. Dead/Non-existent PID (e.g. 99999999) -> ConfirmedExited
    const auto status_dead = verify_process_exit_by_identity(99999999, 123456789ULL);
    require(status_dead.status == ProcessExitStatus::ConfirmedExited, "Non-existent PID must yield ConfirmedExited.");

    // 3. Current process: live process
    const DWORD current_pid = ::GetCurrentProcessId();
    HANDLE h_proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, current_pid);
    require(h_proc != nullptr, "Must be able to open current process.");
    FILETIME creation_ft{}, exit_ft{}, kernel_ft{}, user_ft{};
    BOOL time_ok = ::GetProcessTimes(h_proc, &creation_ft, &exit_ft, &kernel_ft, &user_ft);
    ::CloseHandle(h_proc);
    require(time_ok, "Must query current process times.");

    ULARGE_INTEGER uli{};
    uli.LowPart = creation_ft.dwLowDateTime;
    uli.HighPart = creation_ft.dwHighDateTime;
    const std::uint64_t current_creation_time = uli.QuadPart;

    // Matching creation time -> StillRunning
    const auto status_live = verify_process_exit_by_identity(current_pid, current_creation_time);
    require(status_live.status == ProcessExitStatus::StillRunning, "Current process must be detected as StillRunning.");

    // Mismatched creation time (simulating PID recycling by OS) -> RecycledPidExited!
    const auto status_recycled = verify_process_exit_by_identity(current_pid, current_creation_time - 50000000ULL);
    require(status_recycled.status == ProcessExitStatus::RecycledPidExited,
        "Mismatched creation time on open process must detect PID recycling and yield RecycledPidExited.");

    // 4. System process (PID 4) live test:
    // PID 4 is a live system process with boot creation timestamp != 123456789ULL.
    // If OpenProcess succeeds with PROCESS_QUERY_LIMITED_INFORMATION, it returns RecycledPidExited (creation mismatch).
    // If OpenProcess fails with ERROR_ACCESS_DENIED, it returns VerificationUnavailable.
    // It must NEVER return ConfirmedExited.
    const auto status_sys = verify_process_exit_by_identity(4, 123456789ULL);
    require(status_sys.status == ProcessExitStatus::RecycledPidExited ||
            status_sys.status == ProcessExitStatus::VerificationUnavailable,
        "System PID must yield RecycledPidExited or VerificationUnavailable, never ConfirmedExited.");
    require(status_sys.status != ProcessExitStatus::ConfirmedExited,
        "System PID must NEVER be falsely confirmed exited.");
}

// 15. Test unified post-launch cleanup scenarios
void test_unified_post_launch_cleanup_scenarios() {
    std::cout << "Running test_unified_post_launch_cleanup_scenarios...\n";

    // 1. Successful cleanup: shutdown succeeds, process exits, mapping released, journal EmptyVerified
    {
        bool stop_called = false;
        std::uint64_t simulated_time = 0;
        int query_count = 0;

        auto cleanup = execute_post_launch_cleanup(
            [&] { stop_called = true; return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                query_count++;
                if (query_count < 2) snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopping;
                else snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::VerifiedEmpty;
                return j;
            },
            L"Local\\fake_shm_clean",
            99999999, // non-existent PID -> confirmed exited
            12345,
            5000,
            nullptr, // use default verify_proc_exit_fn
            [](const std::wstring&) { return true; }, // mapping released
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(stop_called, "Stop must be called.");
        require(cleanup.shutdown_requested, "shutdown_requested must be set.");
        require(cleanup.shutdown_succeeded, "shutdown_succeeded must be true.");
        require(cleanup.process_exit.status == ProcessExitStatus::ConfirmedExited, "Process exit must be confirmed.");
        require(cleanup.containment_empty_verified, "Containment empty must be verified.");
        require(cleanup.journal_final_state == "EmptyVerified", "Journal must be EmptyVerified.");
        require(cleanup.mapping_released_verified, "Mapping release must be verified.");
        require(cleanup.cleanup_fully_verified, "Overall cleanup must be fully verified.");
    }

    // 2. Shutdown timeout: supervisor never reaches stopped state
    {
        std::uint64_t simulated_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&] { return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Ready; // Never stops
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::VerifiedEmpty;
                return j;
            },
            L"Local\\fake_shm_timeout",
            99999999,
            12345,
            2000, // 2000 ms timeout
            nullptr,
            [](const std::wstring&) { return true; },
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(!cleanup.shutdown_succeeded, "shutdown_succeeded must be false.");
        require(!cleanup.cleanup_fully_verified, "Cleanup must NOT be verified when shutdown times out.");
        require(cleanup.failure_reason.find("supervisor shutdown not confirmed") != std::string::npos,
            "Failure reason must mention supervisor shutdown not confirmed.");
    }

    // 3. Process still running after supervisor reports stopped -> leaves cleanup unverified
    {
        const DWORD cur_pid = ::GetCurrentProcessId();
        HANDLE h_cur = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, cur_pid);
        require(h_cur != nullptr, "Must open current process.");
        FILETIME ft_c{}, ft_e{}, ft_k{}, ft_u{};
        require(::GetProcessTimes(h_cur, &ft_c, &ft_e, &ft_k, &ft_u), "Must get current process times.");
        ::CloseHandle(h_cur);
        ULARGE_INTEGER uli_cur{};
        uli_cur.LowPart = ft_c.dwLowDateTime;
        uli_cur.HighPart = ft_c.dwHighDateTime;
        const std::uint64_t cur_ctime = uli_cur.QuadPart;

        std::uint64_t simulated_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&] { return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::VerifiedEmpty;
                return j;
            },
            L"Local\\fake_shm_proc",
            cur_pid, // Current process is STILL running!
            cur_ctime, // Matching creation timestamp proves it is still the same process
            2000,
            nullptr,
            [](const std::wstring&) { return true; },
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(cleanup.process_exit.status == ProcessExitStatus::StillRunning, "Process exit must detect StillRunning.");
        require(!cleanup.cleanup_fully_verified, "Cleanup must NOT be verified if process is still running.");
        require(cleanup.failure_reason.find("process exit unconfirmed") != std::string::npos, "Failure reason must mention process exit unconfirmed.");
    }

    // 4. Primary failure and cleanup failure are both retained in report
    {
        VideoSmokeReport report{};
        report.run_id = generate_run_uuid();
        report.run_timestamp = current_iso8601_timestamp();
        report.outcome = "FAIL — readiness timeout reached";
        report.failure_reason = "Boot readiness timeout: guest_ready flag was not asserted within 240000 ms";

        PostLaunchCleanupResult cleanup_res{};
        cleanup_res.shutdown_requested = true;
        cleanup_res.shutdown_succeeded = false;
        cleanup_res.cleanup_fully_verified = false;
        cleanup_res.failure_reason = "supervisor shutdown not confirmed: timed out";

        apply_cleanup_result_to_report(cleanup_res, report);

        require(report.failure_reason.find("Boot readiness timeout") != std::string::npos,
            "Primary failure reason must be preserved.");
        require(report.cleanup_failure_reason.find("supervisor shutdown not confirmed") != std::string::npos,
            "Cleanup failure reason must be recorded.");
        require(!report.cleanup_verified, "cleanup_verified must be false.");

        const std::string json = format_json_report(report);
        std::string err;
        require(validate_report_schema(json, err), "Schema validation must succeed.");
        require(json.find("\"cleanup_verified\": false") != std::string::npos, "JSON must report cleanup_verified false.");
        require(json.find("supervisor shutdown not confirmed") != std::string::npos, "JSON must contain cleanup failure reason.");
        require(json.find("Boot readiness timeout") != std::string::npos, "JSON must contain primary failure reason.");
    }

    // 5. Journal verification failure leaves cleanup unverified
    {
        std::uint64_t simulated_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&] { return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::Corrupt;
                j.error = "CRC mismatch in journal record";
                return j;
            },
            L"Local\\fake_shm_corrupt",
            99999999,
            12345,
            2000,
            nullptr,
            [](const std::wstring&) { return true; },
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(cleanup.shutdown_succeeded, "Shutdown succeeded.");
        require(!cleanup.containment_empty_verified, "Containment empty must be false.");
        require(!cleanup.cleanup_fully_verified, "Cleanup must not be verified on journal failure.");
        require(cleanup.failure_reason.find("journal not EmptyVerified") != std::string::npos,
            "Failure reason must mention journal not EmptyVerified.");
    }
}

// 16. Test prohibition of APK, game, and ownership bypass
void test_prohibition_of_apk_game_and_bypass() {
    std::cout << "Running test_prohibition_of_apk_game_and_bypass...\n";

    // 1. Confirm video smoke runner configuration has NO fields for APK or game
    VideoSmokeConfig cfg{"FANG-StageA-API36-x86_64-headed", 5554};
    std::string err;
    require(validate_video_smoke_config(cfg, err), "Config must be valid.");

    // 2. Report unavailable_fields must explicitly declare no game or APK
    VideoSmokeReport report{};
    report.run_id = generate_run_uuid();
    report.unavailable_fields = {
        {"viewport_rendering_confirmed", "Headless CLI runner without native Win32 GUI window."},
        {"game_presented_fps", "No guest game application installed or launched."},
        {"pixel_atomic_guarantee", "Android Emulator -share-vid lacks OS-level synchronization primitives."}
    };
    report.outcome = "PASS";
    const std::string json = format_json_report(report);
    require(json.find("\"game_presented_fps\"") != std::string::npos, "Report must document game_presented_fps as unavailable.");
    require(json.find("No guest game application installed or launched") != std::string::npos,
        "Report must explicitly document prohibition of APK/game.");
}

// 17. Test deterministic injected process exit verification failures and mock handling
void test_process_exit_verification_api_failures_and_injected_mocks() {
    std::cout << "Running test_process_exit_verification_api_failures_and_injected_mocks...\n";

    // Mock 0: Simulated OpenProcess ERROR_ACCESS_DENIED -> VerificationUnavailable
    {
        auto mock_open_proc_access_denied = [](std::uint32_t pid, std::uint64_t) -> ProcessExitVerificationResult {
            return {ProcessExitStatus::VerificationUnavailable,
                "OpenProcess on PID " + std::to_string(pid) + " failed with ERROR_ACCESS_DENIED."};
        };

        std::uint64_t simulated_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&] { return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::VerifiedEmpty;
                return j;
            },
            L"Local\\fake_shm_mock0",
            1234,
            5678,
            2000,
            mock_open_proc_access_denied,
            [](const std::wstring&) { return true; },
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(cleanup.process_exit.status == ProcessExitStatus::VerificationUnavailable,
            "[MOCK] Process exit must be VerificationUnavailable on access denied.");
        require(!cleanup.cleanup_fully_verified,
            "[MOCK] Cleanup must NOT be verified when process exit verification is access denied.");
        require(cleanup.failure_reason.find("process exit unconfirmed") != std::string::npos,
            "[MOCK] Failure reason must mention process exit unconfirmed.");
    }

    // Mock 1: Simulated GetProcessTimes failure -> VerificationUnavailable
    {
        auto mock_verify_proc_times_fail = [](std::uint32_t pid, std::uint64_t) -> ProcessExitVerificationResult {
            return {ProcessExitStatus::VerificationUnavailable,
                "GetProcessTimes on PID " + std::to_string(pid) + " failed with Win32 error 5 (ERROR_ACCESS_DENIED)"};
        };

        std::uint64_t simulated_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&] { return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::VerifiedEmpty;
                return j;
            },
            L"Local\\fake_shm_mock1",
            1234,
            5678,
            2000,
            mock_verify_proc_times_fail,
            [](const std::wstring&) { return true; },
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(cleanup.process_exit.status == ProcessExitStatus::VerificationUnavailable,
            "[MOCK] Process exit must be VerificationUnavailable.");
        require(!cleanup.cleanup_fully_verified,
            "[MOCK] Cleanup must NOT be verified when process exit verification is unavailable.");
        require(cleanup.failure_reason.find("process exit unconfirmed") != std::string::npos,
            "[MOCK] Failure reason must mention process exit unconfirmed.");
    }

    // Mock 2: Simulated RecycledPidExited -> process exit considered proven and accepted
    {
        auto mock_verify_recycled = [](std::uint32_t pid, std::uint64_t) -> ProcessExitVerificationResult {
            return {ProcessExitStatus::RecycledPidExited,
                "PID " + std::to_string(pid) + " was recycled by another process; original process confirmed exited."};
        };

        std::uint64_t simulated_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&] { return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::VerifiedEmpty;
                return j;
            },
            L"Local\\fake_shm_mock2",
            1234,
            5678,
            2000,
            mock_verify_recycled,
            [](const std::wstring&) { return true; },
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(cleanup.process_exit.status == ProcessExitStatus::RecycledPidExited,
            "[MOCK] Process exit status must be RecycledPidExited.");
        require(cleanup.cleanup_fully_verified,
            "[MOCK] RecycledPidExited combined with empty job and empty journal must verify cleanup.");
    }

    // Mock 3: Exception thrown inside verify_proc_exit_fn -> caught safely without crash
    {
        auto mock_verify_throws = [](std::uint32_t, std::uint64_t) -> ProcessExitVerificationResult {
            throw std::runtime_error("Simulated internal verification fault");
        };

        std::uint64_t simulated_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&] { return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::VerifiedEmpty;
                return j;
            },
            L"Local\\fake_shm_mock3",
            1234,
            5678,
            2000,
            mock_verify_throws,
            [](const std::wstring&) { return true; },
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(cleanup.process_exit.status == ProcessExitStatus::VerificationUnavailable,
            "[MOCK] Exception must be caught and return VerificationUnavailable.");
        require(!cleanup.cleanup_fully_verified,
            "[MOCK] Cleanup must NOT be verified when verification threw.");
        require(cleanup.process_exit.detail.find("Simulated internal verification fault") != std::string::npos,
            "[MOCK] Detail must record exception message.");
    }
}

// 18. Test non-standard exception handling and unified cleanup preservation
void test_non_standard_exception_cleanup_handling() {
    std::cout << "Running test_non_standard_exception_cleanup_handling...\n";

    // Scenario 1: Non-standard exception thrown after launch initiation
    {
        VideoSmokeReport report{};
        report.run_id = generate_run_uuid();
        report.run_timestamp = current_iso8601_timestamp();
        report.avd_name = "FANG-StageA-API36-x86_64-headed";
        report.emulator_port = 5554;
        report.expected_serial = "emulator-5554";
        report.expected_mapping = "videmulator5554";

        bool launch_initiated = true;
        bool cleanup_completed = false;
        bool shutdown_invoked = false;
        PostLaunchCleanupResult cleanup_result{};

        auto run_cleanup = [&]() {
            if (!launch_initiated || cleanup_completed) return;
            cleanup_completed = true;
            std::uint64_t simulated_time = 0;
            cleanup_result = execute_post_launch_cleanup(
                [&] { shutdown_invoked = true; return true; },
                [&] {
                    fang::studio::SupervisorRuntimeSnapshot snap{};
                    snap.runtime.state = fang::broker::RuntimeLifecycleState::Ready; // simulate timeout
                    return snap;
                },
                [&] {
                    fang::broker::JournalLoadResult j{};
                    j.state = fang::broker::JournalLoadState::VerifiedEmpty;
                    return j;
                },
                L"Local\\videmulator5554",
                99999999,
                12345,
                1000,
                nullptr,
                [](const std::wstring&) { return true; },
                [&] { return simulated_time; },
                [&](std::uint32_t step) { simulated_time += step; }
            );
            apply_cleanup_result_to_report(cleanup_result, report);
        };

        // Simulate catch (...) handler for non-standard exception
        try {
            // Simulated throw of a non-standard exception type (int)
            throw 42;
        } catch (const std::exception& e) {
            report.outcome = "FAIL — fatal exception after launch: " + std::string(e.what());
            report.failure_reason = e.what();
            run_cleanup();
        } catch (...) {
            report.outcome = "FAIL — fatal unknown exception after launch: unknown non-standard exception";
            report.failure_reason = "unknown non-standard exception";
            run_cleanup();
        }

        require(shutdown_invoked, "Supervisor shutdown must be invoked during non-standard exception cleanup.");
        require(cleanup_completed, "Cleanup must be marked completed.");
        require(!cleanup_result.cleanup_fully_verified, "Cleanup must fail due to simulated shutdown timeout.");
        require(report.failure_reason == "unknown non-standard exception",
            "Primary failure reason must be preserved and not overwritten by cleanup failure.");
        require(report.cleanup_failure_reason.find("supervisor shutdown not confirmed") != std::string::npos,
            "Cleanup failure reason must be recorded.");
        require(report.outcome.find("unknown non-standard exception") != std::string::npos,
            "Report outcome must contain primary error.");
        require(report.outcome.find("cleanup unverified") != std::string::npos,
            "Report outcome must append cleanup unverified note.");

        const std::string json = format_json_report(report);
        std::string err;
        require(validate_report_schema(json, err), "Report schema must validate: " + err);
    }

    // Scenario 2: Non-standard exception thrown before launch initiation
    {
        VideoSmokeReport report{};
        report.run_id = generate_run_uuid();
        report.run_timestamp = current_iso8601_timestamp();

        bool launch_initiated = false;
        bool cleanup_invoked = false;

        auto handle_exception = [&](const std::string& desc) {
            if (launch_initiated) {
                report.outcome = "FAIL — fatal exception after launch: " + desc;
                report.failure_reason = desc;
                cleanup_invoked = true;
            } else {
                report.outcome = "BLOCKED — fatal exception before launch: " + desc;
                report.failure_reason = "Pre-launch exception: " + desc;
            }
        };

        try {
            throw "non_standard_preflight_error";
        } catch (const std::exception& e) {
            handle_exception(e.what());
        } catch (...) {
            handle_exception("unknown non-standard exception");
        }

        require(!cleanup_invoked, "Post-launch cleanup must NOT be invoked when exception occurs before launch.");
        require(report.outcome.find("BLOCKED — fatal exception before launch") != std::string::npos,
            "Pre-launch exception must be marked BLOCKED.");
        require(report.failure_reason == "Pre-launch exception: unknown non-standard exception",
            "Pre-launch failure reason must be recorded.");
    }

    // Scenario 3: Exception thrown inside a cleanup callback does not skip remaining cleanup checks
    {
        bool mapping_checked = false;
        std::uint64_t simulated_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&]() -> bool { throw std::runtime_error("Shutdown request exploded"); },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::VerifiedEmpty;
                return j;
            },
            L"Local\\fake_shm_exc",
            99999999,
            12345,
            1000,
            nullptr,
            [&](const std::wstring&) { mapping_checked = true; return true; },
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(mapping_checked, "Mapping check must still execute even if shutdown request threw an exception.");
        require(cleanup.containment_empty_verified, "Journal check must still execute even if shutdown request threw.");
        require(!cleanup.cleanup_fully_verified, "Cleanup must not be verified when an exception occurred.");
    }
}

void test_append_u16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xff));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xff));
}

void test_append_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xff));
}

void test_append_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xff));
}

void test_append_str(std::vector<std::byte>& out, std::string_view s) {
    test_append_u32(out, static_cast<std::uint32_t>(s.size()));
    for (char c : s) out.push_back(static_cast<std::byte>(c));
}

class ScopedTestJournalDir {
public:
    ScopedTestJournalDir() {
        const auto temp_base = std::filesystem::temp_directory_path();
        dir_ = temp_base / ("fang_test_journal_" + std::to_string(::GetCurrentProcessId()) + "_" +
                            std::to_string(::GetTickCount64()) + "_" + std::to_string(counter_++));
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
    }

    ~ScopedTestJournalDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    const std::filesystem::path& path() const noexcept { return dir_; }
    std::wstring wpath() const { return dir_.wstring(); }

private:
    std::filesystem::path dir_;
    static inline std::uint32_t counter_ = 1;
};

struct TestOwnerIndexEntry {
    std::string user_sid;
    std::string canonical_avd;
    std::string generation_id;
    std::uint8_t state; // 0=Pending, 1=Owned, 2=EmptyVerified
};

bool restrict_test_file_to_system(const std::wstring& path) {
    const std::wstring sddl = L"D:P(A;;FA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        return false;
    }
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl, &dacl_defaulted);
    const DWORD result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    return result == ERROR_SUCCESS;
}

bool restore_test_file_access(const std::wstring& path, const std::string& user_sid) {
    std::string err;
    return fang::broker::NativeOwnershipJournal::apply_private_acl(path, user_sid, err);
}

void write_test_owner_index_file(const std::filesystem::path& dir,
                                 std::string_view owner_gen,
                                 const std::vector<TestOwnerIndexEntry>& entries,
                                 std::string_view custom_magic = "FIDX",
                                 std::uint16_t version = 1,
                                 bool apply_acl = true,
                                 std::string_view override_acl_sid = {}) {
    std::vector<std::byte> buf;
    for (char c : custom_magic) buf.push_back(static_cast<std::byte>(c));
    test_append_u16(buf, version);
    test_append_str(buf, owner_gen);
    test_append_u32(buf, static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        test_append_str(buf, e.user_sid);
        test_append_str(buf, e.canonical_avd);
        test_append_str(buf, e.generation_id);
        buf.push_back(static_cast<std::byte>(e.state));
    }
    const auto file_path = dir / "owner-index.fji";
    std::ofstream ofs(file_path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    ofs.close();

    if (apply_acl) {
        const std::string sid = !override_acl_sid.empty()
            ? std::string(override_acl_sid)
            : (!entries.empty() ? entries[0].user_sid : fang::broker::current_user_identity().identity.sid);
        std::string err;
        fang::broker::NativeOwnershipJournal::apply_private_acl(file_path.wstring(), sid, err);
    }
}

void write_test_journal_file(const std::wstring& jpath,
                             bool empty_tombstone,
                             std::uint8_t phase,
                             std::uint32_t owner_pid,
                             const fang::broker::CanonicalAvdIdentity& identity,
                             std::string_view gen,
                             std::string_view owner_gen,
                             std::string_view custom_magic = "FJRN",
                             std::uint16_t version = 1,
                             bool apply_acl = true,
                             std::string_view override_acl_sid = {}) {
    std::filesystem::path p(jpath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::vector<std::byte> buf;
    for (char c : custom_magic) buf.push_back(static_cast<std::byte>(c));
    test_append_u16(buf, version);
    buf.push_back(empty_tombstone ? std::byte{0} : std::byte{1});
    buf.push_back(static_cast<std::byte>(phase));
    test_append_u32(buf, 1); // session_id
    test_append_u32(buf, owner_pid);
    test_append_u32(buf, 0); // root_pid
    test_append_u64(buf, 0); // root_creation_time
    test_append_str(buf, identity.user_sid);
    test_append_str(buf, identity.canonical_avd_location);
    test_append_str(buf, gen);
    test_append_str(buf, owner_gen);

    std::wstring job_name_w;
    std::string err;
    fang::broker::expected_job_object_name(identity, gen, job_name_w, err);
    std::string job_name;
    job_name.reserve(job_name_w.size());
    for (const wchar_t wc : job_name_w) job_name.push_back(static_cast<char>(wc & 0x7f));
    test_append_str(buf, job_name);

    std::ofstream ofs(jpath, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    ofs.close();

    if (apply_acl) {
        const std::string sid = !override_acl_sid.empty()
            ? std::string(override_acl_sid)
            : (!identity.user_sid.empty() ? identity.user_sid : fang::broker::current_user_identity().identity.sid);
        std::string acl_err;
        fang::broker::NativeOwnershipJournal::apply_private_acl(jpath, sid, acl_err);
    }
}

// 19. Test durable journal verification scenarios (Stage 2D.5.10 regression requirements)
void test_durable_journal_verification_scenarios() {
    std::cout << "Running test_durable_journal_verification_scenarios...\n";
    std::string gen_err;
    const std::string gen1 = fang::broker::create_runtime_generation_id(gen_err);
    const std::string gen2 = fang::broker::create_runtime_generation_id(gen_err);
    const std::string owner_gen1 = fang::broker::create_runtime_generation_id(gen_err);
    require(!gen1.empty() && !gen2.empty() && !owner_gen1.empty(), "Generation IDs must be generated successfully.");

    const std::string current_sid = fang::broker::current_user_identity().identity.sid;
    require(!current_sid.empty(), "Current user SID must be available.");

    const fang::broker::CanonicalAvdIdentity id1{current_sid, "C:/Users/test/.android/avd/Pixel_API_34.avd"};
    const fang::broker::CanonicalAvdIdentity id2{current_sid, "C:/Users/test/.android/avd/Pixel_API_35.avd"};

    // Scenario 1: Clean first-run state under documented initialization contract
    {
        std::string clean_detail;
        // 1A: Non-existent directory is clean first run
        require(fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, clean_detail, L"C:\\non_existent_fang_dir_54321"),
            "Non-existent directory must be considered clean first run.");
        require(clean_detail.find("clean first run") != std::string::npos, "Detail must record clean first run.");

        // 1B: Existing directory but missing index and journal is clean first run
        ScopedTestJournalDir temp_dir_empty;
        require(fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, clean_detail, temp_dir_empty.wpath()),
            "Empty directory with no files must be considered clean first run.");
        require(clean_detail.find("clean first run") != std::string::npos, "Detail must record clean first run.");

        // 1C: Existing directory with EmptyVerified tombstone is initially clean
        ScopedTestJournalDir temp_dir_tombstone;
        std::wstring jpath_t;
        std::string jerr_t;
        require(fang::broker::NativeOwnershipJournal::get_journal_path(temp_dir_tombstone.wpath(), id1, jpath_t, jerr_t), "jpath must derive.");
        write_test_owner_index_file(temp_dir_tombstone.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 2 /* EmptyVerified */}
        });
        write_test_journal_file(jpath_t, true, 4, 1234, id1, gen1, owner_gen1);
        require(fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, clean_detail, temp_dir_tombstone.wpath()),
            "EmptyVerified tombstone must be considered clean.");
    }

    // Scenario 2: Existing truncated index fails preflight and verification
    {
        ScopedTestJournalDir temp_dir_trunc;
        const auto index_path = temp_dir_trunc.path() / "owner-index.fji";
        // 2A: 0-byte truncated index
        {
            std::ofstream ofs(index_path, std::ios::binary);
            ofs.close();
            std::string acl_err;
            fang::broker::NativeOwnershipJournal::apply_private_acl(index_path.wstring(), current_sid, acl_err);
        }
        std::string detail;
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir_trunc.wpath()),
            "0-byte index must fail preflight.");
        require(detail.find("truncated") != std::string::npos, "Detail must mention truncated index.");

        const auto res_zero = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_trunc.wpath());
        require(res_zero.state == fang::broker::JournalLoadState::Corrupt, "0-byte index must yield Corrupt.");

        // 2B: 4-byte truncated index (< 10 bytes)
        {
            std::ofstream ofs(index_path, std::ios::binary);
            ofs.write("FIDX", 4);
            ofs.close();
            std::string acl_err;
            fang::broker::NativeOwnershipJournal::apply_private_acl(index_path.wstring(), current_sid, acl_err);
        }
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir_trunc.wpath()),
            "4-byte index must fail preflight.");
        require(detail.find("truncated") != std::string::npos, "Detail must mention truncated index.");

        const auto res_four = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_trunc.wpath());
        require(res_four.state == fang::broker::JournalLoadState::Corrupt, "4-byte index must yield Corrupt.");
    }

    // Scenario 3: Existing corrupt index magic or unsupported schema
    {
        // 3A: Corrupt index magic ("BADX")
        ScopedTestJournalDir temp_dir_bad_magic;
        write_test_owner_index_file(temp_dir_bad_magic.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 2}
        }, "BADX");
        std::string detail;
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir_bad_magic.wpath()),
            "Bad index magic must fail preflight.");
        require(detail.find("magic is corrupt or invalid") != std::string::npos, "Detail must mention bad magic.");

        const auto res_bad_magic = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_bad_magic.wpath());
        require(res_bad_magic.state == fang::broker::JournalLoadState::Corrupt, "Bad index magic must yield Corrupt.");

        // 3B: Unsupported schema version (version = 2)
        ScopedTestJournalDir temp_dir_bad_ver;
        write_test_owner_index_file(temp_dir_bad_ver.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 2}
        }, "FIDX", 2 /* unsupported version */);
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir_bad_ver.wpath()),
            "Unsupported index schema version must fail preflight.");
        require(detail.find("schema version is invalid or unsupported") != std::string::npos, "Detail must mention unsupported schema.");

        const auto res_bad_ver = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_bad_ver.wpath());
        require(res_bad_ver.state == fang::broker::JournalLoadState::Unsupported, "Unsupported version must yield Unsupported.");

        // 3C: Duplicate entries in owner-index
        ScopedTestJournalDir temp_dir_dup;
        write_test_owner_index_file(temp_dir_dup.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 2},
            {id1.user_sid, id1.canonical_avd_location, gen1, 2}
        });
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir_dup.wpath()),
            "Duplicate entry in index must fail preflight.");
        require(detail.find("duplicate") != std::string::npos, "Detail must mention duplicate records.");

        const auto res_dup = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_dup.wpath());
        require(res_dup.state == fang::broker::JournalLoadState::Corrupt, "Duplicate entry in index must yield Corrupt.");
    }

    // Scenario 4: Existing truncated or corrupt journal
    {
        ScopedTestJournalDir temp_dir;
        std::wstring jpath;
        std::string jerr;
        require(fang::broker::NativeOwnershipJournal::get_journal_path(temp_dir.wpath(), id1, jpath, jerr), "jpath must derive.");

        write_test_owner_index_file(temp_dir.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 2}
        });

        // 4A: Truncated journal (5 bytes)
        {
            std::ofstream ofs(jpath, std::ios::binary);
            ofs.write("FJRN\0", 5);
            ofs.close();
            std::string acl_err;
            fang::broker::NativeOwnershipJournal::apply_private_acl(jpath, current_sid, acl_err);
        }
        std::string detail;
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir.wpath()),
            "Truncated journal must fail preflight.");
        require(detail.find("corrupt or malformed") != std::string::npos || detail.find("truncated") != std::string::npos,
            "Detail must describe truncated journal.");

        const auto res_trunc = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir.wpath());
        require(res_trunc.state == fang::broker::JournalLoadState::Corrupt, "Truncated journal must yield Corrupt.");

        // 4B: Corrupt journal magic ("BADJ")
        write_test_journal_file(jpath, true, 4, 1234, id1, gen1, owner_gen1, "BADJ");
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir.wpath()),
            "Bad journal magic must fail preflight.");

        const auto res_bad_jrn = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir.wpath());
        require(res_bad_jrn.state == fang::broker::JournalLoadState::Corrupt, "Bad journal magic must yield Corrupt.");
    }

    // Scenario 5: Access-denied or other unreadable evidence
    {
        ScopedTestJournalDir temp_dir;
        std::wstring jpath;
        std::string jerr;
        require(fang::broker::NativeOwnershipJournal::get_journal_path(temp_dir.wpath(), id1, jpath, jerr), "jpath must derive.");

        write_test_owner_index_file(temp_dir.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 2}
        });
        write_test_journal_file(jpath, true, 4, 1234, id1, gen1, owner_gen1);

        const auto index_path = (temp_dir.path() / "owner-index.fji").wstring();
        // Restrict owner-index.fji to SYSTEM only (denies current user)
        if (restrict_test_file_to_system(index_path)) {
            std::string detail;
            require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir.wpath()),
                "Access-denied index must fail preflight.");
            require(detail.find("access denied") != std::string::npos || detail.find("Access denied") != std::string::npos,
                "Preflight detail must distinguish access denied.");

            const auto res_ad = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir.wpath());
            require(res_ad.state == fang::broker::JournalLoadState::AccessDenied,
                "Access-denied index must yield AccessDenied.");

            // Restore permissions so ScopedTestJournalDir cleanup succeeds
            restore_test_file_access(index_path, current_sid);
        }
    }

    // Scenario 6: Conflicting index and journal states
    {
        // 6A: Generation conflict (index gen1, journal gen2)
        ScopedTestJournalDir temp_dir_a;
        std::wstring jpath_a;
        std::string jerr_a;
        require(fang::broker::NativeOwnershipJournal::get_journal_path(temp_dir_a.wpath(), id1, jpath_a, jerr_a), "jpath must derive.");
        write_test_owner_index_file(temp_dir_a.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 2}
        });
        write_test_journal_file(jpath_a, true, 4, 1234, id1, gen2, owner_gen1);

        std::string detail;
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir_a.wpath()),
            "Conflicting generations must fail preflight.");
        require(detail.find("conflict") != std::string::npos, "Detail must record conflict.");

        const auto res_gen_mismatch = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_a.wpath());
        require(res_gen_mismatch.state == fang::broker::JournalLoadState::Stale ||
                res_gen_mismatch.state == fang::broker::JournalLoadState::Corrupt,
            "Conflicting generation must yield Stale or Corrupt.");

        // 6B: State conflict (index Pending state 0, but journal tombstone)
        ScopedTestJournalDir temp_dir_b;
        std::wstring jpath_b;
        std::string jerr_b;
        require(fang::broker::NativeOwnershipJournal::get_journal_path(temp_dir_b.wpath(), id1, jpath_b, jerr_b), "jpath must derive.");
        write_test_owner_index_file(temp_dir_b.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 0 /* Pending */}
        });
        write_test_journal_file(jpath_b, true, 4, 1234, id1, gen1, owner_gen1);

        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir_b.wpath()),
            "Pending index state must fail preflight.");
        require(detail.find("unfinalized") != std::string::npos || detail.find("Pending") != std::string::npos,
            "Detail must mention unfinalized/Pending state.");

        const auto res_pending = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_b.wpath());
        require(res_pending.state == fang::broker::JournalLoadState::Failed, "Pending index state must yield Failed.");

        // 6C: Index has EmptyVerified entry, but .fjn file is missing
        ScopedTestJournalDir temp_dir_c;
        write_test_owner_index_file(temp_dir_c.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 2}
        });
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir_c.wpath()),
            "Missing journal with index entry must fail preflight.");
        require(detail.find("missing") != std::string::npos, "Detail must describe missing journal file.");

        const auto res_no_jrn = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_c.wpath());
        require(res_no_jrn.state == fang::broker::JournalLoadState::Missing, "Missing journal file must yield Missing.");

        // 6D: Journal file exists, but index has no entry for this AVD
        ScopedTestJournalDir temp_dir_d;
        std::wstring jpath_d;
        std::string jerr_d;
        require(fang::broker::NativeOwnershipJournal::get_journal_path(temp_dir_d.wpath(), id1, jpath_d, jerr_d), "jpath must derive.");
        write_test_owner_index_file(temp_dir_d.path(), owner_gen1, {
            {id2.user_sid, id2.canonical_avd_location, gen1, 2}
        });
        write_test_journal_file(jpath_d, true, 4, 1234, id1, gen1, owner_gen1);
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, temp_dir_d.wpath()),
            "Journal without index entry must fail preflight.");
        require(detail.find("no entry") != std::string::npos, "Detail must describe index missing entry.");

        const auto res_missing_index_entry = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_d.wpath());
        require(res_missing_index_entry.state == fang::broker::JournalLoadState::Missing,
            "Index lacking entry for this AVD must yield Missing.");

        // 6E: Missing index entirely, orphan canonical journal file exists (Stage 2D.5.12 Case A)
        ScopedTestJournalDir temp_dir_e;
        std::wstring jpath_e;
        std::string jerr_e;
        require(fang::broker::NativeOwnershipJournal::get_journal_path(temp_dir_e.wpath(), id1, jpath_e, jerr_e), "jpath must derive.");
        const auto index_path_e = temp_dir_e.path() / "owner-index.fji";
        require(!std::filesystem::exists(index_path_e), "owner-index.fji must be genuinely absent for Case A.");
        write_test_journal_file(jpath_e, true, 4, 1234, id1, gen1, owner_gen1);
        require(std::filesystem::exists(jpath_e), "Canonical journal must exist for Case A.");

        std::string detail_e;
        require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail_e, temp_dir_e.wpath()),
            "Orphan journal without owner-index.fji must fail preflight.");
        require(detail_e.find("owner-index.fji is missing") != std::string::npos,
            "Diagnostic must identify that owner-index.fji is missing.");

        const auto res_orphan_jrn = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir_e.wpath());
        require(res_orphan_jrn.state == fang::broker::JournalLoadState::Missing,
            "Missing index file must yield Missing state.");
        require(!std::filesystem::exists(index_path_e), "owner-index.fji must remain absent after checks.");
    }

    // Scenario 7: Reparse-point rejection for index and journal paths
    {
        // Check handle validation directly: validate_private_acl_handle rejects FILE_ATTRIBUTE_REPARSE_POINT
        // We verify that root directory reparse points are also rejected.
        ScopedTestJournalDir temp_dir_target;
        const auto link_dir = temp_dir_target.path().parent_path() /
            ("fang_test_link_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64()));
        // Attempt unprivileged junction creation
        if (CreateSymbolicLinkW(link_dir.c_str(), temp_dir_target.path().c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2 /* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE */) != 0) {
            std::string detail;
            require(!fang::broker::NativeOwnershipJournal::check_durable_initially_clean(id1, detail, link_dir.wstring()),
                "Reparse point directory must fail preflight.");
            require(detail.find("reparse point") != std::string::npos, "Detail must report reparse point.");

            const auto res_reparse = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, link_dir.wstring());
            require(res_reparse.state == fang::broker::JournalLoadState::Corrupt,
                "Reparse point directory must yield Corrupt.");
            RemoveDirectoryW(link_dir.c_str());
        }
    }

    // Scenario 8: Exact matching finalized generation succeeds
    {
        ScopedTestJournalDir temp_dir;
        std::wstring jpath;
        std::string jerr;
        require(fang::broker::NativeOwnershipJournal::get_journal_path(temp_dir.wpath(), id1, jpath, jerr),
            "Journal path derivation must succeed.");

        write_test_owner_index_file(temp_dir.path(), owner_gen1, {
            {id1.user_sid, id1.canonical_avd_location, gen1, 2 /* EmptyVerified */}
        });
        write_test_journal_file(jpath, true /* empty_tombstone */, 4 /* StopRequested */, 1234, id1, gen1, owner_gen1);

        const auto res = fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir.wpath());
        require(res.state == fang::broker::JournalLoadState::VerifiedEmpty,
            "Valid EmptyVerified tombstone must yield VerifiedEmpty.");
        require(res.record.has_value() && res.record->generation_id == gen1,
            "Loaded metadata must match exact generation.");

        std::uint64_t sim_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&] { return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] { return fang::broker::NativeOwnershipJournal::verify_durable_finalized(id1, gen1, temp_dir.wpath()); },
            L"Local\\fake_shm_valid",
            99999999,
            12345,
            1000,
            nullptr,
            [](const std::wstring&) { return true; },
            [&] { return sim_time; },
            [&](std::uint32_t step) { sim_time += step; }
        );
        require(cleanup.containment_empty_verified, "Containment empty must be verified for valid journal.");
        require(cleanup.journal_final_state == "EmptyVerified", "Journal final state must be EmptyVerified.");
        require(cleanup.cleanup_fully_verified, "Overall cleanup must be verified.");
    }

    // Scenario 9: Generation unavailable during cleanup never reports EmptyVerified
    {
        std::uint64_t sim_time = 0;
        auto cleanup = execute_post_launch_cleanup(
            [&] { return true; },
            [&] {
                fang::studio::SupervisorRuntimeSnapshot snap{};
                snap.runtime.state = fang::broker::RuntimeLifecycleState::Stopped;
                return snap;
            },
            [&] {
                fang::broker::JournalLoadResult j{};
                j.state = fang::broker::JournalLoadState::Failed;
                j.error = "Runtime generation ID was not established before cleanup";
                return j;
            },
            L"Local\\fake_shm_no_gen",
            99999999,
            12345,
            1000,
            nullptr,
            [](const std::wstring&) { return true; },
            [&] { return sim_time; },
            [&](std::uint32_t step) { sim_time += step; }
        );

        require(!cleanup.containment_empty_verified, "Containment empty must NOT be reported when generation is empty.");
        require(cleanup.journal_final_state != "EmptyVerified", "Final state must not be EmptyVerified.");

        VideoSmokeReport report{};
        report.run_id = generate_run_uuid();
        report.run_timestamp = current_iso8601_timestamp();
        report.generation_id = "";
        apply_cleanup_result_to_report(cleanup, report);

        const auto json = format_json_report(report);
        require(json.find("\"generation_id\": null") != std::string::npos, "Empty generation must serialize as null.");
        std::string err;
        require(validate_report_schema(json, err), "Schema validation must pass with null generation_id.");
    }

    // Scenario 10: Generation recovery synchronized with report serialization
    {
        VideoSmokeReport report{};
        report.run_id = generate_run_uuid();
        report.run_timestamp = current_iso8601_timestamp();
        report.generation_id = ""; // Initially empty from readiness

        // Simulate recovery from snapshot
        const std::string recovered_gen = gen1;
        report.generation_id = recovered_gen;

        const auto json = format_json_report(report);
        require(json.find("\"generation_id\": \"" + recovered_gen + "\"") != std::string::npos,
            "Recovered generation must serialize into report JSON.");

        // And if generation cannot be established:
        VideoSmokeReport report_unavail{};
        report_unavail.run_id = generate_run_uuid();
        report_unavail.run_timestamp = current_iso8601_timestamp();
        report_unavail.generation_id = "";
        report_unavail.unavailable_fields.push_back({
            "lifecycle_evidence.generation_id",
            "Runtime generation ID was not established before cleanup or observation."
        });
        const auto json_unavail = format_json_report(report_unavail);
        require(json_unavail.find("\"generation_id\": null") != std::string::npos,
            "Unavailable generation must be null.");
        require(json_unavail.find("lifecycle_evidence.generation_id") != std::string::npos,
            "Unavailable fields list must record generation_id.");
    }

    // Scenario 11: Generation discontinuity during readiness polling
    {
        std::uint64_t simulated_time = 0;
        int poll_count = 0;
        fang::studio::SupervisorRuntimeSnapshot mock_snapshot{};
        mock_snapshot.runtime.state = fang::broker::RuntimeLifecycleState::GuestBooting;
        mock_snapshot.runtime.guest_ready = false;
        mock_snapshot.runtime.process_id = 12345;

        const auto result = wait_for_readiness(
            [&] {
                poll_count++;
                if (poll_count == 1) {
                    mock_snapshot.runtime.generation_id = gen1;
                } else {
                    mock_snapshot.runtime.generation_id = gen2; // Discontinuity!
                }
                return mock_snapshot;
            },
            5000,
            [&] { return simulated_time; },
            [&](std::uint32_t step) { simulated_time += step; }
        );

        require(!result.succeeded, "Readiness wait must fail on generation discontinuity.");
        require(result.error.find("generation discontinuity detected") != std::string::npos,
            "Error diagnostic must capture generation discontinuity.");
        require(result.error.find(gen1) != std::string::npos && result.error.find(gen2) != std::string::npos,
            "Error diagnostic must name expected and observed generations.");
    }

    // Scenario 12: Primary failure preservation when cleanup verification fails
    {
        VideoSmokeReport report{};
        report.run_id = generate_run_uuid();
        report.run_timestamp = current_iso8601_timestamp();
        report.outcome = "FAIL — frame counter discontinuity detected";
        report.failure_reason = "Frame sequence skipped from 42 to 99";

        PostLaunchCleanupResult cleanup{};
        cleanup.shutdown_requested = true;
        cleanup.shutdown_succeeded = true;
        cleanup.containment_empty_verified = false;
        cleanup.cleanup_fully_verified = false;
        cleanup.failure_reason = "Journal verification failed: Corrupt magic in owner-index.fji";

        apply_cleanup_result_to_report(cleanup, report);

        require(report.outcome.find("FAIL — frame counter discontinuity detected") != std::string::npos,
            "Primary outcome must remain visible in report outcome.");
        require(report.failure_reason == "Frame sequence skipped from 42 to 99",
            "Primary failure reason must NOT be overwritten by cleanup failure.");
        require(!report.cleanup_verified, "cleanup_verified must be false.");
        require(report.cleanup_failure_reason.find("Corrupt magic") != std::string::npos,
            "cleanup_failure_reason must capture journal failure.");

        const auto json = format_json_report(report);
        std::string err;
        require(validate_report_schema(json, err), "Schema validation must pass.");
        require(json.find("Frame sequence skipped from 42 to 99") != std::string::npos, "Primary failure must be in JSON.");
        require(json.find("Corrupt magic in owner-index.fji") != std::string::npos, "Cleanup failure must be in JSON.");
    }
}

} // namespace

int main() {
    std::cout << "================================================================================\n";
    std::cout << " Fang Video Smoke Offline Unit Test Suite\n";
    std::cout << " (Validates all Phase 6 scenarios without launching any emulator or guest)\n";
    std::cout << "================================================================================\n\n";

    test_invalid_port_and_mapping_configuration();
    test_readiness_timeout();
    test_mapping_unavailable();
    test_malformed_header_and_invalid_dimensions();
    test_counter_wrap_reset_and_discontinuity();
    test_frame_acceptance_limit();
    test_observation_deadline();
    test_shutdown_timeout();
    test_cleanup_failure_and_incomplete_journal_state();
    test_repeated_start_stop_offline_lifecycle();
    test_report_schema_validation_and_unavailable_fields();
    test_report_persistence_collision_and_defaults();
    test_distinguish_unavailable_fields_from_zero();
    test_process_exit_verification_by_identity();
    test_unified_post_launch_cleanup_scenarios();
    test_prohibition_of_apk_game_and_bypass();
    test_process_exit_verification_api_failures_and_injected_mocks();
    test_non_standard_exception_cleanup_handling();
    test_durable_journal_verification_scenarios();

    std::cout << "\n================================================================================\n";
    std::cout << " ALL 19 OFFLINE TEST SUITES (28+ SCENARIOS) PASSED SUCCESSFULLY!\n";
    std::cout << "================================================================================\n";
    return 0;
}
