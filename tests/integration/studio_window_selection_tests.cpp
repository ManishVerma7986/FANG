#include "studio_window_selection.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace fang::studio;

constexpr DWORD test_process_id = 42;
constexpr DWORD other_process_id = 84;

HWND test_window(std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct FakeWindows {
    std::unordered_map<HWND, DWORD> owner_process_ids;
    std::unordered_map<HWND, bool> studio_class_matches;

    bool is_window(HWND window) const {
        return owner_process_ids.contains(window);
    }

    DWORD owner_process_id(HWND window) const {
        const auto found = owner_process_ids.find(window);
        return found == owner_process_ids.end() ? 0 : found->second;
    }

    bool has_studio_class(HWND window) const {
        const auto found = studio_class_matches.find(window);
        return found != studio_class_matches.end() && found->second;
    }

    HWND find(const std::vector<HWND>& candidates) const {
        return find_owned_studio_window(test_process_id, candidates,
            [this](HWND window) { return has_studio_class(window); },
            [this](HWND window) { return is_window(window); },
            [this](HWND window) { return owner_process_id(window); });
    }
};

void test_matching_studio_window_is_accepted() {
    FakeWindows windows;
    const HWND candidate = test_window(1);
    windows.owner_process_ids.emplace(candidate, test_process_id);
    windows.studio_class_matches.emplace(candidate, true);

    require(windows.find({candidate}) == candidate,
        "The test-created Studio window was not accepted.");
}

void test_same_class_window_from_other_process_is_rejected() {
    FakeWindows windows;
    const HWND other_studio = test_window(2);
    windows.owner_process_ids.emplace(other_studio, other_process_id);
    windows.studio_class_matches.emplace(other_studio, true);

    require(windows.find({other_studio}) == nullptr,
        "A same-class window from another process was accepted.");
}

void test_missing_window_waits_only_until_timeout() {
    FakeWindows windows;
    constexpr ULONGLONG timeout_ms = 100;
    constexpr ULONGLONG poll_interval_ms = 25;
    ULONGLONG now = 1000;
    int searches{};
    const HWND unrelated_window = test_window(3);
    windows.owner_process_ids.emplace(unrelated_window, other_process_id);
    windows.studio_class_matches.emplace(unrelated_window, false);

    const HWND found = wait_for_owned_studio_window(test_process_id, timeout_ms,
        [&](DWORD) {
            ++searches;
            return windows.find({unrelated_window});
        },
        [] { return true; },
        [&](HWND window) { return windows.is_window(window); },
        [&](HWND window) { return windows.owner_process_id(window); },
        [&] { return now; },
        [&] { now += poll_interval_ms; });

    require(found == nullptr, "A missing owned window was reported as found.");
    require(now >= 1000 + timeout_ms && now <= 1000 + timeout_ms + poll_interval_ms,
        "The missing-window search did not stop at its bounded timeout.");
    require(searches > 0, "The missing-window search did not poll for a candidate.");
}

void test_destroyed_window_is_not_owned() {
    const HWND destroyed_window = test_window(4);
    bool is_window_call{};
    const bool accepted = is_window_owned_by_process(destroyed_window, test_process_id,
        [&](HWND) {
            is_window_call = !is_window_call;
            return is_window_call;
        },
        [](HWND) { return test_process_id; });

    require(!accepted, "A window destroyed during ownership validation was accepted.");
}

void test_reused_window_handle_is_not_owned() {
    const HWND reused_window = test_window(6);
    int owner_queries{};
    const bool accepted = is_window_owned_by_process(reused_window, test_process_id,
        [](HWND) { return true; },
        [&](HWND) { return ++owner_queries == 1 ? test_process_id : other_process_id; });

    require(!accepted, "A window handle reused by another process was accepted.");
}

void test_cleanup_does_not_target_another_studio_process() {
    FakeWindows windows;
    const HWND other_studio = test_window(5);
    windows.owner_process_ids.emplace(other_studio, other_process_id);
    bool close_sent{};

    const bool closed = invoke_on_owned_window(other_studio, test_process_id,
        [&](HWND window) { return windows.is_window(window); },
        [&](HWND window) { return windows.owner_process_id(window); },
        [&](HWND) {
            close_sent = true;
            return true;
        });

    require(!closed && !close_sent,
        "Cleanup sent WM_CLOSE to a Studio window owned by another process.");
}

}

int main() {
    try {
        test_matching_studio_window_is_accepted();
        test_same_class_window_from_other_process_is_rejected();
        test_missing_window_waits_only_until_timeout();
        test_destroyed_window_is_not_owned();
        test_reused_window_handle_is_not_owned();
        test_cleanup_does_not_target_another_studio_process();
    } catch (const std::exception& error) {
        std::cerr << "Studio window selection test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Studio window selection tests passed.\n";
    return 0;
}
