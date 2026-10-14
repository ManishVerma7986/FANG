#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "capabilities.hpp"
#include "logger.hpp"
#include "runtime_backend.hpp"
#include "service_host.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr wchar_t window_class_name[] = L"FangStudioDevelopmentWindow";
constexpr int start_button_id = 1001;
constexpr int stop_button_id = 1002;
constexpr int start_player_button_id = 1003;
constexpr int stop_player_button_id = 1004;
constexpr int restart_player_button_id = 1005;
constexpr int open_player_button_id = 1006;
constexpr int avd_combo_id = 1007;
constexpr UINT runtime_timer_id = 1;

enum class ControlId : int {
    Title = 1100,
    CoreStatus,
    RuntimeStatus,
    Capabilities,
    Log,
    AvdLabel
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
    std::unique_ptr<fang::studio::RuntimeBackend> runtime;
    std::string initialization_error;
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

    StudioApp() : logger(log_buffer, fang::core::LogLevel::Trace) {
        emulator = fang::studio::discover_android_emulator();
        if (emulator.error.empty()) {
            runtime = std::make_unique<fang::studio::RuntimeBackend>(
                fang::studio::create_windows_emulator_process());
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

void update_view(StudioApp& app) {
    const auto core_text = "Core services: " + std::string(lifecycle_name(app.services.state()));
    SetWindowTextW(app.core_status, to_wide(core_text).c_str());
    std::string runtime_text;
    if (app.runtime == nullptr) {
        runtime_text = "Android Emulator unavailable: " + app.emulator.error;
    } else {
        const auto selected = SendMessageW(app.avd_combo, CB_GETCURSEL, 0, 0);
        runtime_text = "Android Emulator: " + fang::studio::runtime_state_name(app.runtime->status().state);
        if (selected == CB_ERR) {
            runtime_text += " — select an installed AVD.";
        } else if (!app.runtime->status().message.empty() &&
            app.runtime->status().message != fang::studio::runtime_state_name(app.runtime->status().state)) {
            runtime_text += " — " + app.runtime->status().message;
        }
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
    capability_text << "Player mode: external Android Emulator window; display embedding and guest boot verification are not implemented.\r\n";
    SetWindowTextW(app.capability_list, to_wide(capability_text.str()).c_str());

    SetWindowTextW(app.log_view, to_wide(app.log_buffer.str()).c_str());
    SendMessageW(app.log_view, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(app.log_view, EM_SCROLLCARET, 0, 0);

    const auto state = app.services.state();
    EnableWindow(app.start_button, state == fang::core::LifecycleState::Created && app.initialization_error.empty());
    EnableWindow(app.stop_button, state == fang::core::LifecycleState::Running || state == fang::core::LifecycleState::Failed);
    const auto selected = app.avd_combo != nullptr && SendMessageW(app.avd_combo, CB_GETCURSEL, 0, 0) != CB_ERR;
    const auto runtime_state = app.runtime == nullptr ? fang::studio::RuntimeState::Failed : app.runtime->status().state;
    const auto runtime_active = runtime_state == fang::studio::RuntimeState::Starting ||
        runtime_state == fang::studio::RuntimeState::Running || runtime_state == fang::studio::RuntimeState::Stopping;
    EnableWindow(app.avd_combo, app.runtime != nullptr && !runtime_active);
    EnableWindow(app.start_player_button, app.runtime != nullptr && selected &&
        (runtime_state == fang::studio::RuntimeState::Stopped || runtime_state == fang::studio::RuntimeState::Failed));
    EnableWindow(app.stop_player_button, app.runtime != nullptr &&
        (runtime_active || runtime_state == fang::studio::RuntimeState::Failed));
    EnableWindow(app.restart_player_button, app.runtime != nullptr && selected &&
        (runtime_state == fang::studio::RuntimeState::Stopped || runtime_state == fang::studio::RuntimeState::Starting ||
         runtime_state == fang::studio::RuntimeState::Running || runtime_state == fang::studio::RuntimeState::Failed));
    EnableWindow(app.open_player_button, app.runtime != nullptr && runtime_state == fang::studio::RuntimeState::Running);
}

void layout_controls(StudioApp& app, int width, int height) {
    constexpr int margin = 16;
    constexpr int title_height = 28;
    constexpr int row_height = 26;
    constexpr int button_width = 100;
    const int content_width = std::max(0, width - margin * 2);
    const int top = 12;

    MoveWindow(app.title, margin, top, content_width, title_height, TRUE);
    MoveWindow(app.core_status, margin, top + title_height + 4, content_width - button_width * 2 - 16, row_height, TRUE);
    MoveWindow(app.start_button, width - margin - button_width * 2 - 8, top + title_height, button_width, row_height, TRUE);
    MoveWindow(app.stop_button, width - margin - button_width, top + title_height, button_width, row_height, TRUE);
    const int runtime_top = top + title_height + row_height + 10;
    MoveWindow(app.runtime_status, margin, runtime_top, content_width, row_height, TRUE);
    MoveWindow(app.avd_label, margin, runtime_top + row_height + 6, 36, row_height, TRUE);
    MoveWindow(app.avd_combo, margin + 40, runtime_top + row_height + 4, 270, 240, TRUE);
    MoveWindow(app.start_player_button, margin + 320, runtime_top + row_height + 4, 88, row_height, TRUE);
    MoveWindow(app.stop_player_button, margin + 414, runtime_top + row_height + 4, 80, row_height, TRUE);
    MoveWindow(app.restart_player_button, margin + 500, runtime_top + row_height + 4, 88, row_height, TRUE);
    MoveWindow(app.open_player_button, margin + 594, runtime_top + row_height + 4, 96, row_height, TRUE);

    const int capability_top = runtime_top + row_height * 2 + 20;
    const int capability_height = 90;
    MoveWindow(app.capability_list, margin, capability_top, content_width, capability_height, TRUE);

    const int log_top = capability_top + capability_height + 12;
    MoveWindow(app.log_view, margin, log_top, content_width, std::max(80, height - log_top - margin), TRUE);
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
        app->open_player_button = create_control(window, L"BUTTON", L"Open Player", WS_TABSTOP | BS_PUSHBUTTON, open_player_button_id);

        const auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (const auto control : {app->title, app->core_status, app->runtime_status, app->avd_label, app->avd_combo,
                 app->capability_list, app->log_view, app->start_button, app->stop_button, app->start_player_button,
                 app->stop_player_button, app->restart_player_button, app->open_player_button}) {
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
        if (LOWORD(w_param) == start_player_button_id || LOWORD(w_param) == stop_player_button_id ||
            LOWORD(w_param) == restart_player_button_id || LOWORD(w_param) == open_player_button_id) {
            if (app->runtime == nullptr) {
                update_view(*app);
                return 0;
            }
            auto result = fang::studio::RuntimeResult::failure("No Android Emulator action was selected.");
            const auto command = LOWORD(w_param);
            if (command == start_player_button_id || command == restart_player_button_id) {
                const auto selected = SendMessageW(app->avd_combo, CB_GETCURSEL, 0, 0);
                if (selected != CB_ERR && static_cast<std::size_t>(selected) < app->emulator.avd_names.size()) {
                    const fang::studio::EmulatorConfig config{
                        app->emulator.executable,
                        app->emulator.avd_names[static_cast<std::size_t>(selected)]};
                    result = command == start_player_button_id
                        ? app->runtime->start(config)
                        : app->runtime->restart(config);
                } else {
                    result = fang::studio::RuntimeResult::failure("Select an installed AVD before starting the Android Emulator.");
                }
            } else if (command == stop_player_button_id) {
                result = app->runtime->stop();
            } else {
                result = app->runtime->open_player();
            }
            if (!result.message.empty()) {
                static_cast<void>(app->logger.write(
                    result.succeeded ? fang::core::LogLevel::Info : fang::core::LogLevel::Error,
                    result.succeeded ? "runtime_action" : "runtime_action_failed",
                    "android-emulator-reference",
                    result.message));
            }
            update_view(*app);
            return 0;
        }
        break;
    case WM_TIMER:
        if (w_param == runtime_timer_id && app->runtime != nullptr) {
            app->runtime->poll();
            update_view(*app);
            if (app->close_when_stopped && app->runtime->status().state == fang::studio::RuntimeState::Stopped) {
                DestroyWindow(window);
                return 0;
            }
            if (app->close_when_stopped && app->runtime->status().state == fang::studio::RuntimeState::Failed &&
                !app->close_failure_prompted) {
                app->close_failure_prompted = true;
                const auto answer = MessageBoxW(window,
                    L"Fang could not confirm a graceful Emulator shutdown. Exit Fang and leave the Android Emulator process running?",
                    L"Android Emulator still active", MB_YESNO | MB_ICONWARNING);
                if (answer == IDYES) {
                    DestroyWindow(window);
                    return 0;
                }
                app->close_when_stopped = false;
                app->close_failure_prompted = false;
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
        if (app->runtime != nullptr && app->runtime->status().state != fang::studio::RuntimeState::Stopped) {
            const auto result = app->runtime->stop();
            if (!result.succeeded) {
                MessageBoxW(window, to_wide(result.message).c_str(), L"Android Emulator shutdown failed", MB_OK | MB_ICONERROR);
                return 0;
            }
            if (app->runtime->status().state == fang::studio::RuntimeState::Stopped) {
                DestroyWindow(window);
                return 0;
            }
            app->close_when_stopped = true;
            app->close_failure_prompted = false;
            update_view(*app);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
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
        760,
        560,
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