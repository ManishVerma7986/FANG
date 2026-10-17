#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include "video_smoke_runner.hpp"
#include "broker_native_journal.hpp"
#include "broker_pipe.hpp"
#include "broker_reference_runtime.hpp"
#include "supervisor_runtime_client.hpp"
#include "video_transport.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fang::broker;
using namespace fang::studio;
using namespace fang::video_smoke;

void print_stage_line(const SmokeStageResult& stage) {
    std::cout << "stage=" << std::left << std::setw(28) << stage.name
              << " result=" << std::setw(10) << stage_status_string(stage.status)
              << " elapsed_ms=" << std::setw(10) << stage.elapsed_ms;
    if (!stage.details.empty()) {
        std::cout << " details=\"" << stage.details << "\"";
    }
    std::cout << '\n';
}

bool check_port_free(std::uint16_t port) {
    SOCKET test_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (test_socket == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    const int bind_result = bind(test_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    closesocket(test_socket);
    return bind_result == 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    // Initialize Winsock for port checks
    WSADATA wsa_data{};
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    struct WsaGuard { ~WsaGuard() { WSACleanup(); } } wsa_guard;

    VideoSmokeConfig config;
    std::string json_output_path;
    bool allow_overwrite = false;
    std::string parse_error;

    if (!parse_video_smoke_options(argc, argv, config, json_output_path, allow_overwrite, parse_error)) {
        std::cerr << "Configuration Error: " << parse_error << "\n";
        std::cerr << "Usage: fang-video-smoke --avd <name> [--port <5554>] [--timeout-ms <240000>] "
                  << "[--observation-ms <60000>] [--max-frames <100>] [--json <output-path>] [--overwrite]\n";
        return 2;
    }

    VideoSmokeReport report;
    report.run_id = generate_run_uuid();
    report.run_timestamp = current_iso8601_timestamp();
    report.avd_name = config.avd_name;
    report.emulator_port = config.emulator_port;
    report.expected_serial = "emulator-" + std::to_string(config.emulator_port);
    report.expected_mapping = shared_memory_name_for_port(config.emulator_port);

    ReportPersistenceOptions persistence_options{json_output_path, allow_overwrite};
    auto save_report = [&](const std::string& context) {
        const auto res = persist_json_report(report, persistence_options);
        if (res.succeeded) {
            std::cout << "Report artifact saved to: " << res.resolved_path << "\n";
        } else {
            std::cerr << "ERROR: Failed to save report artifact (" << context << "): " << res.error << "\n";
        }
        return res;
    };

    // Explicit unavailable annotations as mandated by Phase 4
    report.unavailable_fields.push_back({
        "viewport_rendering_confirmed",
        "Headless CLI smoke runner; Win32 window and GDI/D3D presentation viewport are not instantiated."
    });
    report.unavailable_fields.push_back({
        "game_presented_fps",
        "Developer reference emulator smoke test; no guest gaming workload is installed or running."
    });
    report.unavailable_fields.push_back({
        "pixel_payload_atomic_guarantee",
        "Android Emulator -share-vid interface lacks host OS synchronization primitives (no Win32 Mutex/Event)."
    });

    std::cout << "================================================================================\n";
    std::cout << " Fang Supervised Video-Only Smoke Runner\n";
    std::cout << " Run ID:        " << report.run_id << "\n";
    std::cout << " Timestamp:     " << report.run_timestamp << "\n";
    std::cout << " Target AVD:    " << config.avd_name << "\n";
    std::cout << " Console Port:  " << config.emulator_port << " (" << report.expected_serial << ")\n";
    std::cout << " Shared Memory: " << report.expected_mapping << "\n";
    std::cout << " Max Frames:    " << config.max_accepted_frames << "\n";
    std::cout << " Max Duration:  " << config.max_observation_ms << " ms\n";
    std::cout << "================================================================================\n\n";

    bool launch_initiated = false;
    bool cleanup_completed = false;
    PostLaunchCleanupResult cleanup_result{};
    std::string user_sid;
    std::optional<CanonicalAvdIdentity> canonical_id;
    std::unique_ptr<SupervisorRuntimeClient> client;
    const std::wstring wide_mapping = L"Local\\" + std::wstring(report.expected_mapping.begin(), report.expected_mapping.end());

    auto run_post_launch_cleanup = [&]() {
        if (!launch_initiated || cleanup_completed) return;
        cleanup_completed = true;
        std::cout << "\n>>> Executing unified supervisor-managed cleanup...\n";
        try {
            cleanup_result = execute_post_launch_cleanup(
                [&] { return client ? client->shutdown() : false; },
                [&] { return client ? client->snapshot() : fang::studio::SupervisorRuntimeSnapshot{}; },
                [&]() -> fang::broker::JournalLoadResult {
                    if (!user_sid.empty() && canonical_id.has_value()) {
                        NativeOwnershipJournal verify_journal(user_sid, 0, 0, "");
                        return verify_journal.load(*canonical_id);
                    }
                    fang::broker::JournalLoadResult r{};
                    r.state = fang::broker::JournalLoadState::Failed;
                    r.error = "User SID or canonical identity unavailable";
                    return r;
                },
                wide_mapping,
                report.owned_process_id,
                report.process_creation_time,
                config.shutdown_timeout_ms
            );
            apply_cleanup_result_to_report(cleanup_result, report);
            print_stage_line({"graceful_shutdown_cleanup",
                cleanup_result.cleanup_fully_verified ? SmokeStageStatus::Pass : SmokeStageStatus::Fail,
                cleanup_result.shutdown_elapsed_ms,
                cleanup_result.cleanup_fully_verified
                    ? "Process exited, job empty, journal EmptyVerified"
                    : cleanup_result.failure_reason});
        } catch (const std::exception& e) {
            cleanup_result.cleanup_fully_verified = false;
            cleanup_result.failure_reason = "Exception during cleanup execution: " + std::string(e.what());
            apply_cleanup_result_to_report(cleanup_result, report);
        } catch (...) {
            cleanup_result.cleanup_fully_verified = false;
            cleanup_result.failure_reason = "Unknown non-standard exception during cleanup execution";
            apply_cleanup_result_to_report(cleanup_result, report);
        }
    };

    auto handle_fatal_exception = [&](const std::string& error_desc, bool is_standard) {
        std::cerr << "\nFATAL " << (is_standard ? "EXCEPTION: " : "UNKNOWN EXCEPTION: ") << error_desc << "\n";
        if (launch_initiated) {
            report.outcome = "FAIL — fatal exception after launch: " + error_desc;
            if (report.failure_reason.empty()) {
                report.failure_reason = error_desc;
            }
            run_post_launch_cleanup();
            save_report(is_standard ? "exception handler" : "unknown exception handler");
        } else {
            report.outcome = "BLOCKED — fatal exception before launch: " + error_desc;
            if (report.failure_reason.empty()) {
                report.failure_reason = "Pre-launch exception: " + error_desc;
            }
            save_report(is_standard ? "pre-launch exception handler" : "pre-launch unknown exception handler");
        }
    };

    try {
        const auto preflight_start = std::chrono::steady_clock::now();

        // Stage 1: Preflight Prerequisites
        std::string config_error;
        const auto ref_config = resolve_reference_runtime_config(config.avd_name, config_error);
        if (!ref_config.has_value()) {
            const auto preflight_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflight_start).count());
            print_stage_line({"preflight_prerequisites", SmokeStageStatus::Blocked, preflight_ms,
                "AVD configuration resolution failed: " + config_error});
            report.outcome = "BLOCKED";
            report.failure_reason = "AVD configuration resolution failed: " + config_error;
            save_report("preflight config");
            return 3;
        }

        report.emulator_version = ref_config->sdk_root.string(); // SDK path record
        
        // Check 1.1: Target ports free
        report.port_free = check_port_free(config.emulator_port) && check_port_free(config.emulator_port + 1);
        if (!report.port_free) {
            const auto preflight_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflight_start).count());
            print_stage_line({"preflight_prerequisites", SmokeStageStatus::Blocked, preflight_ms,
                "Target port " + std::to_string(config.emulator_port) + " or " +
                std::to_string(config.emulator_port + 1) + " is already occupied"});
            report.outcome = "BLOCKED";
            report.failure_reason = "Target console/ADB port is already occupied.";
            save_report("preflight port check");
            return 3;
        }

        // Check 1.2: Mapping initially absent
        HANDLE existing_shm = OpenFileMappingW(FILE_MAP_READ, FALSE, wide_mapping.c_str());
        if (existing_shm != nullptr) {
            CloseHandle(existing_shm);
            const auto preflight_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflight_start).count());
            print_stage_line({"preflight_prerequisites", SmokeStageStatus::Blocked, preflight_ms,
                "Shared-memory mapping " + report.expected_mapping + " already exists before launch"});
            report.outcome = "BLOCKED";
            report.failure_reason = "Shared-memory mapping already exists prior to launch.";
            save_report("preflight mapping check");
            return 3;
        }
        report.mapping_initially_absent = true;

        // Check 1.3: User identity and journal state
        const auto current_user = current_user_identity();
        if (!current_user.succeeded) {
            const auto preflight_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflight_start).count());
            print_stage_line({"preflight_prerequisites", SmokeStageStatus::Blocked, preflight_ms,
                "User identity resolution failed: " + current_user.error.message});
            report.outcome = "BLOCKED";
            report.failure_reason = "User identity resolution failed: " + current_user.error.message;
            save_report("preflight user identity");
            return 3;
        }

        user_sid = current_user.identity.sid;
        canonical_id = CanonicalAvdIdentity{user_sid, ref_config->canonical_avd_location};
        NativeOwnershipJournal initial_journal(user_sid, 0, 0, "");
        const auto initial_journal_load = initial_journal.load(*canonical_id);
        if (initial_journal_load.state == JournalLoadState::Record ||
            initial_journal_load.state == JournalLoadState::Stale) {
            const auto preflight_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflight_start).count());
            print_stage_line({"preflight_prerequisites", SmokeStageStatus::Blocked, preflight_ms,
                "Ownership journal already contains unfinalized record: " +
                (initial_journal_load.record.has_value() ? initial_journal_load.record->generation_id : initial_journal_load.error)});
            report.outcome = "BLOCKED";
            report.failure_reason = "Ownership journal contains an unfinalized record.";
            save_report("preflight journal check");
            return 3;
        }
        report.journal_initially_clean = true;

        // Check 1.4: Locate supervisor executable
        std::array<wchar_t, 32768> module_path{};
        const DWORD module_length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
        const auto supervisor_executable = std::filesystem::path(
            std::wstring(module_path.data(), module_length)).parent_path() / L"fang-runtime-supervisor.exe";

        if (!std::filesystem::is_regular_file(supervisor_executable)) {
            const auto preflight_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflight_start).count());
            print_stage_line({"preflight_prerequisites", SmokeStageStatus::Blocked, preflight_ms,
                "Supervisor executable not found at: " + supervisor_executable.string()});
            report.outcome = "BLOCKED";
            report.failure_reason = "Supervisor executable missing.";
            save_report("preflight supervisor check");
            return 3;
        }

        const auto preflight_elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflight_start).count());
        print_stage_line({"preflight_prerequisites", SmokeStageStatus::Pass, preflight_elapsed,
            "Ports free, mapping absent, journal clean, supervisor found"});

        // Stage 2: Supervisor Connect & Launch
        const auto supervisor_pipe = supervisor_pipe_name_for_user(user_sid);
        client = std::make_unique<SupervisorRuntimeClient>(supervisor_pipe, supervisor_executable);

        const auto launch_start = std::chrono::steady_clock::now();
        if (!client->start(config.avd_name)) {
            const auto launch_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - launch_start).count());
            print_stage_line({"start_and_ownership", SmokeStageStatus::Fail, launch_ms,
                "Supervisor client failed to queue start request"});
            report.outcome = "FAIL";
            report.failure_reason = "Supervisor start request rejected.";
            save_report("start request rejected");
            return 1;
        }

        launch_initiated = true;

        // Stage 3: Wait for Supervisor Ready and guest_ready
        const auto readiness = wait_for_readiness([&] { return client->snapshot(); }, config.readiness_timeout_ms);
        report.owned_process_id = readiness.process_id;
        report.process_creation_time = readiness.process_creation_time;
        report.containment_verified = readiness.containment_verified;
        report.active_processes_in_job = readiness.active_processes;
        report.readiness_elapsed_ms = readiness.elapsed_ms;
        report.state_transitions = readiness.transitions;
        report.guest_ready_observed = readiness.succeeded;

        if (!readiness.succeeded) {
            print_stage_line({"guest_readiness_wait", SmokeStageStatus::Fail, readiness.elapsed_ms,
                readiness.error});
            report.outcome = "FAIL — guest readiness failed: " + readiness.error;
            report.failure_reason = readiness.error;

            // Perform unified graceful shutdown cleanup immediately
            run_post_launch_cleanup();
            save_report("readiness failure");
            return 1;
        }

        print_stage_line({"guest_readiness_wait", SmokeStageStatus::Pass, readiness.elapsed_ms,
            "Supervisor Ready and guest_ready == true; PID " + std::to_string(readiness.process_id)});

        // Stage 4: Attach Shared-Memory Transport
        const auto transport_start = std::chrono::steady_clock::now();
        auto consumer = std::make_shared<StudioVideoConsumer>(1);
        SharedMemoryVideoTransport transport(consumer);

        if (!transport.start(report.expected_mapping)) {
            const auto attach_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - transport_start).count());
            report.mapping_open_succeeded = false;
            report.mapping_open_error = transport.last_error();
            print_stage_line({"attach_video_transport", SmokeStageStatus::Fail, attach_ms,
                "Failed to attach shared memory: " + transport.last_error()});

            report.outcome = "FAIL — failed to attach shared memory: " + transport.last_error();
            report.failure_reason = report.mapping_open_error;

            // Unified graceful cleanup
            run_post_launch_cleanup();
            save_report("transport start failure");
            return 1;
        }

        report.mapping_open_succeeded = true;
        print_stage_line({"attach_video_transport", SmokeStageStatus::Pass, 0,
            "Attached to " + report.expected_mapping});

        // Stage 5: Bounded Video Observation (<= 100 frames or <= 60 seconds)
        ObservationLimits limits{config.max_accepted_frames, config.max_observation_ms};
        const auto observation = run_video_observation(transport, *consumer, limits);
        transport.stop();

        report.observation_duration_ms = observation.duration_ms;
        report.accepted_frames = observation.accepted_frames;
        report.dropped_frames = observation.dropped_frames;
        report.torn_frames = observation.torn_frames;
        report.counter_discontinuities = observation.counter_discontinuities;
        report.observation_stopped_by_frame_limit = observation.reached_frame_limit;
        report.observation_stopped_by_deadline = observation.reached_deadline;

        const auto metrics = consumer->metrics();
        report.header_width = static_cast<std::uint32_t>(metrics.current_width);
        report.header_height = static_cast<std::uint32_t>(metrics.current_height);
        report.final_frame_counter = observation.final_sequence;

        if (observation.accepted_frames >= limits.max_frames) {
            print_stage_line({"video_frame_observation", SmokeStageStatus::Pass, observation.duration_ms,
                "Accepted " + std::to_string(observation.accepted_frames) + " frames (reached frame limit)"});
        } else if (observation.reached_deadline && observation.accepted_frames > 0) {
            print_stage_line({"video_frame_observation", SmokeStageStatus::Pass, observation.duration_ms,
                "Accepted " + std::to_string(observation.accepted_frames) + " frames (reached duration deadline)"});
        } else {
            print_stage_line({"video_frame_observation", SmokeStageStatus::Fail, observation.duration_ms,
                observation.error.empty() ? "Zero frames accepted during observation window" : observation.error});
        }

        // Stage 6: Graceful Shutdown and Verified Cleanup via unified routine
        run_post_launch_cleanup();

        // Determine final outcome
        if (observation.accepted_frames > 0 && cleanup_result.cleanup_fully_verified) {
            report.outcome = "PASS — live video frames observed and accepted";
        } else if (observation.accepted_frames == 0 && report.mapping_open_succeeded) {
            report.outcome = "PARTIAL — transport opened, but no video frames were accepted";
        } else {
            report.outcome = "FAIL — smoke test did not meet required evidence criteria";
        }

        if (!cleanup_result.cleanup_fully_verified) {
            report.outcome += " (cleanup unverified: " + cleanup_result.failure_reason + ")";
        }

        std::cout << "\n================================================================================\n";
        std::cout << " Final Outcome: " << report.outcome << "\n";
        std::cout << "================================================================================\n\n";

        const auto persist_res = save_report("final completion");
        return (report.outcome.starts_with("PASS") && persist_res.succeeded) ? 0 : 1;

    } catch (const std::exception& e) {
        handle_fatal_exception(e.what(), true);
        return 1;
    } catch (...) {
        handle_fatal_exception("unknown non-standard exception", false);
        return 1;
    }
}
