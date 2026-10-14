#include "runtime_backend.hpp"

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
        observation = {true, false, {}};
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
    process_view->observation.process_running = false;
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
    backend.poll();
    process_view->observation.process_running = false;
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
    backend.poll();
    require(backend.restart(config()).succeeded && backend.status().state == RuntimeState::Stopping,
        "Restart did not request a graceful stop.");
    require(process_view->launch_count == 1, "Restart relaunched before the old process exited.");
    process_view->observation.process_running = false;
    backend.poll();
    require(process_view->launch_count == 2 && backend.status().state == RuntimeState::Starting,
        "Restart did not launch after the old process exited.");
}

}

int main() {
    try {
        test_command_construction();
        test_start_running_stop_and_duplicate_prevention();
        test_unexpected_exit_and_launch_failure();
        test_restart_waits_for_exit();
        std::cout << "4 Fang runtime backend test groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "test failure: " << exception.what() << '\n';
        return 1;
    }
}
