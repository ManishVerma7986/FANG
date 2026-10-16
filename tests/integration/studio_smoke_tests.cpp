#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "suspended_process_launch.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "studio_window_selection.hpp"

namespace {

constexpr int start_button_id = 1001;
constexpr int stop_button_id = 1002;
constexpr int start_player_button_id = 1003;
constexpr int stop_player_button_id = 1004;
constexpr int restart_player_button_id = 1005;
constexpr int open_player_button_id = 1006;
constexpr int avd_combo_id = 1007;
constexpr int core_status_id = 1101;
constexpr int runtime_status_id = 1102;
constexpr int capabilities_id = 1103;
constexpr int log_id = 1104;

std::wstring read_text(HWND window, DWORD expected_process_id);
HWND find_child(HWND parent, int id, DWORD expected_process_id);
HWND find_studio_window(DWORD expected_process_id);

bool native_is_window(HWND window) {
    return IsWindow(window) != FALSE;
}

DWORD native_window_process_id(HWND window) {
    DWORD process_id{};
    return GetWindowThreadProcessId(window, &process_id) == 0 ? 0 : process_id;
}

bool is_owned_window(HWND window, DWORD expected_process_id) {
    return fang::studio::is_window_owned_by_process(window, expected_process_id,
        native_is_window, native_window_process_id);
}

struct ProcessGuard {
    PROCESS_INFORMATION process{};
    DWORD process_id{};
    HWND main_window{};
    HWND runtime_status{};
    bool runtime_launch_requested{};
    bool completed{};

    ~ProcessGuard() {
        if (process.hProcess != nullptr && !completed &&
            WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
            if (!is_owned_window(main_window, process_id)) {
                main_window = find_studio_window(process_id);
            }
            if (!is_owned_window(runtime_status, process_id) && main_window != nullptr) {
                runtime_status = find_child(main_window, runtime_status_id, process_id);
            }

            const auto cleanup = fang::studio::shutdown_owned_process(
                [&] {
                    return fang::studio::invoke_on_owned_window(main_window, process_id,
                        native_is_window, native_window_process_id, [&](HWND window) {
                            DWORD_PTR result{};
                            return SendMessageTimeoutW(window, WM_CLOSE, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0;
                        });
                },
                [&] { return WaitForSingleObject(process.hProcess, 45000) == WAIT_OBJECT_0; },
                [&] {
                    if (!runtime_launch_requested) {
                        return true;
                    }
                    return is_owned_window(runtime_status, process_id) &&
                        read_text(runtime_status, process_id).find(L"Android Emulator: Stopped") != std::wstring::npos;
                },
                [&] {
                    if (TerminateProcess(process.hProcess, 1) != 0) {
                        return true;
                    }
                    return WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0;
                },
                [&] { return WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0; });

            if (cleanup == fang::studio::OwnedProcessCleanupResult::Unverified) {
                std::fprintf(stderr,
                    "Smoke cleanup unverified: preserving Fang Studio PID %lu because runtime shutdown was not confirmed.\n",
                    process_id);
            } else if (cleanup == fang::studio::OwnedProcessCleanupResult::EscalatedExit) {
                std::fprintf(stderr,
                    "Smoke cleanup escalated only the owned Fang Studio PID %lu after runtime shutdown was confirmed.\n",
                    process_id);
            } else if (cleanup == fang::studio::OwnedProcessCleanupResult::EscalationFailed ||
                cleanup == fang::studio::OwnedProcessCleanupResult::EscalationUnconfirmed) {
                std::fprintf(stderr,
                    "Smoke cleanup failed for owned Fang Studio PID %lu; process exit is unconfirmed.\n",
                    process_id);
            }
        }
        if (process.hProcess != nullptr) CloseHandle(process.hProcess);
        if (process.hThread != nullptr) {
            CloseHandle(process.hThread);
        }
    }
};

struct ChildIdSearch {
    int id;
    DWORD process_id;
    HWND window{};
};

BOOL CALLBACK find_child_by_id(HWND child, LPARAM parameter) {
    auto* search = reinterpret_cast<ChildIdSearch*>(parameter);
    if (is_owned_window(child, search->process_id) && GetDlgCtrlID(child) == search->id) {
        search->window = child;
        return FALSE;
    }
    return TRUE;
}

HWND find_child(HWND parent, int id, DWORD expected_process_id) {
    if (!is_owned_window(parent, expected_process_id)) {
        return nullptr;
    }
    ChildIdSearch search{id, expected_process_id};
    EnumChildWindows(parent, find_child_by_id, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

struct TopLevelWindowSearch {
    std::vector<HWND> windows;
};

BOOL CALLBACK collect_top_level_windows(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<TopLevelWindowSearch*>(parameter);
    search->windows.push_back(window);
    return TRUE;
}

HWND find_studio_window(DWORD expected_process_id) {
    TopLevelWindowSearch search;
    EnumWindows(collect_top_level_windows, reinterpret_cast<LPARAM>(&search));
    return fang::studio::find_owned_studio_window(expected_process_id, search.windows,
        [](HWND window) {
            wchar_t class_name[128]{};
            const int length = GetClassNameW(window, class_name,
                static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
            return length > 0 &&
                std::wstring_view(class_name, static_cast<std::size_t>(length)) == L"FangStudioDevelopmentWindow";
        },
        native_is_window, native_window_process_id);
}

std::wstring read_text(HWND window, DWORD expected_process_id) {
    std::vector<wchar_t> buffer(4096);
    DWORD_PTR result{};
    const bool sent = fang::studio::invoke_on_owned_window(window, expected_process_id,
        native_is_window, native_window_process_id, [&](HWND target) {
            return SendMessageTimeoutW(target, WM_GETTEXT, static_cast<WPARAM>(buffer.size()),
                reinterpret_cast<LPARAM>(buffer.data()), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0;
        });
    if (!sent) {
        return {};
    }
    return std::wstring(buffer.data(), static_cast<std::size_t>(result));
}

std::string to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "unreadable status";
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

bool wait_for_text(HWND window, DWORD expected_process_id, std::wstring_view expected, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        if (read_text(window, expected_process_id).find(expected) != std::wstring::npos) {
            return true;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return read_text(window, expected_process_id).find(expected) != std::wstring::npos;
}

bool wait_for_runtime_running(HWND status, DWORD expected_process_id, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    std::wstring last_status;
    do {
        const auto text = read_text(status, expected_process_id);
        if (text != last_status) {
            std::cout << "RUNTIME_STATUS=" << to_utf8(text) << '\n';
            last_status = text;
        }
        if (text.find(L"Android Emulator: Running") != std::wstring::npos) {
            return true;
        }
        if (text.find(L"Android Emulator: Failed") != std::wstring::npos) {
            return false;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    if (last_status.empty()) {
        std::cout << "RUNTIME_STATUS=" << to_utf8(read_text(status, expected_process_id)) << '\n';
    }
    return false;
}

bool wait_for_runtime_text(HWND status, DWORD expected_process_id, std::wstring_view expected, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    std::wstring last_status;
    do {
        const auto text = read_text(status, expected_process_id);
        if (text != last_status) {
            std::cout << "RUNTIME_STATUS=" << to_utf8(text) << '\n';
            last_status = text;
        }
        if (text.find(expected) != std::wstring::npos) {
            return true;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    std::cout << "RUNTIME_STATUS=" << to_utf8(read_text(status, expected_process_id)) << '\n';
    return false;
}

bool invoke_button(HWND parent, HWND button, int control_id, DWORD expected_process_id) {
    if (!is_owned_window(button, expected_process_id)) {
        return false;
    }
    return fang::studio::invoke_on_owned_window(parent, expected_process_id,
        native_is_window, native_window_process_id, [&](HWND target) {
            DWORD_PTR result{};
            return SendMessageTimeoutW(target, WM_COMMAND, MAKEWPARAM(control_id, BN_CLICKED),
                reinterpret_cast<LPARAM>(button), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0;
        });
}

bool wait_for_window(ProcessGuard& child, HWND& window) {
    window = fang::studio::wait_for_owned_studio_window(child.process_id, 5000,
        [](DWORD expected_process_id) { return find_studio_window(expected_process_id); },
        [&] { return WaitForSingleObject(child.process.hProcess, 0) == WAIT_TIMEOUT; },
        native_is_window, native_window_process_id,
        [] { return GetTickCount64(); }, [] { Sleep(25); });
    return window != nullptr;
}

bool run_reference_runtime(HWND main_window, DWORD expected_process_id, HWND avd_combo,
    HWND start_button, HWND stop_button,
    HWND restart_button, HWND open_button, HWND runtime_status, int avd_argument_index,
    wchar_t* argv[], bool& runtime_launch_requested, std::string& error) {
    if (avd_argument_index >= 0) {
        DWORD_PTR match{};
        const bool found_match = fang::studio::invoke_on_owned_window(avd_combo, expected_process_id,
            native_is_window, native_window_process_id, [&](HWND target) {
                return SendMessageTimeoutW(target, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                    reinterpret_cast<LPARAM>(argv[avd_argument_index]),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &match) != 0;
            });
        if (!found_match || static_cast<LRESULT>(match) == CB_ERR) {
            error = "The requested reference AVD is not listed by Fang Studio.";
            return false;
        }
        DWORD_PTR result{};
        if (!fang::studio::invoke_on_owned_window(avd_combo, expected_process_id,
                native_is_window, native_window_process_id, [&](HWND target) {
                    return SendMessageTimeoutW(target, CB_SETCURSEL, static_cast<WPARAM>(match), 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0;
                }) ||
            !is_owned_window(main_window, expected_process_id) ||
            !fang::studio::invoke_on_owned_window(main_window, expected_process_id,
                native_is_window, native_window_process_id, [&](HWND target) {
                    return SendMessageTimeoutW(target, WM_COMMAND, MAKEWPARAM(avd_combo_id, CBN_SELCHANGE),
                        reinterpret_cast<LPARAM>(avd_combo), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0;
                })) {
            error = "Fang Studio did not accept the selected AVD.";
            return false;
        }
        if (!is_owned_window(start_button, expected_process_id) || !IsWindowEnabled(start_button)) {
            error = "Fang Studio did not enable Start for the selected AVD.";
            return false;
        }
        if (!invoke_button(main_window, start_button, start_player_button_id, expected_process_id)) {
            error = "Fang Studio did not accept the Start action.";
            return false;
        }
        runtime_launch_requested = true;
        if (!wait_for_runtime_running(runtime_status, expected_process_id, 120000)) {
            error = "Fang Studio did not detect the launched standalone Emulator window. Status: " +
                to_utf8(read_text(runtime_status, expected_process_id));
            if (is_owned_window(stop_button, expected_process_id) && IsWindowEnabled(stop_button)) {
                invoke_button(main_window, stop_button, stop_player_button_id, expected_process_id);
                wait_for_text(runtime_status, expected_process_id, L"Android Emulator: Stopped", 45000);
            }
            return false;
        }
        if (!is_owned_window(open_button, expected_process_id) || !IsWindowEnabled(open_button) ||
            !invoke_button(main_window, open_button, open_player_button_id, expected_process_id) ||
            !wait_for_text(find_child(main_window, log_id, expected_process_id), expected_process_id,
                L"Focused the standalone Android Emulator window.", 3000)) {
            error = "Fang Studio could not open/focus the standalone Emulator window by process ID.";
            if (is_owned_window(stop_button, expected_process_id) && IsWindowEnabled(stop_button)) {
                invoke_button(main_window, stop_button, stop_player_button_id, expected_process_id);
                wait_for_text(runtime_status, expected_process_id, L"Android Emulator: Stopped", 45000);
            }
            return false;
        }
        if (!is_owned_window(restart_button, expected_process_id) || !IsWindowEnabled(restart_button) ||
            !invoke_button(main_window, restart_button, restart_player_button_id, expected_process_id) ||
            !wait_for_runtime_text(runtime_status, expected_process_id, L"Android Emulator: Stopping", 3000) ||
            !wait_for_runtime_running(runtime_status, expected_process_id, 120000)) {
            error = "Fang Studio did not stop and relaunch the selected AVD through Restart. Status: " +
                to_utf8(read_text(runtime_status, expected_process_id));
            if (is_owned_window(stop_button, expected_process_id) && IsWindowEnabled(stop_button)) {
                invoke_button(main_window, stop_button, stop_player_button_id, expected_process_id);
                wait_for_text(runtime_status, expected_process_id, L"Android Emulator: Stopped", 45000);
            }
            return false;
        }
        if (!is_owned_window(stop_button, expected_process_id) || !IsWindowEnabled(stop_button) ||
            !invoke_button(main_window, stop_button, stop_player_button_id, expected_process_id) ||
            !wait_for_runtime_text(runtime_status, expected_process_id, L"Android Emulator: Stopped", 45000)) {
            error = "Fang Studio did not complete a graceful Emulator stop.";
            return false;
        }
    }
    return true;
}

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2 && argc != 4) {
        return fail("Expected the path to fang-studio.exe and optional --runtime <avd-name>.");
    }
    const bool test_runtime = argc == 4 && std::wstring_view(argv[2]) == L"--runtime";
    if (argc == 4 && !test_runtime) {
        return fail("The optional runtime argument must be --runtime <avd-name>.");
    }

    ProcessGuard child;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    std::wstring command_line = L"\"" + std::wstring(argv[1]) + L"\"";
    if (CreateProcessW(argv[1], command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
            &startup, &child.process) == 0) {
        return fail("Could not start fang-studio.exe.");
    }
    child.process_id = child.process.dwProcessId;

    WaitForInputIdle(child.process.hProcess, 5000);
    HWND main_window{};
    if (!wait_for_window(child, main_window)) {
        return fail(("The test-created Fang Studio window (PID " +
            std::to_string(child.process_id) + ") was not found within the 5-second timeout.").c_str());
    }
    child.main_window = main_window;

    const HWND start_button = find_child(main_window, start_button_id, child.process_id);
    const HWND stop_button = find_child(main_window, stop_button_id, child.process_id);
    const HWND start_player_button = find_child(main_window, start_player_button_id, child.process_id);
    const HWND stop_player_button = find_child(main_window, stop_player_button_id, child.process_id);
    const HWND restart_player_button = find_child(main_window, restart_player_button_id, child.process_id);
    const HWND open_player_button = find_child(main_window, open_player_button_id, child.process_id);
    const HWND avd_combo = find_child(main_window, avd_combo_id, child.process_id);
    const HWND core_status = find_child(main_window, core_status_id, child.process_id);
    const HWND runtime_status = find_child(main_window, runtime_status_id, child.process_id);
    child.runtime_status = runtime_status;
    const HWND capabilities = find_child(main_window, capabilities_id, child.process_id);
    const HWND log = find_child(main_window, log_id, child.process_id);
    if (start_button == nullptr || stop_button == nullptr || start_player_button == nullptr ||
        stop_player_button == nullptr || restart_player_button == nullptr || open_player_button == nullptr ||
        avd_combo == nullptr || core_status == nullptr || runtime_status == nullptr ||
        capabilities == nullptr || log == nullptr) {
        return fail("Fang Studio is missing an expected control.");
    }

    if (!wait_for_text(core_status, child.process_id, L"Core services: created", 3000) ||
        !wait_for_text(runtime_status, child.process_id, L"Android Emulator:", 3000) ||
        !wait_for_text(capabilities, child.process_id,
            L"display embedding and guest boot verification are not implemented.", 1000)) {
        return fail("Fang Studio did not report the reference-player mode and its limitations.");
    }

    std::string runtime_error;
    if (test_runtime && !run_reference_runtime(main_window, child.process_id, avd_combo, start_player_button,
            stop_player_button, restart_player_button, open_player_button, runtime_status, 3, argv,
            child.runtime_launch_requested, runtime_error)) {
        return fail(runtime_error.c_str());
    }

    if (!is_owned_window(start_button, child.process_id) || !IsWindowEnabled(start_button) ||
        !invoke_button(main_window, start_button, start_button_id, child.process_id) ||
        !wait_for_text(core_status, child.process_id, L"Core services: running", 3000)) {
        return fail("Start did not transition Fang Studio to the running state.");
    }

    if (!is_owned_window(stop_button, child.process_id) || !IsWindowEnabled(stop_button) ||
        !invoke_button(main_window, stop_button, stop_button_id, child.process_id) ||
        !wait_for_text(core_status, child.process_id, L"Core services: stopped", 3000)) {
        return fail("Stop did not transition Fang Studio to the stopped state.");
    }

    const auto log_text = read_text(log, child.process_id);
    if (log_text.find(L"core_started") == std::wstring::npos ||
        log_text.find(L"core_stopped") == std::wstring::npos) {
        return fail("Fang Studio did not display both lifecycle log entries.");
    }

    DWORD_PTR result{};
    if (WaitForSingleObject(child.process.hProcess, 0) != WAIT_TIMEOUT ||
        !fang::studio::invoke_on_owned_window(main_window, child.process_id,
            native_is_window, native_window_process_id, [&](HWND window) {
                return SendMessageTimeoutW(window, WM_CLOSE, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0;
            }) ||
        WaitForSingleObject(child.process.hProcess, 5000) != WAIT_OBJECT_0) {
        return fail("Fang Studio did not close cleanly.");
    }

    DWORD exit_code{};
    if (GetExitCodeProcess(child.process.hProcess, &exit_code) == 0 || exit_code != 0) {
        return fail("Fang Studio exited with a nonzero status.");
    }

    child.completed = true;
    std::cout << "Fang Studio GUI smoke test passed.\n";
    return 0;
}