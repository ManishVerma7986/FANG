#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fang::studio {

enum class RuntimeState {
    Stopped,
    Starting,
    Running,
    Stopping,
    Failed
};

struct RuntimeStatus {
    RuntimeState state{RuntimeState::Stopped};
    std::string message{"Stopped"};
};

struct RuntimeResult {
    bool succeeded{};
    std::string message;

    static RuntimeResult success(std::string message = {});
    static RuntimeResult failure(std::string message);
};

struct RuntimeObservation {
    bool process_running{};
    bool player_window_available{};
    std::string error;
};

struct EmulatorConfig {
    std::filesystem::path executable;
    std::wstring avd_name;
};

class IEmulatorProcess {
public:
    virtual ~IEmulatorProcess() = default;
    virtual RuntimeResult launch(const EmulatorConfig& config) = 0;
    virtual RuntimeResult request_stop() = 0;
    virtual RuntimeResult open_player() = 0;
    virtual RuntimeObservation observe() = 0;
};

class RuntimeBackend {
public:
    explicit RuntimeBackend(std::unique_ptr<IEmulatorProcess> process);

    RuntimeResult start(const EmulatorConfig& config);
    RuntimeResult stop();
    RuntimeResult restart(const EmulatorConfig& config);
    RuntimeResult open_player();
    void poll();
    const RuntimeStatus& status() const noexcept;

private:
    RuntimeResult start_after_stopped(const EmulatorConfig& config);

    std::unique_ptr<IEmulatorProcess> process_;
    RuntimeStatus status_;
    bool restart_pending_{};
    EmulatorConfig restart_config_;
};

struct EmulatorInstallation {
    std::filesystem::path executable;
    std::vector<std::wstring> avd_names;
    std::string error;
};

EmulatorInstallation discover_android_emulator();
std::vector<std::wstring> build_emulator_arguments(std::wstring_view avd_name);
std::wstring quote_windows_argument(std::wstring_view argument);
std::unique_ptr<IEmulatorProcess> create_windows_emulator_process();
std::string runtime_state_name(RuntimeState state);

}
