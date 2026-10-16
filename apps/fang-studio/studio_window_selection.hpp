#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <vector>

namespace fang::studio {

template <typename IsWindow, typename GetOwnerProcessId>
bool is_window_owned_by_process(HWND window, DWORD expected_process_id,
    IsWindow&& is_window, GetOwnerProcessId&& get_owner_process_id) {
    if (window == nullptr || expected_process_id == 0 || !is_window(window) ||
        get_owner_process_id(window) != expected_process_id) {
        return false;
    }
    return is_window(window) && get_owner_process_id(window) == expected_process_id;
}

template <typename ClassMatches, typename IsWindow, typename GetOwnerProcessId>
HWND find_owned_studio_window(DWORD expected_process_id, const std::vector<HWND>& candidates,
    ClassMatches&& class_matches, IsWindow&& is_window, GetOwnerProcessId&& get_owner_process_id) {
    for (const HWND candidate : candidates) {
        if (class_matches(candidate) && is_window_owned_by_process(candidate, expected_process_id,
                is_window, get_owner_process_id)) {
            return candidate;
        }
    }
    return nullptr;
}

template <typename FindWindow, typename IsProcessRunning, typename IsWindow,
    typename GetOwnerProcessId, typename GetTickCount, typename Sleep>
HWND wait_for_owned_studio_window(DWORD expected_process_id, ULONGLONG timeout_ms,
    FindWindow&& find_window, IsProcessRunning&& is_process_running, IsWindow&& is_window,
    GetOwnerProcessId&& get_owner_process_id, GetTickCount&& get_tick_count, Sleep&& sleep) {
    const ULONGLONG deadline = get_tick_count() + timeout_ms;
    do {
        if (!is_process_running()) {
            return nullptr;
        }
        const HWND candidate = find_window(expected_process_id);
        if (is_window_owned_by_process(candidate, expected_process_id,
                is_window, get_owner_process_id)) {
            return candidate;
        }
        if (get_tick_count() >= deadline) {
            return nullptr;
        }
        sleep();
    } while (get_tick_count() < deadline);
    return nullptr;
}

template <typename IsWindow, typename GetOwnerProcessId, typename Action>
bool invoke_on_owned_window(HWND window, DWORD expected_process_id,
    IsWindow&& is_window, GetOwnerProcessId&& get_owner_process_id, Action&& action) {
    if (!is_window_owned_by_process(window, expected_process_id,
            is_window, get_owner_process_id)) {
        return false;
    }
    return action(window);
}

}
