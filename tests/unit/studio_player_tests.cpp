#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "player_viewport.hpp"
#include "studio_input_router.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

using namespace fang::studio;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_coordinate_mapping() {
    DisplayDimensions display{1080, 1920};
    RECT client_rect{0, 0, 360, 640}; // exact 9:16 aspect ratio

    const auto center_opt = map_client_to_display({180, 320}, client_rect, display);
    require(center_opt.has_value(), "Center coordinate should be mapped.");
    require(std::abs(center_opt->x - 540) <= 2, "Center X should map to ~540.");
    require(std::abs(center_opt->y - 960) <= 2, "Center Y should map to ~960.");

    const auto top_left = map_client_to_display({0, 0}, client_rect, display);
    require(top_left.has_value(), "Top left coordinate should be mapped.");
    require(top_left->x == 0 && top_left->y == 0, "Top left must map to (0, 0).");

    const auto bottom_right = map_client_to_display({360, 640}, client_rect, display);
    require(bottom_right.has_value(), "Bottom right coordinate should be mapped.");
    require(bottom_right->x == 1079 && bottom_right->y == 1919,
        "Bottom right must clamp to (1079, 1919).");

    // Pillarboxed client rect (wide aspect ratio: 800 x 600)
    RECT wide_rect{0, 0, 800, 600};
    const auto display_box = calculate_display_viewport_rect(wide_rect, display);
    require(display_box.left > 0, "Wide client must produce pillarbox margins.");
    require(display_box.right < 800, "Wide client must produce pillarbox margins.");

    // Point in pillarbox margin should not map
    const auto out_of_bounds = map_client_to_display({10, 300}, wide_rect, display);
    require(!out_of_bounds.has_value(), "Point in pillarbox margin must be rejected.");

    // Point in center of pillarboxed display should map
    const auto in_bounds = map_client_to_display({400, 300}, wide_rect, display);
    require(in_bounds.has_value(), "Point in center of pillarboxed display must map.");
}

void test_keycode_translation_and_filtering() {
    require(map_virtual_key_to_android(VK_BACK) == 4, "VK_BACK must map to KEYCODE_BACK (4).");
    require(map_virtual_key_to_android(VK_RETURN) == 66, "VK_RETURN must map to KEYCODE_ENTER (66).");
    require(map_virtual_key_to_android(VK_HOME) == 3, "VK_HOME must map to KEYCODE_HOME (3).");
    require(map_virtual_key_to_android(VK_SPACE) == 62, "VK_SPACE must map to KEYCODE_SPACE (62).");
    require(map_virtual_key_to_android(VK_LEFT) == 21, "VK_LEFT must map to KEYCODE_DPAD_LEFT (21).");
    require(map_virtual_key_to_android(VK_UP) == 19, "VK_UP must map to KEYCODE_DPAD_UP (19).");
    require(map_virtual_key_to_android(VK_RIGHT) == 22, "VK_RIGHT must map to KEYCODE_DPAD_RIGHT (22).");
    require(map_virtual_key_to_android(VK_DOWN) == 20, "VK_DOWN must map to KEYCODE_DPAD_DOWN (20).");

    // '0' -> 7, '9' -> 16
    require(map_virtual_key_to_android('0') == 7, "'0' must map to KEYCODE_0 (7).");
    require(map_virtual_key_to_android('9') == 16, "'9' must map to KEYCODE_9 (16).");

    // 'A' -> 29, 'Z' -> 54
    require(map_virtual_key_to_android('A') == 29, "'A' must map to KEYCODE_A (29).");
    require(map_virtual_key_to_android('Z') == 54, "'Z' must map to KEYCODE_Z (54).");

    // Reserved host keys
    require(is_reserved_host_key(VK_ESCAPE), "VK_ESCAPE must be reserved for host focus exit.");
    require(is_reserved_host_key(VK_TAB), "VK_TAB must be reserved for host dialog navigation.");
    require(is_reserved_host_key(VK_MENU), "Alt key must be reserved for host shortcuts.");
    require(is_reserved_host_key(VK_CONTROL), "Control key must be reserved for host shortcuts.");
    require(is_reserved_host_key(VK_F1), "F1 must be reserved for host help.");
}

void test_input_router_gating_and_queue() {
    auto mock_dispatcher = std::make_shared<MockInputDispatcher>();
    StudioInputRouter router(mock_dispatcher, 4);

    RECT client_rect{0, 0, 360, 640};

    // 1. When guest is NOT ready, inputs must be dropped
    require(!router.is_guest_ready(), "Initial state must not be guest ready.");
    const auto result_not_ready = router.route_pointer_down({180, 320}, client_rect);
    require(result_not_ready == InputRoutingResult::DroppedGuestNotReady,
        "Inputs must be dropped when guest is not ready.");
    require(router.dropped_event_count() == 1, "Dropped counter should increment.");
    require(mock_dispatcher->event_count() == 0, "No event should reach dispatcher when not ready.");

    const auto key_not_ready = router.route_key_down(VK_RETURN);
    require(key_not_ready == InputRoutingResult::DroppedGuestNotReady,
        "Keys must be dropped when guest is not ready.");
    require(router.dropped_event_count() == 2, "Dropped counter should increment.");

    // 2. Set guest ready
    router.set_guest_ready(true);
    router.set_adb_serial("emulator-5554");
    require(router.is_guest_ready(), "Guest ready state must be true.");

    // Route touch down
    const auto down_res = router.route_pointer_down({180, 320}, client_rect);
    require(down_res == InputRoutingResult::Success, "Touch down must succeed when ready.");
    require(mock_dispatcher->event_count() == 1, "Event should reach dispatcher.");
    require(mock_dispatcher->events().back().type == InputEventType::TouchDown,
        "Dispatched event must be TouchDown.");

    // Route key down
    const auto key_res = router.route_key_down(VK_BACK);
    require(key_res == InputRoutingResult::Success, "Key routing must succeed when ready.");
    require(mock_dispatcher->event_count() == 2, "Key event should reach dispatcher.");
    require(mock_dispatcher->events().back().keycode == 4, "Dispatched keycode must be 4.");

    // Reserved host key must be ignored
    const auto esc_res = router.route_key_down(VK_ESCAPE);
    require(esc_res == InputRoutingResult::IgnoredUnmappedKey,
        "Reserved Escape key must be ignored by router.");
    require(mock_dispatcher->event_count() == 2, "Escape must not reach dispatcher.");

    // 3. Queue bounds check without immediate dispatcher
    StudioInputRouter queue_router(nullptr, 3);
    queue_router.set_guest_ready(true);

    require(queue_router.route_key_down('A') == InputRoutingResult::Success, "Item 1 queued.");
    require(queue_router.route_key_down('B') == InputRoutingResult::Success, "Item 2 queued.");
    require(queue_router.route_key_down('C') == InputRoutingResult::Success, "Item 3 queued.");
    require(queue_router.queued_event_count() == 3, "Queue depth should be 3.");

    // 4th item should exceed capacity and drop safely
    const auto overflow_res = queue_router.route_key_down('D');
    require(overflow_res == InputRoutingResult::DroppedQueueFull,
        "Queue overflow must return DroppedQueueFull.");
    require(queue_router.queued_event_count() == 3, "Queue depth must remain capped.");
    require(queue_router.dropped_event_count() == 1, "Dropped counter should record overflow.");
}

void test_viewport_window_lifecycle_and_focus() {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    // Create a temporary hidden host window
    HWND host_window = CreateWindowExW(0, L"STATIC", L"TestHost",
        WS_OVERLAPPEDWINDOW, 0, 0, 600, 600, nullptr, nullptr, instance, nullptr);
    require(host_window != nullptr, "Host window must be created.");

    struct HostGuard {
        HWND window;
        ~HostGuard() { if (window != nullptr) DestroyWindow(window); }
    } host_guard{host_window};

    auto router = std::make_shared<StudioInputRouter>(std::make_shared<MockInputDispatcher>());

    // 1. Create PlayerViewport child control
    auto viewport = std::make_unique<PlayerViewport>(host_window, instance, 2001, router);
    require(viewport->is_valid(), "PlayerViewport window must be valid.");
    require(viewport->cached_status().state == fang::broker::RuntimeLifecycleState::Stopped,
        "Initial status must be Stopped.");
    require(!viewport->has_focus(), "Initial focus must be false.");

    // 2. Resize and move
    viewport->move(10, 10, 300, 500, false);

    // 3. Lifecycle transitions
    fang::broker::RuntimeStatusData status;
    status.state = fang::broker::RuntimeLifecycleState::Starting;
    viewport->update_runtime_state(status, true);
    require(viewport->cached_status().state == fang::broker::RuntimeLifecycleState::Starting,
        "Status must update to Starting.");
    require(!router->is_guest_ready(), "Router must not be ready while Starting.");

    status.state = fang::broker::RuntimeLifecycleState::Ready;
    status.guest_ready = true;
    status.adb_serial = "emulator-5554";
    viewport->update_runtime_state(status, false);
    require(viewport->cached_status().state == fang::broker::RuntimeLifecycleState::Ready,
        "Status must update to Ready.");
    require(router->is_guest_ready(), "Router must be ready when Ready and guest_ready are true.");

    // 4. Focus acquisition
    SendMessageW(viewport->handle(), WM_SETFOCUS, 0, 0);
    require(viewport->has_focus(), "Viewport must report focus after WM_SETFOCUS.");

    // 5. Escape key releases focus back to parent
    SendMessageW(viewport->handle(), WM_KEYDOWN, VK_ESCAPE, 0);
    require(!viewport->has_focus(), "Escape key must release viewport focus.");

    // 6. Focus loss (WM_KILLFOCUS)
    SendMessageW(viewport->handle(), WM_SETFOCUS, 0, 0);
    require(viewport->has_focus(), "Viewport must have focus again.");
    SendMessageW(viewport->handle(), WM_KILLFOCUS, 0, 0);
    require(!viewport->has_focus(), "WM_KILLFOCUS must clear viewport focus.");

    // 7. Mouse interaction and safe capture handling
    SendMessageW(viewport->handle(), WM_LBUTTONDOWN, 0, MAKELPARAM(150, 250));
    require(viewport->has_focus(), "Click must acquire focus.");

    // Capture changed message must not crash and safely clear capture state
    SendMessageW(viewport->handle(), WM_CAPTURECHANGED, 0, 0);

    // 8. Unexpected runtime exit or transition to Failed
    status.state = fang::broker::RuntimeLifecycleState::Failed;
    status.guest_ready = false;
    status.diagnostic = "Unexpected Emulator exit";
    viewport->update_runtime_state(status, false);
    require(viewport->cached_status().state == fang::broker::RuntimeLifecycleState::Failed,
        "Status must reflect Failed.");
    require(!router->is_guest_ready(), "Router must be disabled on failure.");

    // 9. Repeated destruction and re-creation
    viewport.reset(); // Destroy existing viewport
    auto second_viewport = std::make_unique<PlayerViewport>(host_window, instance, 2002, router);
    require(second_viewport->is_valid(), "Second PlayerViewport must be successfully created.");
    second_viewport->show(true);
    second_viewport->show(false);
    second_viewport.reset();
}

}

int main() {
    try {
        test_coordinate_mapping();
        test_keycode_translation_and_filtering();
        test_input_router_gating_and_queue();
        test_viewport_window_lifecycle_and_focus();
        std::cout << "All studio player viewport and input forwarding tests passed successfully.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
