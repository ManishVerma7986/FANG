#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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

struct ProcessGuard {
    PROCESS_INFORMATION process{};
    bool completed{};

    ~ProcessGuard() {
        if (process.hProcess != nullptr) {
            if (!completed) {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 5000);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread != nullptr) {
            CloseHandle(process.hThread);
        }
    }
};

struct ChildIdSearch {
    int id;
    HWND window{};
};

BOOL CALLBACK find_child_by_id(HWND child, LPARAM parameter) {
    auto* search = reinterpret_cast<ChildIdSearch*>(parameter);
    if (GetDlgCtrlID(child) == search->id) {
        search->window = child;
        return FALSE;
    }
    return TRUE;
}

HWND find_child(HWND parent, int id) {
    ChildIdSearch search{id};
    EnumChildWindows(parent, find_child_by_id, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

std::wstring read_text(HWND window) {
    std::vector<wchar_t> buffer(4096);
    DWORD_PTR result{};
    if (SendMessageTimeoutW(window, WM_GETTEXT, static_cast<WPARAM>(buffer.size()),
            reinterpret_cast<LPARAM>(buffer.data()), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) == 0) {
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

bool wait_for_text(HWND window, std::wstring_view expected, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        if (read_text(window).find(expected) != std::wstring::npos) {
            return true;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return read_text(window).find(expected) != std::wstring::npos;
}

bool wait_for_runtime_running(HWND status, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        const auto text = read_text(status);
        if (text.find(L"Android Emulator: Running") != std::wstring::npos) {
            return true;
        }
        if (text.find(L"Android Emulator: Failed") != std::wstring::npos) {
            return false;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return false;
}

bool invoke_button(HWND parent, HWND button, int control_id) {
    DWORD_PTR result{};
    return SendMessageTimeoutW(parent, WM_COMMAND, MAKEWPARAM(control_id, BN_CLICKED),
        reinterpret_cast<LPARAM>(button), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0;
}

bool wait_for_window(ProcessGuard& child, HWND& window) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    do {
        if (WaitForSingleObject(child.process.hProcess, 0) == WAIT_OBJECT_0) {
            return false;
        }
        window = FindWindowW(L"FangStudioDevelopmentWindow", nullptr);
        if (window != nullptr) {
            return true;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return false;
}

bool run_reference_runtime(HWND main_window, HWND avd_combo, HWND start_button, HWND stop_button,
    HWND restart_button, HWND open_button, HWND runtime_status, int avd_argument_index,
    wchar_t* argv[], std::string& error) {
    if (avd_argument_index >= 0) {
        const auto match = SendMessageW(avd_combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(argv[avd_argument_index]));
        if (match == CB_ERR) {
            error = "The requested reference AVD is not listed by Fang Studio.";
            return false;
        }
        SendMessageW(avd_combo, CB_SETCURSEL, static_cast<WPARAM>(match), 0);
        DWORD_PTR result{};
        SendMessageTimeoutW(main_window, WM_COMMAND, MAKEWPARAM(avd_combo_id, CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(avd_combo), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result);
        if (!IsWindowEnabled(start_button)) {
            error = "Fang Studio did not enable Start for the selected AVD.";
            return false;
        }
        if (!invoke_button(main_window, start_button, start_player_button_id)) {
            error = "Fang Studio did not accept the Start action.";
            return false;
        }
        if (!wait_for_runtime_running(runtime_status, 120000)) {
            error = "Fang Studio did not detect the launched standalone Emulator window. Status: " +
                to_utf8(read_text(runtime_status));
            if (IsWindowEnabled(stop_button)) {
                invoke_button(main_window, stop_button, stop_player_button_id);
                wait_for_text(runtime_status, L"Android Emulator: Stopped", 45000);
            }
            return false;
        }
        if (!IsWindowEnabled(open_button) || !invoke_button(main_window, open_button, open_player_button_id) ||
            !wait_for_text(find_child(main_window, log_id), L"Focused the standalone Android Emulator window.", 3000)) {
            error = "Fang Studio could not open/focus the standalone Emulator window by process ID.";
            if (IsWindowEnabled(stop_button)) {
                invoke_button(main_window, stop_button, stop_player_button_id);
                wait_for_text(runtime_status, L"Android Emulator: Stopped", 45000);
            }
            return false;
        }
        if (!IsWindowEnabled(restart_button) || !invoke_button(main_window, restart_button, restart_player_button_id) ||
            !wait_for_text(runtime_status, L"Android Emulator: Stopping", 3000) ||
            !wait_for_runtime_running(runtime_status, 120000)) {
            error = "Fang Studio did not stop and relaunch the selected AVD through Restart. Status: " +
                to_utf8(read_text(runtime_status));
            if (IsWindowEnabled(stop_button)) {
                invoke_button(main_window, stop_button, stop_player_button_id);
                wait_for_text(runtime_status, L"Android Emulator: Stopped", 45000);
            }
            return false;
        }
        if (!IsWindowEnabled(stop_button) || !invoke_button(main_window, stop_button, stop_player_button_id) ||
            !wait_for_text(runtime_status, L"Android Emulator: Stopped", 45000)) {
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

    WaitForInputIdle(child.process.hProcess, 5000);
    HWND main_window{};
    if (!wait_for_window(child, main_window)) {
        return fail("Fang Studio did not create its main window.");
    }

    const HWND start_button = find_child(main_window, start_button_id);
    const HWND stop_button = find_child(main_window, stop_button_id);
    const HWND start_player_button = find_child(main_window, start_player_button_id);
    const HWND stop_player_button = find_child(main_window, stop_player_button_id);
    const HWND restart_player_button = find_child(main_window, restart_player_button_id);
    const HWND open_player_button = find_child(main_window, open_player_button_id);
    const HWND avd_combo = find_child(main_window, avd_combo_id);
    const HWND core_status = find_child(main_window, core_status_id);
    const HWND runtime_status = find_child(main_window, runtime_status_id);
    const HWND capabilities = find_child(main_window, capabilities_id);
    const HWND log = find_child(main_window, log_id);
    if (start_button == nullptr || stop_button == nullptr || start_player_button == nullptr ||
        stop_player_button == nullptr || restart_player_button == nullptr || open_player_button == nullptr ||
        avd_combo == nullptr || core_status == nullptr || runtime_status == nullptr ||
        capabilities == nullptr || log == nullptr) {
        return fail("Fang Studio is missing an expected control.");
    }

    if (!wait_for_text(core_status, L"Core services: created", 3000) ||
        !wait_for_text(runtime_status, L"Android Emulator:", 3000) ||
        !wait_for_text(capabilities, L"display embedding and guest boot verification are not implemented.", 1000)) {
        return fail("Fang Studio did not report the reference-player mode and its limitations.");
    }

    std::string runtime_error;
    if (test_runtime && !run_reference_runtime(main_window, avd_combo, start_player_button,
            stop_player_button, restart_player_button, open_player_button, runtime_status, 3, argv, runtime_error)) {
        return fail(runtime_error.c_str());
    }

    if (!IsWindowEnabled(start_button) || !invoke_button(main_window, start_button, start_button_id) ||
        !wait_for_text(core_status, L"Core services: running", 3000)) {
        return fail("Start did not transition Fang Studio to the running state.");
    }

    if (!IsWindowEnabled(stop_button) || !invoke_button(main_window, stop_button, stop_button_id) ||
        !wait_for_text(core_status, L"Core services: stopped", 3000)) {
        return fail("Stop did not transition Fang Studio to the stopped state.");
    }

    const auto log_text = read_text(log);
    if (log_text.find(L"core_started") == std::wstring::npos ||
        log_text.find(L"core_stopped") == std::wstring::npos) {
        return fail("Fang Studio did not display both lifecycle log entries.");
    }

    DWORD_PTR result{};
    if (SendMessageTimeoutW(main_window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) == 0 ||
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