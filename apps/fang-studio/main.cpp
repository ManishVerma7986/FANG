#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "capabilities.hpp"
#include "logger.hpp"
#include "runtime_backend.hpp"
#include "supervisor_runtime_client.hpp"
#include "broker_pipe.hpp"
#include "service_host.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include "player_viewport.hpp"
#include "studio_input_router.hpp"
#include "video_transport.hpp"

namespace {

constexpr wchar_t window_class_name[] = L"FangStudioDevelopmentWindow";
constexpr int start_button_id = 1001;
constexpr int stop_button_id = 1002;
constexpr int start_player_button_id = 1003;
constexpr int stop_player_button_id = 1004;
constexpr int restart_player_button_id = 1005;
constexpr int open_player_button_id = 1006;
constexpr int avd_combo_id = 1007;
constexpr int player_viewport_id = 2000;
constexpr UINT runtime_timer_id = 1;

enum class ControlId : int {
    Title = 1100,
    CoreStatus = 1101,
    RuntimeStatus = 1102,
    Capabilities = 1103,
    Log = 1104,
    AvdLabel = 1105,
    ViewportHeader = 1106
};

class StudioCoreService final : public fang::core::IService {
public:
    explicit StudioCoreService(fang::core::Logger& logger) : logger_(logger) {}

    std::string_view name() const noexcept override {
        return "studio-core";
    }

    fang::core::Result<void> start() override {
        return logger_.write(fang::core::LogLevel::Info, "core_started", "studio-core", "Core development services started");
    }

    fang::core::Result<void> stop() override {
        return logger_.write(fang::core::LogLevel::Info, "core_stopped", "studio-core", "Core development services stopped");
    }

private:
    fang::core::Logger& logger_;
};

struct StudioApp {
    std::ostringstream log_buffer;
    fang::core::Logger logger;
    fang::core::ServiceHost services;
    fang::core::CapabilityRegistry capabilities;
    fang::studio::EmulatorInstallation emulator;
    std::unique_ptr<fang::studio::SupervisorRuntimeClient> runtime;
    std::shared_ptr<fang::studio::StudioInputRouter> input_router;
    std::shared_ptr<fang::studio::StudioVideoConsumer> video_consumer;
    std::shared_ptr<fang::studio::SharedMemoryVideoTransport> video_transport;
    std::unique_ptr<fang::studio::PlayerViewport> player_viewport;
    std::string initialization_error;
    std::string runtime_initialization_error;
    bool close_when_stopped{};
    bool close_failure_prompted{};
    HWND title{};
    HWND core_status{};
    HWND runtime_status{};
    HWND avd_label{};
    HWND avd_combo{};
    HWND capability_list{};
    HWND log_view{};
    HWND start_button{};
    HWND stop_button{};
    HWND start_player_button{};
    HWND stop_player_button{};
    HWND restart_player_button{};
    HWND open_player_button{};
    HWND viewport_header{};

    StudioApp() : logger(log_buffer, fang::core::LogLevel::Trace) {
        input_router = std::make_shared<fang::studio::StudioInputRouter>();
        video_consumer = std::make_shared<fang::studio::StudioVideoConsumer>(2);
        video_transport = std::make_shared<fang::studio::SharedMemoryVideoTransport>(video_consumer);
        emulator = fang::studio::discover_android_emulator();
        const auto identity = fang::broker::current_user_identity();
        if (!identity.succeeded) {
            runtime_initialization_error = identity.error.message;
        } else {
            std::array<wchar_t, 32768> module_path{};
            const DWORD length = GetModuleFileNameW(nullptr, module_path.data(),
                static_cast<DWORD>(module_path.size()));
            if (length == 0 || length >= module_path.size()) {
                runtime_initialization_error = "Could not resolve the Fang Studio executable path.";
            } else {
                const auto supervisor_executable = std::filesystem::path(
                    std::wstring(module_path.data(), length)).parent_path() /
                    L"fang-runtime-supervisor.exe";
                runtime = std::make_unique<fang::studio::SupervisorRuntimeClient>(
                    fang::broker::supervisor_pipe_name_for_user(identity.identity.sid),
                    supervisor_executable);
            }
        }

        for (const auto name : {"android.guest_boot", "guest.graphics_integration"}) {
            auto result = capabilities.define(name);
            if (!result) {
                initialization_error = result.error().message;
                return;
            }
            result = capabilities.report(name, fang::core::CapabilityState::Unavailable,
                "Optional external Android Emulator reference only; no Fang product guest or graphics backend is selected.");
            if (!result) {
                initialization_error = result.error().message;
                return;
            }
        }

        auto result = services.add_service(std::make_unique<StudioCoreService>(logger));
        if (!result) {
            initialization_error = result.error().message;
        }
    }
};

std::wstring to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return L"Unable to display non-UTF-8 text.";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string_view lifecycle_name(fang::core::LifecycleState state) noexcept {
    switch (state) {
    case fang::core::LifecycleState::Created: return "created";
    case fang::core::LifecycleState::Starting: return "starting";
    case fang::core::LifecycleState::Running: return "running";
    case fang::core::LifecycleState::Stopping: return "stopping";
    case fang::core::LifecycleState::Stopped: return "stopped";
    case fang::core::LifecycleState::Failed: return "failed";
    }
    return "unknown";
}

HWND create_control(HWND parent, const wchar_t* class_name, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(
        0,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0,
        0,
        0,
        0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
}

using fang::broker::runtime_lifecycle_name;

void update_view(StudioApp& app) {
    const auto core_text = "Core services: " + std::string(lifecycle_name(app.services.state()));
    SetWindowTextW(app.core_status, to_wide(core_text).c_str());
    std::string runtime_text;
    const auto snapshot = app.runtime == nullptr
        ? fang::studio::SupervisorRuntimeSnapshot{}
        : app.runtime->snapshot();
    if (app.runtime == nullptr) {
        runtime_text = "Supervisor client unavailable: " + app.runtime_initialization_error;
    } else {
        const auto selected = SendMessageW(app.avd_combo, CB_GETCURSEL, 0, 0);
        runtime_text = "Reference Android Emulator: " +
            std::string(runtime_lifecycle_name(snapshot.runtime.state));
        if (selected == CB_ERR) runtime_text += " | select an installed AVD";
        if (!snapshot.runtime.avd_name.empty()) runtime_text += " | AVD=" + snapshot.runtime.avd_name;
        if (!snapshot.runtime.adb_serial.empty()) runtime_text += " | " + snapshot.runtime.adb_serial;
        if (snapshot.runtime.process_id != 0) {
            runtime_text += " | PID=" + std::to_string(snapshot.runtime.process_id);
        }
        if (snapshot.runtime.elapsed_ms != 0) {
            runtime_text += " | " + std::to_string(snapshot.runtime.elapsed_ms) + " ms";
        }
        if (!snapshot.message.empty()) runtime_text += " | " + snapshot.message;
    }
    SetWindowTextW(app.runtime_status, to_wide(runtime_text).c_str());

    std::ostringstream capability_text;
    capability_text << "Reported capabilities (not guest execution evidence):\r\n";
    for (const auto& report : app.capabilities.snapshot()) {
        capability_text << report.name << ": " << fang::core::capability_state_name(report.state)
                        << " (" << report.evidence << ")\r\n";
    }
    if (!app.initialization_error.empty()) {
        capability_text << "Core initialization error: " << app.initialization_error << "\r\n";
    }
    capability_text << "Reference mode: external Android Emulator window; display embedding and guest boot verification are not implemented.\r\n";
    capability_text << "Stage 2C: Embedded player presentation viewport and coordinate-mapped input forwarding are active.\r\n";
    SetWindowTextW(app.capability_list, to_wide(capability_text.str()).c_str());

    SetWindowTextW(app.log_view, to_wide(app.log_buffer.str()).c_str());
    SendMessageW(app.log_view, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(app.log_view, EM_SCROLLCARET, 0, 0);

    const auto state = app.services.state();
    EnableWindow(app.start_button, state == fang::core::LifecycleState::Created && app.initialization_error.empty());
    EnableWindow(app.stop_button, state == fang::core::LifecycleState::Running || state == fang::core::LifecycleState::Failed);
    const auto selected = app.avd_combo != nullptr && SendMessageW(app.avd_combo, CB_GETCURSEL, 0, 0) != CB_ERR;
    const auto runtime_state = snapshot.runtime.state;
    const auto runtime_active = runtime_state != fang::broker::RuntimeLifecycleState::Stopped &&
        runtime_state != fang::broker::RuntimeLifecycleState::Failed;
    EnableWindow(app.avd_combo, app.runtime != nullptr && !runtime_active);
    EnableWindow(app.start_player_button, app.runtime != nullptr && selected && !snapshot.request_pending &&
        (runtime_state == fang::broker::RuntimeLifecycleState::Stopped || runtime_state == fang::broker::RuntimeLifecycleState::Failed));
    EnableWindow(app.stop_player_button, app.runtime != nullptr && !snapshot.request_pending &&
        runtime_state != fang::broker::RuntimeLifecycleState::Stopped);
    EnableWindow(app.restart_player_button, app.runtime != nullptr && selected && !snapshot.request_pending &&
        runtime_state != fang::broker::RuntimeLifecycleState::RecoveryRequired &&
        runtime_state != fang::broker::RuntimeLifecycleState::Stopping);
    EnableWindow(app.open_player_button, app.runtime != nullptr &&
        (runtime_state == fang::broker::RuntimeLifecycleState::Ready ||
         runtime_state == fang::broker::RuntimeLifecycleState::OwnershipVerified));

    if (app.player_viewport != nullptr) {
        app.player_viewport->update_runtime_state(snapshot.runtime, snapshot.request_pending);
    }

    if (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Ready && snapshot.runtime.guest_ready) {
        if (app.video_transport &&
            (app.video_transport->state() == fang::studio::VideoTransportState::Disconnected ||
             app.video_transport->state() == fang::studio::VideoTransportState::Closed ||
             app.video_transport->state() == fang::studio::VideoTransportState::Error)) {
            const auto endpoint = fang::studio::shared_memory_name_for_serial(snapshot.runtime.adb_serial);
            if (endpoint.has_value()) {
                app.video_transport->start(*endpoint);
            }
        }
    } else if (app.video_transport &&
               app.video_transport->state() != fang::studio::VideoTransportState::Disconnected &&
               app.video_transport->state() != fang::studio::VideoTransportState::Closed) {
        app.video_transport->stop();
    }
}

void layout_controls(StudioApp& app, int width, int height) {
    constexpr int margin = 14;
    constexpr int title_height = 26;
    constexpr int row_height = 26;
    constexpr int button_width = 90;
    const int top = 10;

    // Allocate two columns if window is wide enough (>= 800px)
    const bool split = width >= 800;
    const int left_width = split ? std::max(480, std::min(540, width - 360)) : std::max(0, width - margin * 2);

    MoveWindow(app.title, margin, top, left_width, title_height, TRUE);
    MoveWindow(app.core_status, margin, top + title_height + 4, left_width - button_width * 2 - 16, row_height, TRUE);
    MoveWindow(app.start_button, margin + left_width - button_width * 2 - 8, top + title_height, button_width, row_height, TRUE);
    MoveWindow(app.stop_button, margin + left_width - button_width, top + title_height, button_width, row_height, TRUE);

    const int runtime_top = top + title_height + row_height + 8;
    MoveWindow(app.runtime_status, margin, runtime_top, left_width, row_height, TRUE);
    MoveWindow(app.avd_label, margin, runtime_top + row_height + 6, 32, row_height, TRUE);
    MoveWindow(app.avd_combo, margin + 36, runtime_top + row_height + 4, 180, 240, TRUE);
    MoveWindow(app.start_player_button, margin + 222, runtime_top + row_height + 4, 66, row_height, TRUE);
    MoveWindow(app.stop_player_button, margin + 292, runtime_top + row_height + 4, 62, row_height, TRUE);
    MoveWindow(app.restart_player_button, margin + 358, runtime_top + row_height + 4, 68, row_height, TRUE);
    MoveWindow(app.open_player_button, margin + 430, runtime_top + row_height + 4, 62, row_height, TRUE);

    const int capability_top = runtime_top + row_height * 2 + 16;
    const int capability_height = 80;
    MoveWindow(app.capability_list, margin, capability_top, left_width, capability_height, TRUE);

    const int log_top = capability_top + capability_height + 10;
    MoveWindow(app.log_view, margin, log_top, left_width, std::max(80, height - log_top - margin), TRUE);

    if (split) {
        const int right_left = margin + left_width + margin;
        const int right_width = std::max(120, width - right_left - margin);
        MoveWindow(app.viewport_header, right_left, top, right_width, title_height, TRUE);
        if (app.player_viewport != nullptr) {
            app.player_viewport->show(true);
            app.player_viewport->move(right_left, top + title_height + 2, right_width,
                std::max(100, height - top - title_height - margin - 2), TRUE);
        }
    } else {
        MoveWindow(app.viewport_header, 0, 0, 0, 0, FALSE);
        if (app.player_viewport != nullptr) {
            app.player_viewport->show(false);
        }
    }
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* app = reinterpret_cast<StudioApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        app = static_cast<StudioApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (app == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
    case WM_CREATE: {
        app->title = create_control(window, L"STATIC", L"Fang Studio  |  Android Player (local reference runtime)", SS_LEFT, static_cast<int>(ControlId::Title));
        app->core_status = create_control(window, L"STATIC", L"Core services: created", SS_LEFT, static_cast<int>(ControlId::CoreStatus));
        app->runtime_status = create_control(window, L"STATIC", L"Android Emulator: Stopped", SS_LEFT, static_cast<int>(ControlId::RuntimeStatus));
        app->avd_label = create_control(window, L"STATIC", L"AVD:", SS_LEFT, static_cast<int>(ControlId::AvdLabel));
        app->avd_combo = create_control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, avd_combo_id);
        app->capability_list = create_control(window, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, static_cast<int>(ControlId::Capabilities));
        app->log_view = create_control(window, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, static_cast<int>(ControlId::Log));
        app->start_button = create_control(window, L"BUTTON", L"Start core", WS_TABSTOP | BS_PUSHBUTTON, start_button_id);
        app->stop_button = create_control(window, L"BUTTON", L"Stop core", WS_TABSTOP | BS_PUSHBUTTON, stop_button_id);
        app->start_player_button = create_control(window, L"BUTTON", L"Start", WS_TABSTOP | BS_PUSHBUTTON, start_player_button_id);
        app->stop_player_button = create_control(window, L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON, stop_player_button_id);
        app->restart_player_button = create_control(window, L"BUTTON", L"Restart", WS_TABSTOP | BS_PUSHBUTTON, restart_player_button_id);
        app->open_player_button = create_control(window, L"BUTTON", L"Open", WS_TABSTOP | BS_PUSHBUTTON, open_player_button_id);
        app->viewport_header = create_control(window, L"STATIC", L"Player Viewport (Stage 2D)", SS_LEFT, static_cast<int>(ControlId::ViewportHeader));

        app->player_viewport = std::make_unique<fang::studio::PlayerViewport>(
            window, GetModuleHandleW(nullptr), player_viewport_id, app->input_router, app->video_consumer);

        const auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (const auto control : {app->title, app->core_status, app->runtime_status, app->avd_label, app->avd_combo,
                 app->capability_list, app->log_view, app->start_button, app->stop_button, app->start_player_button,
                 app->stop_player_button, app->restart_player_button, app->open_player_button, app->viewport_header}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        for (const auto& avd_name : app->emulator.avd_names) {
            SendMessageW(app->avd_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(avd_name.c_str()));
        }
        if (!app->emulator.avd_names.empty()) {
            const auto configured_avd_length = GetEnvironmentVariableW(L"FANG_ANDROID_AVD", nullptr, 0);
            if (configured_avd_length > 0) {
                std::wstring configured_avd(static_cast<std::size_t>(configured_avd_length), L'\0');
                const auto copied = GetEnvironmentVariableW(L"FANG_ANDROID_AVD", configured_avd.data(), configured_avd_length);
                if (copied > 0 && copied < configured_avd_length) {
                    configured_avd.resize(copied);
                    const auto match = std::find(app->emulator.avd_names.begin(), app->emulator.avd_names.end(), configured_avd);
                    if (match != app->emulator.avd_names.end()) {
                        SendMessageW(app->avd_combo, CB_SETCURSEL,
                            static_cast<WPARAM>(std::distance(app->emulator.avd_names.begin(), match)), 0);
                    }
                }
            } else if (app->emulator.avd_names.size() == 1) {
                SendMessageW(app->avd_combo, CB_SETCURSEL, 0, 0);
            }
        }
        SetTimer(window, runtime_timer_id, 250, nullptr);
        update_view(*app);
        return 0;
    }
    case WM_SIZE:
        layout_controls(*app, LOWORD(l_param), HIWORD(l_param));
        return 0;
    case WM_COMMAND:
        if (LOWORD(w_param) == start_button_id) {
            auto result = app->services.start();
            if (!result) {
                static_cast<void>(app->logger.write_error(result.error()));
            }
            update_view(*app);
            return 0;
        }
        if (LOWORD(w_param) == stop_button_id) {
            auto result = app->services.stop();
            if (!result) {
                static_cast<void>(app->logger.write_error(result.error()));
            }
            update_view(*app);
            return 0;
        }
        if (LOWORD(w_param) == avd_combo_id && HIWORD(w_param) == CBN_SELCHANGE) {
            update_view(*app);
            return 0;
        }
        if (LOWORD(w_param) == open_player_button_id) {
            if (app->runtime != nullptr) {
                const auto snapshot = app->runtime->snapshot();
                if (snapshot.runtime.process_id != 0) {
                    struct SearchState {
                        DWORD pid{};
                        HWND hwnd{};
                    } search{snapshot.runtime.process_id, nullptr};

                    EnumWindows([](HWND candidate, LPARAM param) -> BOOL {
                        auto* state = reinterpret_cast<SearchState*>(param);
                        DWORD pid{};
                        GetWindowThreadProcessId(candidate, &pid);
                        if (pid == state->pid && IsWindowVisible(candidate)) {
                            state->hwnd = candidate;
                            return FALSE;
                        }
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&search));

                    if (search.hwnd != nullptr) {
                        ShowWindow(search.hwnd, SW_RESTORE);
                        SetForegroundWindow(search.hwnd);
                    }
                }
            }
            static_cast<void>(app->logger.write(fang::core::LogLevel::Info, "player_window",
                "android-emulator-reference", "Focused the standalone Android Emulator window."));
            update_view(*app);
            return 0;
        }
        if (LOWORD(w_param) == start_player_button_id || LOWORD(w_param) == stop_player_button_id ||
            LOWORD(w_param) == restart_player_button_id) {
            if (app->runtime == nullptr) {
                update_view(*app);
                return 0;
            }
            const auto command = LOWORD(w_param);
            bool accepted{};
            std::string action_message;
            if (command == start_player_button_id || command == restart_player_button_id) {
                const auto selected = SendMessageW(app->avd_combo, CB_GETCURSEL, 0, 0);
                if (selected != CB_ERR && static_cast<std::size_t>(selected) < app->emulator.avd_names.size()) {
                    const auto& wide_name = app->emulator.avd_names[static_cast<std::size_t>(selected)];
                    if (wide_name.empty() || std::any_of(wide_name.begin(), wide_name.end(),
                            [](wchar_t value) { return value <= 0 || value > 0x7f; })) {
                        action_message = "The selected AVD name is not supported by the reference-runtime protocol.";
                    } else {
                        std::string avd_name;
                        avd_name.reserve(wide_name.size());
                        for (const wchar_t character : wide_name) {
                            avd_name.push_back(static_cast<char>(character));
                        }
                        accepted = command == start_player_button_id
                            ? app->runtime->start(std::move(avd_name))
                            : app->runtime->restart(std::move(avd_name));
                        if (!accepted) action_message = "A runtime request is already pending or the current state is not safe for this action.";
                    }
                } else {
                    action_message = "Select an installed AVD before starting the reference runtime.";
                }
            } else {
                accepted = app->runtime->shutdown();
                if (!accepted) action_message = "A shutdown request is already pending or the runtime is already stopped.";
            }
            if (!action_message.empty()) {
                static_cast<void>(app->logger.write(
                    accepted ? fang::core::LogLevel::Info : fang::core::LogLevel::Error,
                    accepted ? "runtime_action" : "runtime_action_failed",
                    "android-emulator-reference", action_message));
            }
            update_view(*app);
            return 0;
        }
        break;
    case WM_TIMER:
        if (w_param == runtime_timer_id && app->runtime != nullptr) {
            const auto snapshot = app->runtime->snapshot();
            update_view(*app);
            if (app->close_when_stopped &&
                (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Stopped ||
                 (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Failed &&
                  snapshot.runtime.generation_id.empty() && snapshot.runtime.process_id == 0))) {
                DestroyWindow(window);
                return 0;
            }
            if (app->close_when_stopped &&
                (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Failed ||
                 snapshot.runtime.state == fang::broker::RuntimeLifecycleState::RecoveryRequired) &&
                !app->close_failure_prompted) {
                app->close_failure_prompted = true;
                MessageBoxW(window,
                    L"Fang could not confirm safe runtime shutdown. The supervisor retains ownership where available. Review the runtime status and retry Stop; Studio will remain open.",
                    L"Reference runtime shutdown unverified", MB_OK | MB_ICONWARNING);
                app->close_when_stopped = false;
                update_view(*app);
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        if (app->services.state() == fang::core::LifecycleState::Running || app->services.state() == fang::core::LifecycleState::Failed) {
            auto result = app->services.stop();
            if (!result) {
                static_cast<void>(app->logger.write_error(result.error()));
            }
        }
        if (app->runtime != nullptr) {
            const auto snapshot = app->runtime->snapshot();
            const bool no_runtime = snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Stopped ||
                (snapshot.runtime.state == fang::broker::RuntimeLifecycleState::Failed &&
                 snapshot.runtime.generation_id.empty() && snapshot.runtime.process_id == 0);
            if (no_runtime) {
                if (app->services.state() == fang::core::LifecycleState::Running ||
                    app->services.state() == fang::core::LifecycleState::Failed) {
                    auto result = app->services.stop();
                    if (!result) static_cast<void>(app->logger.write_error(result.error()));
                }
                DestroyWindow(window);
                return 0;
            }
            if (!app->runtime->shutdown()) {
                MessageBoxW(window,
                    L"A shutdown request could not be queued. Review the runtime state before closing Studio.",
                    L"Reference runtime is still owned", MB_OK | MB_ICONWARNING);
                return 0;
            }
            app->close_when_stopped = true;
            app->close_failure_prompted = false;
            update_view(*app);
            return 0;
        }
        if (app->services.state() == fang::core::LifecycleState::Running ||
            app->services.state() == fang::core::LifecycleState::Failed) {
            auto result = app->services.stop();
            if (!result) static_cast<void>(app->logger.write_error(result.error()));
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (app != nullptr) {
            if (app->video_transport != nullptr) {
                app->video_transport->stop();
            }
            app->player_viewport.reset();
        }
        KillTimer(window, runtime_timer_id);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    StudioApp app;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = window_class_name;

    if (RegisterClassExW(&window_class) == 0) {
        MessageBoxW(nullptr, L"Fang Studio could not register its window class.", L"Fang Studio", MB_OK | MB_ICONERROR);
        return 1;
    }

    const auto window = CreateWindowExW(
        0,
        window_class_name,
        L"Fang Studio",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        960,
        600,
        nullptr,
        nullptr,
        instance,
        &app);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"Fang Studio could not create its main window.", L"Fang Studio", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
