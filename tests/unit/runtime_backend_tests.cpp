#include "runtime_backend.hpp"
#include "suspended_process_launch.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace fang::studio;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class FakeEmulatorProcess final : public IEmulatorProcess {
public:
    RuntimeResult launch(const EmulatorConfig&) override {
        ++launch_count;
        if (!launch_succeeds) {
            return RuntimeResult::failure("fake launch failure");
        }
        observation = {true, false, {}, true, 1};
        return RuntimeResult::success("fake process started");
    }

    RuntimeResult request_stop() override {
        ++stop_count;
        return RuntimeResult::success("fake stop requested");
    }

    RuntimeResult open_player() override {
        ++open_count;
        return focus_succeeds ? RuntimeResult::success() : RuntimeResult::failure("fake focus failure");
    }

    RuntimeObservation observe() override {
        return observation;
    }

    RuntimeObservation observation;
    bool launch_succeeds{true};
    bool focus_succeeds{true};
    int launch_count{};
    int stop_count{};
    int open_count{};
};

EmulatorConfig config() {
    return {L"C:\\Android SDK\\emulator\\emulator.exe", L"FANG-StageA-API36-x86_64-headed"};
}

void test_command_construction() {
    const auto arguments = build_emulator_arguments(L"FANG reference");
    require(arguments == std::vector<std::wstring>{L"-avd", L"FANG reference", L"-gpu", L"host", L"-accel", L"on"},
        "Android Emulator arguments did not preserve the selected AVD and configured graphics mode.");
    require(quote_windows_argument(L"C:\\Android SDK\\emulator\\emulator.exe") ==
        L"\"C:\\Android SDK\\emulator\\emulator.exe\"", "Windows path quoting did not preserve spaces.");
    require(quote_windows_argument(L"ends with\\") == L"\"ends with\\\\\"", "Windows trailing backslash was not escaped.");
    bool rejected{};
    try {
        static_cast<void>(build_emulator_arguments(L""));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "An empty AVD name was accepted.");
}

void test_start_running_stop_and_duplicate_prevention() {
    auto process = std::make_unique<FakeEmulatorProcess>();
    auto* process_view = process.get();
    RuntimeBackend backend(std::move(process));
    require(backend.start(config()).succeeded, "Initial runtime start failed.");
    require(backend.status().state == RuntimeState::Starting, "Start did not enter Starting.");
    require(!backend.start(config()).succeeded && process_view->launch_count == 1,
        "Duplicate launch was not prevented.");

    process_view->observation.player_window_available = true;
    backend.poll();
    require(backend.status().state == RuntimeState::Running, "Window availability did not enter Running.");
    require(backend.open_player().succeeded && process_view->open_count == 1, "Open Player did not reach the process adapter.");
    require(backend.stop().succeeded && backend.status().state == RuntimeState::Stopping,
        "Stop did not enter Stopping.");
    process_view->observation.root_process_running = false;
    process_view->observation.active_process_count = 1;
    backend.poll();
    require(backend.status().state == RuntimeState::Stopping && process_view->launch_count == 1,
        "Root exit was mistaken for complete Job Object shutdown while a child remained.");
    process_view->observation.process_running = false;
    process_view->observation.active_process_count = 0;
    backend.poll();
    require(backend.status().state == RuntimeState::Stopped, "Process exit did not complete Stopping.");
}

void test_unexpected_exit_and_launch_failure() {
    auto process = std::make_unique<FakeEmulatorProcess>();
    auto* process_view = process.get();
    RuntimeBackend backend(std::move(process));
    process_view->launch_succeeds = false;
    require(!backend.start(config()).succeeded && backend.status().state == RuntimeState::Failed,
        "Launch failure did not surface as Failed.");
    process_view->launch_succeeds = true;
    require(backend.start(config()).succeeded, "Retry after failed launch did not start.");
    process_view->observation.player_window_available = true;
    process_view->observation.root_process_running = true;
    backend.poll();
    process_view->observation.process_running = false;
    process_view->observation.root_process_running = false;
    process_view->observation.active_process_count = 0;
    backend.poll();
    require(backend.status().state == RuntimeState::Failed &&
        backend.status().message == "Android Emulator exited unexpectedly.",
        "Unexpected process exit was not reported.");
}

void test_restart_waits_for_exit() {
    auto process = std::make_unique<FakeEmulatorProcess>();
    auto* process_view = process.get();
    RuntimeBackend backend(std::move(process));
    require(backend.start(config()).succeeded, "Initial restart-test launch failed.");
    process_view->observation.player_window_available = true;
    process_view->observation.root_process_running = true;
    backend.poll();
    require(backend.restart(config()).succeeded && backend.status().state == RuntimeState::Stopping,
        "Restart did not request a graceful stop.");
    require(process_view->launch_count == 1, "Restart relaunched before the old process exited.");
    process_view->observation.root_process_running = false;
    process_view->observation.active_process_count = 1;
    backend.poll();
    require(process_view->launch_count == 1 && backend.status().state == RuntimeState::Stopping,
        "Restart relaunched while an owned child remained in the Job Object.");
    process_view->observation.process_running = false;
    process_view->observation.active_process_count = 0;
    backend.poll();
    require(process_view->launch_count == 2 && backend.status().state == RuntimeState::Starting,
        "Restart did not launch after the old process exited.");
}

void test_root_exit_with_live_child_marks_failure_but_allows_stop() {
    auto process = std::make_unique<FakeEmulatorProcess>();
    auto* process_view = process.get();
    RuntimeBackend backend(std::move(process));
    require(backend.start(config()).succeeded, "Initial child-lifecycle launch failed.");
    process_view->observation.player_window_available = true;
    backend.poll();
    process_view->observation.root_process_running = false;
    process_view->observation.active_process_count = 1;
    backend.poll();
    require(backend.status().state == RuntimeState::Failed &&
        backend.status().message.find("root PID") != std::string::npos &&
        backend.status().message.find("exited") != std::string::npos,
        "Unexpected root exit with an owned child was not reported explicitly.");
    require(process_view->observation.process_running,
        "The owned child was not retained as an active runtime group.");
    require(backend.stop().succeeded && process_view->stop_count == 1,
        "Stop could not target the still-active owned process group after root exit.");
    process_view->observation.process_running = false;
    process_view->observation.active_process_count = 0;
    backend.poll();
    require(backend.status().state == RuntimeState::Stopped,
        "Runtime did not stop after the complete owned group exited.");
}

struct SuspendedLaunchFixture {
    bool job_creation_succeeds{true};
    bool process_creation_succeeds{true};
    bool assignment_succeeds{true};
    bool resume_succeeds{true};
    bool abort_succeeds{true};
    std::vector<std::string> calls;

    SuspendedLaunchResult run() {
        return launch_suspended_process(
            [&] { calls.push_back("create-job"); return job_creation_succeeds; },
            [&] { calls.push_back("create-suspended"); return process_creation_succeeds; },
            [&] { calls.push_back("assign"); return assignment_succeeds; },
            [&] { calls.push_back("resume"); return resume_succeeds; },
            [&] { calls.push_back("abort-owned-suspended-root"); return abort_succeeds; });
    }
};

void test_suspended_launch_sequence_and_setup_failures() {
    SuspendedLaunchFixture job_failure;
    job_failure.job_creation_succeeds = false;
    require(job_failure.run() == SuspendedLaunchResult::JobCreationFailed &&
        job_failure.calls == std::vector<std::string>{"create-job"},
        "Job creation failure allowed process creation.");

    SuspendedLaunchFixture assignment_failure;
    assignment_failure.assignment_succeeds = false;
    require(assignment_failure.run() == SuspendedLaunchResult::AssignmentFailed &&
        assignment_failure.calls == std::vector<std::string>{
            "create-job", "create-suspended", "assign", "abort-owned-suspended-root"},
        "Job assignment failure did not abort only the suspended root before resume.");

    SuspendedLaunchFixture unverified_abort;
    unverified_abort.assignment_succeeds = false;
    unverified_abort.abort_succeeds = false;
    require(unverified_abort.run() == SuspendedLaunchResult::AssignmentCleanupUnverified,
        "Failed cleanup of the suspended root was not reported as unverified.");

    SuspendedLaunchFixture resume_failure;
    resume_failure.resume_succeeds = false;
    require(resume_failure.run() == SuspendedLaunchResult::ResumeFailed &&
        resume_failure.calls == std::vector<std::string>{
            "create-job", "create-suspended", "assign", "resume", "abort-owned-suspended-root"},
        "Resume failure did not attempt bounded cleanup of only its owned root.");

    SuspendedLaunchFixture success;
    require(success.run() == SuspendedLaunchResult::Started &&
        success.calls == std::vector<std::string>{"create-job", "create-suspended", "assign", "resume"},
        "Process did not enter the Job Object before resume.");
}

void test_smoke_cleanup_is_graceful_first_and_gated() {
    std::vector<std::string> calls;
    const auto safe_cleanup = shutdown_owned_process(
        [&] { calls.push_back("graceful-close"); return true; },
        [&] { calls.push_back("wait-graceful"); return false; },
        [&] { calls.push_back("runtime-group-empty"); return true; },
        [&] { calls.push_back("escalate-owned-studio"); return true; },
        [&] { calls.push_back("wait-escalated"); return true; });
    require(safe_cleanup == OwnedProcessCleanupResult::EscalatedExit &&
        calls == std::vector<std::string>{"graceful-close", "wait-graceful", "runtime-group-empty",
            "escalate-owned-studio", "wait-escalated"},
        "Smoke cleanup did not attempt graceful close before ownership-gated escalation.");

    calls.clear();
    const auto unsafe_cleanup = shutdown_owned_process(
        [&] { calls.push_back("graceful-close"); return true; },
        [&] { calls.push_back("wait-graceful"); return false; },
        [&] { calls.push_back("runtime-group-empty"); return false; },
        [&] { calls.push_back("escalate-owned-studio"); return true; },
        [&] { calls.push_back("wait-escalated"); return true; });
    require(unsafe_cleanup == OwnedProcessCleanupResult::Unverified &&
        calls == std::vector<std::string>{"graceful-close", "wait-graceful", "runtime-group-empty"},
        "Smoke cleanup escalated while runtime ownership remained active or unverified.");
}

}

int main() {
    try {
        test_command_construction();
        test_start_running_stop_and_duplicate_prevention();
        test_unexpected_exit_and_launch_failure();
        test_restart_waits_for_exit();
        test_root_exit_with_live_child_marks_failure_but_allows_stop();
        test_suspended_launch_sequence_and_setup_failures();
        test_smoke_cleanup_is_graceful_first_and_gated();
        std::cout << "7 Fang runtime backend test groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "test failure: " << exception.what() << '\n';
        return 1;
    }
}
