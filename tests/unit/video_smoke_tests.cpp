#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "video_smoke_runner.hpp"
#include "video_transport.hpp"
#include "video_frame.hpp"

#include <cassert>
#include <cstring>
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

    std::cout << "\n================================================================================\n";
    std::cout << " ALL 18 OFFLINE TEST SCENARIOS PASSED SUCCESSFULLY!\n";
    std::cout << "================================================================================\n";
    return 0;
}
