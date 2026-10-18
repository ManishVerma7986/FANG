#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "player_viewport.hpp"
#include "studio_input_router.hpp"
#include "adb_input_dispatcher.hpp"
#include "studio_session.hpp"
#include "input_mapping_types.hpp"
#include "touch_contact_model.hpp"
#include "guest_input_bridge.hpp"
#include "input_profile.hpp"
#include "input_mapping_engine.hpp"
#include "input_ipc_protocol.hpp"
#include "input_transport.hpp"
#include "fake_guest_input_endpoint.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

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
    queue_router.set_adb_serial("emulator-5554");
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

class MockProcessExecutor final : public IProcessExecutor {
public:
    struct Call {
        std::filesystem::path executable;
        std::vector<std::string> arguments;
        std::chrono::milliseconds timeout;
    };

    ProcessExecutionResult execute(
        const std::filesystem::path& executable,
        const std::vector<std::string>& arguments,
        std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lock(mutex_);
        calls_.push_back({executable, arguments, timeout});
        cv_.notify_all();

        if (delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }

        ProcessExecutionResult res;
        res.succeeded = should_succeed_;
        res.exit_code = return_exit_code_;
        res.error = custom_error_;
        return res;
    }

    bool wait_for_calls(std::size_t count, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return calls_.size() >= count; });
    }

    std::vector<Call> calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    void set_result(bool succeeded, DWORD exit_code = 0, std::string error = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        should_succeed_ = succeeded;
        return_exit_code_ = exit_code;
        custom_error_ = std::move(error);
    }

    void set_delay_ms(int ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        delay_ms_ = ms;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Call> calls_;
    bool should_succeed_{true};
    DWORD return_exit_code_{0};
    std::string custom_error_;
    int delay_ms_{0};
};

void test_adb_serial_validation() {
    require(AdbInputDispatcher::is_valid_adb_serial("emulator-5554"), "emulator-5554 must be valid.");
    require(AdbInputDispatcher::is_valid_adb_serial("127.0.0.1:5555"), "IP:Port serial must be valid.");
    require(AdbInputDispatcher::is_valid_adb_serial("device_01-test.adb:5555"), "Complex serial must be valid.");

    require(!AdbInputDispatcher::is_valid_adb_serial(""), "Empty serial must be invalid.");
    require(!AdbInputDispatcher::is_valid_adb_serial("emulator-5554; rm -rf /"), "Shell injection with semicolon must be rejected.");
    require(!AdbInputDispatcher::is_valid_adb_serial("emulator-5554 && whoami"), "Shell injection with && must be rejected.");
    require(!AdbInputDispatcher::is_valid_adb_serial("emulator-5554 | cat"), "Shell pipe must be rejected.");
    require(!AdbInputDispatcher::is_valid_adb_serial("emulator 5554"), "Serial with spaces must be rejected.");
    require(!AdbInputDispatcher::is_valid_adb_serial("\"emulator-5554\""), "Serial with quotes must be rejected.");
    require(!AdbInputDispatcher::is_valid_adb_serial("'emulator-5554'"), "Serial with single quotes must be rejected.");
    require(!AdbInputDispatcher::is_valid_adb_serial("emulator-5554\n"), "Serial with newline must be rejected.");
    require(!AdbInputDispatcher::is_valid_adb_serial("`calc`"), "Serial with backticks must be rejected.");
    require(!AdbInputDispatcher::is_valid_adb_serial("$PATH"), "Serial with $ must be rejected.");

    std::string too_long(65, 'a');
    require(!AdbInputDispatcher::is_valid_adb_serial(too_long), "Serial longer than 64 characters must be rejected.");
}

void test_text_sanitization() {
    require(AdbInputDispatcher::sanitize_text_argument("hello world") == "hello%sworld",
        "Spaces must be converted to %s for Android input text.");
    require(AdbInputDispatcher::sanitize_text_argument("test 1 2 3") == "test%s1%s2%s3",
        "Multiple spaces must all convert to %s.");
    require(AdbInputDispatcher::sanitize_text_argument("bad;rm -rf /") == "badrm%s-rf%s/",
        "Dangerous shell characters like ';' must be stripped.");
    require(AdbInputDispatcher::sanitize_text_argument("") == "",
        "Empty text must return empty.");
}

void test_adb_command_argument_formatting() {
    const std::string serial = "emulator-5554";

    // 1. Tap
    InputEvent tap_event;
    tap_event.type = InputEventType::Tap;
    tap_event.x = 100;
    tap_event.y = 200;
    const auto tap_args = AdbInputDispatcher::build_command_arguments(serial, tap_event);
    require(tap_args.has_value(), "Tap arguments must be built.");
    require(tap_args->size() == 7, "Tap arguments size must be 7.");
    require((*tap_args)[0] == "-s" && (*tap_args)[1] == serial && (*tap_args)[2] == "shell" &&
            (*tap_args)[3] == "input" && (*tap_args)[4] == "tap" &&
            (*tap_args)[5] == "100" && (*tap_args)[6] == "200",
        "Tap command format must match expected Android shell input syntax.");

    // 2. Swipe
    InputEvent swipe_event;
    swipe_event.type = InputEventType::Swipe;
    swipe_event.x = 50;
    swipe_event.y = 100;
    swipe_event.end_x = 500;
    swipe_event.end_y = 800;
    swipe_event.duration_ms = 250;
    const auto swipe_args = AdbInputDispatcher::build_command_arguments(serial, swipe_event);
    require(swipe_args.has_value(), "Swipe arguments must be built.");
    require(swipe_args->size() == 10, "Swipe arguments size must be 10.");
    require((*swipe_args)[3] == "input" && (*swipe_args)[4] == "swipe" &&
            (*swipe_args)[5] == "50" && (*swipe_args)[6] == "100" &&
            (*swipe_args)[7] == "500" && (*swipe_args)[8] == "800" &&
            (*swipe_args)[9] == "250",
        "Swipe command format must match expected Android shell input syntax.");

    // 3. Keyevent
    InputEvent key_event;
    key_event.type = InputEventType::Key;
    key_event.keycode = 66; // KEYCODE_ENTER
    const auto key_args = AdbInputDispatcher::build_command_arguments(serial, key_event);
    require(key_args.has_value(), "Key arguments must be built.");
    require(key_args->size() == 6, "Key arguments size must be 6.");
    require((*key_args)[3] == "input" && (*key_args)[4] == "keyevent" && (*key_args)[5] == "66",
        "Keyevent command format must match expected Android shell input syntax.");

    // 4. Text
    InputEvent text_event;
    text_event.type = InputEventType::Text;
    text_event.text = "Hello World";
    const auto text_args = AdbInputDispatcher::build_command_arguments(serial, text_event);
    require(text_args.has_value(), "Text arguments must be built.");
    require(text_args->size() == 6, "Text arguments size must be 6.");
    require((*text_args)[3] == "input" && (*text_args)[4] == "text" && (*text_args)[5] == "Hello%sWorld",
        "Text command format must match expected Android shell input syntax.");

    // 5. Coalesce TouchDown
    InputEvent down_event;
    down_event.type = InputEventType::TouchDown;
    down_event.x = 100;
    down_event.y = 200;
    const auto coalesced_down = AdbInputDispatcher::build_command_arguments(serial, down_event, true);
    require(coalesced_down.has_value() && coalesced_down->empty(),
        "TouchDown must produce empty arguments when coalescing is enabled.");

    const auto raw_down = AdbInputDispatcher::build_command_arguments(serial, down_event, false);
    require(raw_down.has_value() && raw_down->size() == 8,
        "TouchDown must produce motionevent DOWN when coalescing is disabled.");
    require((*raw_down)[4] == "motionevent" && (*raw_down)[5] == "DOWN",
        "TouchDown arguments must use motionevent DOWN.");

    // 6. Invalid inputs
    InputEvent invalid_coord;
    invalid_coord.type = InputEventType::Tap;
    invalid_coord.x = -1;
    invalid_coord.y = 100;
    require(!AdbInputDispatcher::build_command_arguments(serial, invalid_coord).has_value(),
        "Negative coordinates must be rejected.");

    InputEvent invalid_key;
    invalid_key.type = InputEventType::Key;
    invalid_key.keycode = 0;
    require(!AdbInputDispatcher::build_command_arguments(serial, invalid_key).has_value(),
        "Zero keycode must be rejected.");

    require(!AdbInputDispatcher::build_command_arguments("invalid;serial", tap_event).has_value(),
        "Invalid serial must be rejected.");
}

void test_adb_async_worker_dispatch() {
    auto mock_executor = std::make_shared<MockProcessExecutor>();
    AdbInputDispatcherConfig config;
    config.adb_executable = "test_adb.exe";
    config.max_queue_depth = 16;

    AdbInputDispatcher dispatcher(config, mock_executor);
    dispatcher.bind_session_target("emulator-5554");
    require(!dispatcher.is_running(), "Dispatcher should initially not be running.");

    dispatcher.start();
    require(dispatcher.is_running(), "Dispatcher must report running after start().");

    const std::string serial = "emulator-5554";

    // 1. Dispatch Tap
    InputEvent tap;
    tap.type = InputEventType::Tap;
    tap.x = 200;
    tap.y = 400;
    require(dispatcher.dispatch(serial, tap), "Dispatch tap should succeed.");

    // 2. Dispatch Key
    InputEvent key;
    key.type = InputEventType::Key;
    key.keycode = 4; // KEYCODE_BACK
    require(dispatcher.dispatch(serial, key), "Dispatch key should succeed.");

    // Wait for mock executor to process both calls
    require(mock_executor->wait_for_calls(2), "Worker must process both queued commands.");

    const auto calls = mock_executor->calls();
    require(calls.size() == 2, "Mock executor must have received exactly 2 calls.");
    require(calls[0].executable == "test_adb.exe", "First call must target test_adb.exe.");
    require(calls[0].arguments[4] == "tap", "First call must be tap command.");
    require(calls[0].arguments[5] == "200" && calls[0].arguments[6] == "400", "Tap coordinates must match.");

    require(calls[1].arguments[4] == "keyevent" && calls[1].arguments[5] == "4", "Keyevent keycode must match.");

    require(dispatcher.dispatched_count() == 2, "Dispatched count must be 2.");
    require(dispatcher.failed_count() == 0, "Failed count must be 0.");
    require(dispatcher.dropped_count() == 0, "Dropped count must be 0.");

    dispatcher.stop();
    require(!dispatcher.is_running(), "Dispatcher must report not running after stop().");
}

void test_adb_dispatcher_queue_overflow() {
    auto mock_executor = std::make_shared<MockProcessExecutor>();
    mock_executor->set_delay_ms(100);

    AdbInputDispatcherConfig config;
    config.adb_executable = "test_adb.exe";
    config.max_queue_depth = 2; // tiny queue

    AdbInputDispatcher dispatcher(config, mock_executor);
    dispatcher.bind_session_target("emulator-5554");
    dispatcher.start();

    const std::string serial = "emulator-5554";
    InputEvent tap{InputEventType::Tap, 100, 100, 0, 0, 0, 0, ""};

    int drop_count = 0;
    for (int i = 0; i < 6; ++i) {
        if (!dispatcher.dispatch(serial, tap)) {
            ++drop_count;
        }
    }

    require(drop_count > 0, "At least one event must be dropped due to queue limit.");
    require(dispatcher.dropped_count() > 0, "Dropped counter must reflect queue overflow.");
    require(!dispatcher.last_error().empty(), "Last error must record queue full message.");

    dispatcher.stop();
}

void test_adb_dispatcher_executor_failure() {
    auto mock_executor = std::make_shared<MockProcessExecutor>();
    mock_executor->set_result(false, 1, "Process exited with non-zero code 1");

    AdbInputDispatcherConfig config;
    config.adb_executable = "test_adb.exe";

    AdbInputDispatcher dispatcher(config, mock_executor);
    dispatcher.bind_session_target("emulator-5554");
    dispatcher.start();

    InputEvent tap{InputEventType::Tap, 50, 50, 0, 0, 0, 0, ""};
    require(dispatcher.dispatch("emulator-5554", tap), "Dispatch should accept valid event.");

    require(mock_executor->wait_for_calls(1), "Mock executor should be called.");
    dispatcher.stop();

    require(dispatcher.failed_count() == 1, "Failed count must record executor error.");
    require(dispatcher.dispatched_count() == 0, "Dispatched count must be 0 on failure.");
    require(dispatcher.last_error().find("code 1") != std::string::npos, "Error message must record failure code.");
}

void test_end_to_end_router_with_adb_dispatcher() {
    auto mock_executor = std::make_shared<MockProcessExecutor>();
    AdbInputDispatcherConfig config;
    config.adb_executable = "test_adb.exe";

    auto dispatcher = std::make_shared<AdbInputDispatcher>(config, mock_executor);
    dispatcher->bind_session_target("emulator-5554");
    dispatcher->start();

    StudioInputRouter router(dispatcher, 8);
    router.set_adb_serial("emulator-5554");
    router.set_guest_ready(true);

    RECT client_rect{0, 0, 360, 640};

    // 1. Route pointer up (Tap)
    const auto tap_res = router.route_pointer_up({180, 320}, client_rect);
    require(tap_res == InputRoutingResult::Success, "Route pointer up must succeed.");

    // 2. Route key down (Backspace)
    const auto key_res = router.route_key_down(VK_BACK);
    require(key_res == InputRoutingResult::Success, "Route key down must succeed.");

    require(mock_executor->wait_for_calls(2), "Both routed inputs must execute on mock adb.");

    const auto calls = mock_executor->calls();
    require(calls.size() == 2, "Exactly 2 commands should reach mock ADB executor.");
    require(calls[0].arguments[4] == "tap", "First command must be tap.");
    require(calls[0].arguments[5] == "540" && calls[0].arguments[6] == "960", "Mapped coordinates must be 540x960.");
    require(calls[1].arguments[4] == "keyevent" && calls[1].arguments[5] == "4", "Keyevent keycode must be 4.");

    dispatcher->stop();
}

void test_viewport_renderer_pixel_format_translation() {
    require(ViewportD3D11Renderer::translate_pixel_format_to_dxgi(VideoPixelFormat::RGBA8888) == 28, // DXGI_FORMAT_R8G8B8A8_UNORM
        "RGBA8888 must map to DXGI_FORMAT_R8G8B8A8_UNORM (28).");
    require(ViewportD3D11Renderer::translate_pixel_format_to_dxgi(VideoPixelFormat::BGRA8888) == 87, // DXGI_FORMAT_B8G8R8A8_UNORM
        "BGRA8888 must map to DXGI_FORMAT_B8G8R8A8_UNORM (87).");
    require(ViewportD3D11Renderer::translate_pixel_format_to_dxgi(VideoPixelFormat::RGB888) == 0, // DXGI_FORMAT_UNKNOWN
        "RGB888 without alpha must map to DXGI_FORMAT_UNKNOWN (0).");
    require(ViewportD3D11Renderer::translate_pixel_format_to_dxgi(VideoPixelFormat::Unknown) == 0,
        "Unknown format must map to DXGI_FORMAT_UNKNOWN (0).");
}

void test_viewport_renderer_initialization_and_fallback() {
    ViewportD3D11Renderer renderer;
    require(!renderer.is_initialized(), "Renderer must not be initialized before initialize call.");
    require(renderer.active_backend() == ViewportBackend::None, "Initial backend must be None.");

    // 1. Force GDI fallback
    ViewportRenderOptions force_gdi_opts;
    force_gdi_opts.force_gdi_fallback = true;
    require(renderer.initialize(nullptr, force_gdi_opts), "Initializing with forced GDI must succeed.");
    require(renderer.is_initialized(), "Renderer must be marked initialized.");
    require(renderer.active_backend() == ViewportBackend::GdiFallback, "Active backend must be GdiFallback.");
    require(renderer.backend_name() == "GDI (Software Fallback)", "Backend name must reflect fallback.");
    require(renderer.diagnostic().find("forced") != std::string::npos, "Diagnostic must explain forced fallback.");

    // 2. Clear and shutdown
    renderer.shutdown();
    require(!renderer.is_initialized(), "Renderer must be shut down.");
    require(renderer.active_backend() == ViewportBackend::None, "Backend must be None after shutdown.");

    // 3. Null HWND without forced GDI should fail D3D11 and enter fallback safely
    ViewportRenderOptions normal_opts;
    require(!renderer.initialize(nullptr, normal_opts), "Null window cannot initialize hardware D3D11.");
    require(renderer.is_initialized(), "Renderer safely enters fallback mode on invalid window.");
    require(renderer.active_backend() == ViewportBackend::GdiFallback, "Active backend must fall back to GDI.");
    require(!renderer.diagnostic().empty(), "Diagnostic must record invalid window explanation.");
}

void test_viewport_renderer_frame_validation_and_rejection() {
    ViewportD3D11Renderer renderer;
    ViewportRenderOptions opts;
    opts.force_gdi_fallback = true;
    renderer.initialize(nullptr, opts);

    VideoFrame invalid_frame;
    invalid_frame.width = 0;
    invalid_frame.height = 0;
    RECT rect{0, 0, 100, 100};

    require(!renderer.present_frame(invalid_frame, rect), "Invalid frame must be rejected.");
    const auto m = renderer.metrics();
    require(m.frames_dropped == 1, "Dropped frame count must increment on invalid frame.");
    require(m.frames_submitted == 0, "Frames submitted must not increment on invalid frame.");
    require(!m.diagnostic.empty(), "Diagnostic must record frame rejection reason.");
}

void test_viewport_renderer_resize_handling() {
    ViewportD3D11Renderer renderer;
    require(!renderer.resize(100, 100), "Resize before initialization must return false.");

    ViewportRenderOptions opts;
    opts.force_gdi_fallback = true;
    renderer.initialize(nullptr, opts);

    // Minimized / zero dimensions
    require(renderer.resize(0, 0), "Resize with 0 dimensions (minimized) must succeed.");
    require(renderer.resize(-10, -20), "Resize with negative dimensions must be handled safely.");

    // Normal dimensions in fallback mode
    require(renderer.resize(800, 600), "Resize to 800x600 in fallback mode must succeed.");
    require(renderer.resize(800, 600), "Resize to identical dimensions must be a successful no-op.");
}

void test_viewport_renderer_metrics_and_telemetry() {
    ViewportD3D11Renderer renderer;
    ViewportRenderOptions opts;
    opts.force_gdi_fallback = true;
    renderer.initialize(nullptr, opts);

    auto m_initial = renderer.metrics();
    require(m_initial.frames_submitted == 0, "Initial submitted must be 0.");
    require(m_initial.frames_presented == 0, "Initial presented must be 0.");
    require(m_initial.frames_dropped == 0, "Initial dropped must be 0.");
    require(!m_initial.hardware_accelerated, "Forced fallback must not report hardware accelerated.");

    // Create a mock frame
    const std::uint32_t w = 64;
    const std::uint32_t h = 64;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(w * h * 4, std::uint8_t{128});
    VideoFrame frame;
    frame.width = w;
    frame.height = h;
    frame.stride = w * 4;
    frame.format = VideoPixelFormat::BGRA8888;
    frame.sequence = 42;
    frame.timestamp_us = 12345678ULL;
    frame.pixels = pixels;

    // In GDI fallback mode without target DC, present fails safely
    RECT rect{0, 0, 64, 64};
    require(!renderer.present_frame(frame, rect, nullptr, nullptr),
        "Present in GDI fallback without target DC must fail cleanly.");

    // Now present with a mock memory DC
    HDC screen_dc = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    HBITMAP mem_bmp = CreateCompatibleBitmap(screen_dc, 64, 64);
    auto* old_bmp = SelectObject(mem_dc, mem_bmp);

    bool overlay_called = false;
    auto overlay_cb = [&](HDC dc) {
        overlay_called = (dc == mem_dc);
    };

    require(renderer.present_frame(frame, rect, overlay_cb, mem_dc),
        "Present in GDI fallback with valid DC must succeed.");
    require(overlay_called, "Overlay callback must be invoked during frame presentation.");

    auto m = renderer.metrics();
    require(m.frames_submitted == 2, "Frames submitted must be 2.");
    require(m.frames_presented == 1, "Frames presented must be 1.");
    require(m.last_arrival_timestamp_us == 12345678ULL, "Arrival timestamp must match frame timestamp.");
    require(m.current_width == 64 && m.current_height == 64, "Metrics dimensions must match frame.");
    require(m.current_format == VideoPixelFormat::BGRA8888, "Metrics format must match frame.");

    renderer.clear_metrics();
    auto m_cleared = renderer.metrics();
    require(m_cleared.frames_submitted == 0, "Frames submitted must reset to 0.");
    require(m_cleared.frames_presented == 0, "Frames presented must reset to 0.");

    SelectObject(mem_dc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
}

void test_player_viewport_renderer_injection_and_fallback_display() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW host_wc{};
    host_wc.cbSize = sizeof(host_wc);
    host_wc.lpfnWndProc = DefWindowProcW;
    host_wc.hInstance = instance;
    host_wc.lpszClassName = L"FangStudioRendererTestHost";
    RegisterClassExW(&host_wc);

    HWND host_window = CreateWindowExW(
        0, L"FangStudioRendererTestHost", L"TestHost", WS_OVERLAPPEDWINDOW,
        50, 50, 300, 500, nullptr, nullptr, instance, nullptr);
    require(host_window != nullptr, "Host window must be created.");

    struct HostGuard {
        HWND window;
        ~HostGuard() { if (window != nullptr) DestroyWindow(window); }
    } host_guard{host_window};

    class MockInputDispatcher final : public IInputDispatcher {
    public:
        bool dispatch(const std::string&, const InputEvent&) override { return true; }
    };

    auto router = std::make_shared<StudioInputRouter>(std::make_shared<MockInputDispatcher>());
    auto consumer = std::make_shared<StudioVideoConsumer>(1);

    // Inject custom renderer with forced GDI fallback
    auto custom_renderer = std::make_unique<ViewportD3D11Renderer>();
    ViewportRenderOptions opts;
    opts.force_gdi_fallback = true;
    custom_renderer->initialize(host_window, opts);

    auto viewport = std::make_unique<PlayerViewport>(
        host_window, instance, 5001, router, consumer, std::move(custom_renderer));

    require(viewport->is_valid(), "Viewport with injected renderer must be valid.");
    require(viewport->active_backend() == ViewportBackend::GdiFallback,
        "Injected renderer active backend must be GdiFallback.");
    require(viewport->renderer() != nullptr, "Renderer pointer must be non-null.");

    // Update state to Ready and test paint
    fang::broker::RuntimeStatusData ready_status;
    ready_status.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_status.guest_ready = true;
    viewport->update_runtime_state(ready_status, false);

    auto pixels = std::make_shared<std::vector<std::uint8_t>>(32 * 32 * 4, std::uint8_t{200});
    VideoFrame frame{32, 32, 128, VideoPixelFormat::BGRA8888, 1, 0, pixels};
    require(consumer->publish_frame(std::move(frame)), "Publishing frame must succeed.");

    viewport->move(0, 0, 200, 350, false);
    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);

    const auto metrics = viewport->presentation_metrics();
    require(metrics.frames_presented >= 1, "Presentation metrics must record at least 1 presented frame.");
    require(!metrics.hardware_accelerated, "GDI fallback must not report hardware acceleration.");
}

void test_session_target_binding_and_rejection() {
    auto mock_executor = std::make_shared<MockProcessExecutor>();
    AdbInputDispatcherConfig config;
    config.adb_executable = "test_adb.exe";

    AdbInputDispatcher dispatcher(config, mock_executor);
    dispatcher.start();

    InputEvent tap{InputEventType::Tap, 100, 200, 0, 0, 0, 0, ""};

    // 1. Unbound dispatch must be rejected
    require(!dispatcher.is_session_bound(), "Initially dispatcher must not be bound.");
    require(!dispatcher.dispatch("emulator-5554", tap), "Dispatch without session binding must fail.");
    require(dispatcher.failed_count() == 1, "Failed count must increment on unbound dispatch.");
    require(dispatcher.last_error().find("No session target bound") != std::string::npos,
        "Error message must indicate missing session binding.");

    // 2. Bind target serial
    dispatcher.bind_session_target("emulator-5554");
    require(dispatcher.is_session_bound(), "Dispatcher must report session bound.");
    require(dispatcher.bound_session_target() == "emulator-5554", "Bound target must match.");

    // 3. Dispatch to a different serial must be rejected
    require(!dispatcher.dispatch("emulator-5556", tap), "Mismatched serial dispatch must fail.");
    require(dispatcher.failed_count() == 2, "Failed count must increment on mismatched serial.");
    require(dispatcher.last_error().find("Serial mismatch") != std::string::npos,
        "Error message must indicate serial mismatch.");

    // 4. Dispatch to matching bound serial must succeed
    require(dispatcher.dispatch("emulator-5554", tap), "Matching serial dispatch must succeed.");

    // 5. Unbind clears binding and drops pending
    dispatcher.unbind_session();
    require(!dispatcher.is_session_bound(), "Dispatcher must be unbound after unbind_session().");
    require(!dispatcher.dispatch("emulator-5554", tap), "Dispatch after unbind must fail.");

    dispatcher.stop();
}

void test_session_input_router_ambiguity_and_missing_identity() {
    auto mock_dispatcher = std::make_shared<MockInputDispatcher>();
    StudioInputRouter router(mock_dispatcher, 4);

    // 1. Ready but empty serial -> DroppedNoTargetIdentity
    router.set_guest_ready(true);
    router.set_adb_serial("");
    require(router.route_key_down('A') == InputRoutingResult::DroppedNoTargetIdentity,
        "Input must be dropped when target identity is empty.");
    require(router.dropped_event_count() == 1, "Dropped counter should increment.");

    // 2. Ambiguous target flag set -> DroppedAmbiguousTarget
    router.set_adb_serial("emulator-5554");
    router.set_target_ambiguous(true);
    require(router.is_target_ambiguous(), "Target ambiguous flag must be true.");
    require(router.route_key_down('A') == InputRoutingResult::DroppedAmbiguousTarget,
        "Input must be dropped when target is ambiguous.");
    require(router.dropped_event_count() == 2, "Dropped counter should increment.");

    // 3. Valid target and not ambiguous -> Success
    router.set_target_ambiguous(false);
    require(!router.is_target_ambiguous(), "Target ambiguous flag must be false.");
    require(router.route_key_down('A') == InputRoutingResult::Success,
        "Input routing must succeed when target is verified.");
    require(mock_dispatcher->event_count() == 1, "Event should reach dispatcher.");
}

void test_session_controller_lifecycle_and_session_switching() {
    auto mock_executor = std::make_shared<MockProcessExecutor>();
    AdbInputDispatcherConfig config;
    config.adb_executable = "test_adb.exe";
    auto dispatcher = std::make_shared<AdbInputDispatcher>(config, mock_executor);
    dispatcher->start();

    auto router = std::make_shared<StudioInputRouter>(dispatcher);
    auto consumer = std::make_shared<StudioVideoConsumer>(2);
    auto transport = std::make_shared<SharedMemoryVideoTransport>(consumer);

    StudioSessionController session(router, dispatcher, consumer, transport);
    require(session.snapshot().phase == StudioSessionPhase::NotStarted,
        "Initial session phase must be NotStarted.");

    // 1. Update with Starting state
    SupervisorRuntimeSnapshot start_snap;
    start_snap.runtime.state = fang::broker::RuntimeLifecycleState::Starting;
    start_snap.runtime.generation_id = "gen-1";
    start_snap.runtime.avd_name = "Pixel_7";
    auto snap = session.update(start_snap, ViewportBackend::Direct3D11, 0);
    require(snap.phase == StudioSessionPhase::Starting, "Must transition to Starting phase.");
    require(snap.input_state == StudioInputBindingState::Verifying, "Input state must be Verifying.");
    require(!snap.is_interactive(), "Starting session must not be interactive.");

    // 2. Update with Ready state
    SupervisorRuntimeSnapshot ready_snap;
    ready_snap.runtime.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_snap.runtime.guest_ready = true;
    ready_snap.runtime.generation_id = "gen-1";
    ready_snap.runtime.avd_name = "Pixel_7";
    ready_snap.runtime.adb_serial = "emulator-5554";
    ready_snap.runtime.process_id = 1234;

    snap = session.update(ready_snap, ViewportBackend::Direct3D11, 0);
    require(snap.phase == StudioSessionPhase::ReadyAwaitingVideo,
        "Phase must be ReadyAwaitingVideo before frames arrive.");
    require(snap.input_state == StudioInputBindingState::Bound, "Input must be Bound.");
    require(dispatcher->is_session_bound(), "Dispatcher must be bound.");
    require(dispatcher->bound_session_target() == "emulator-5554", "Bound target must match serial.");
    require(router->is_guest_ready(), "Router must be guest ready.");
    require(snap.is_interactive(), "Ready session must be interactive.");

    // 3. Publish video frame
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(16 * 16 * 4, std::uint8_t{100});
    VideoFrame frame{16, 16, 64, VideoPixelFormat::BGRA8888, 1, 0, pixels};
    consumer->publish_frame(frame);

    snap = session.update(ready_snap, ViewportBackend::Direct3D11, 1);
    require(snap.phase == StudioSessionPhase::ActiveStreaming,
        "Phase must transition to ActiveStreaming once frames arrive.");
    require(snap.video_state == StudioVideoState::Streaming, "Video state must be Streaming.");

    // 4. Session switch: Generation changes to gen-2
    SupervisorRuntimeSnapshot switch_snap;
    switch_snap.runtime.state = fang::broker::RuntimeLifecycleState::Starting;
    switch_snap.runtime.generation_id = "gen-2";
    switch_snap.runtime.avd_name = "Pixel_8";
    switch_snap.runtime.process_id = 5678;

    snap = session.update(switch_snap, ViewportBackend::Direct3D11, 0);
    require(snap.phase == StudioSessionPhase::Starting, "Must transition to Starting on session switch.");
    require(snap.input_state == StudioInputBindingState::Verifying, "Input must be reset from Bound to Verifying.");
    require(!dispatcher->is_session_bound(), "Dispatcher must be unbound on session switch.");
    require(!router->is_guest_ready(), "Router must not be guest ready on session switch.");
    require(!consumer->has_frame(), "Old frames must be purged on session switch.");

    dispatcher->stop();
}

void test_session_controller_ambiguous_devices_rejection() {
    auto mock_executor = std::make_shared<MockProcessExecutor>();
    AdbInputDispatcherConfig config;
    config.adb_executable = "test_adb.exe";
    auto dispatcher = std::make_shared<AdbInputDispatcher>(config, mock_executor);
    dispatcher->start();

    auto router = std::make_shared<StudioInputRouter>(dispatcher);
    auto consumer = std::make_shared<StudioVideoConsumer>(2);
    auto transport = std::make_shared<SharedMemoryVideoTransport>(consumer);
    StudioSessionController session(router, dispatcher, consumer, transport);

    std::vector<std::string> known_devices{"emulator-5554", "emulator-5556"};

    // Runtime reports ready but no serial while multiple devices exist
    SupervisorRuntimeSnapshot snap_input;
    snap_input.runtime.state = fang::broker::RuntimeLifecycleState::Ready;
    snap_input.runtime.guest_ready = true;
    snap_input.runtime.adb_serial = "";

    auto snap = session.update(snap_input, ViewportBackend::Direct3D11, 0, known_devices);
    require(snap.input_state == StudioInputBindingState::AmbiguousOrMultiple,
        "Multiple devices with missing serial must be flagged AmbiguousOrMultiple.");
    require(snap.is_ambiguous_target, "Ambiguous target flag must be set.");
    require(router->is_target_ambiguous(), "Router must have target_ambiguous set.");
    require(!dispatcher->is_session_bound(), "Dispatcher must not be bound when ambiguous.");

    // Semicolon/comma injection in serial
    snap_input.runtime.adb_serial = "emulator-5554; emulator-5556";
    snap = session.update(snap_input, ViewportBackend::Direct3D11, 0, known_devices);
    require(snap.input_state == StudioInputBindingState::AmbiguousOrMultiple,
        "Delimited serials must be rejected as ambiguous.");

    dispatcher->stop();
}

void test_session_controller_failed_startup_recovery() {
    auto router = std::make_shared<StudioInputRouter>();
    auto consumer = std::make_shared<StudioVideoConsumer>(1);
    auto transport = std::make_shared<SharedMemoryVideoTransport>(consumer);
    StudioSessionController session(router, nullptr, consumer, transport);

    // 1. Failed startup
    SupervisorRuntimeSnapshot failed_snap;
    failed_snap.runtime.state = fang::broker::RuntimeLifecycleState::Failed;
    failed_snap.runtime.diagnostic = "Could not reserve ADB port 5554";
    auto snap = session.update(failed_snap, ViewportBackend::GdiFallback, 0);

    require(snap.phase == StudioSessionPhase::Failed, "Phase must be Failed.");
    require(snap.input_state == StudioInputBindingState::Unbound, "Input must remain Unbound.");
    require(snap.diagnostic.find("Could not reserve ADB port") != std::string::npos,
        "Diagnostic must record startup failure reason.");
    require(!snap.is_interactive(), "Failed session must not be interactive.");

    // 2. Recovery attempt
    SupervisorRuntimeSnapshot retry_snap;
    retry_snap.runtime.state = fang::broker::RuntimeLifecycleState::Starting;
    retry_snap.runtime.generation_id = "gen-retry-1";
    retry_snap.runtime.avd_name = "Pixel_7";
    snap = session.update(retry_snap, ViewportBackend::GdiFallback, 0);

    require(snap.phase == StudioSessionPhase::Starting, "Must recover to Starting phase.");
    require(snap.diagnostic.empty(), "Diagnostic should be cleared on new starting generation.");
}

void test_session_shutdown_with_active_workers_and_queue() {
    auto mock_executor = std::make_shared<MockProcessExecutor>();
    mock_executor->set_delay_ms(50);

    AdbInputDispatcherConfig config;
    config.adb_executable = "test_adb.exe";
    config.max_queue_depth = 16;
    auto dispatcher = std::make_shared<AdbInputDispatcher>(config, mock_executor);
    dispatcher->bind_session_target("emulator-5554");
    dispatcher->start();

    auto router = std::make_shared<StudioInputRouter>(dispatcher);
    auto consumer = std::make_shared<StudioVideoConsumer>(1);
    auto transport = std::make_shared<SharedMemoryVideoTransport>(consumer);
    StudioSessionController session(router, dispatcher, consumer, transport);

    SupervisorRuntimeSnapshot ready_snap;
    ready_snap.runtime.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_snap.runtime.guest_ready = true;
    ready_snap.runtime.adb_serial = "emulator-5554";
    session.update(ready_snap, ViewportBackend::Direct3D11, 0);

    // Queue 5 events
    InputEvent tap{InputEventType::Tap, 100, 100, 0, 0, 0, 0, ""};
    for (int i = 0; i < 5; ++i) {
        dispatcher->dispatch("emulator-5554", tap);
    }

    // Notify shutdown
    session.notify_shutdown();
    const auto snap = session.snapshot();
    require(snap.phase == StudioSessionPhase::Stopped, "Phase must be Stopped after notify_shutdown.");
    require(!dispatcher->is_session_bound(), "Dispatcher must be unbound after shutdown.");
    require(dispatcher->queue_depth() == 0, "Queue must be immediately drained on shutdown.");

    dispatcher->stop();
    require(!dispatcher->is_running(), "Dispatcher must be stopped.");
}

void test_player_viewport_session_states_and_fallback_rendering() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND host_window = CreateWindowExW(
        0, L"STATIC", L"ViewportSessionTestHost", WS_OVERLAPPEDWINDOW,
        0, 0, 400, 700, nullptr, nullptr, instance, nullptr);
    require(host_window != nullptr, "Host window must be created.");

    struct HostGuard {
        HWND window;
        ~HostGuard() { if (window != nullptr) DestroyWindow(window); }
    } host_guard{host_window};

    auto router = std::make_shared<StudioInputRouter>();
    auto consumer = std::make_shared<StudioVideoConsumer>(2);
    auto renderer = std::make_unique<ViewportD3D11Renderer>();
    ViewportRenderOptions opts;
    opts.force_gdi_fallback = true;
    renderer->initialize(host_window, opts);

    auto viewport = std::make_unique<PlayerViewport>(
        host_window, instance, 6001, router, consumer, std::move(renderer));
    require(viewport->is_valid(), "PlayerViewport must be valid.");

    // 1. ReadyAwaitingVideo session state
    StudioSessionSnapshot awaiting_snap;
    awaiting_snap.phase = StudioSessionPhase::ReadyAwaitingVideo;
    awaiting_snap.input_state = StudioInputBindingState::Bound;
    awaiting_snap.video_state = StudioVideoState::AwaitingFirstFrame;
    awaiting_snap.adb_serial = "emulator-5554";
    awaiting_snap.guest_ready = true;

    viewport->update_session_state(awaiting_snap);
    require(viewport->session_state().phase == StudioSessionPhase::ReadyAwaitingVideo,
        "Viewport must store ReadyAwaitingVideo phase.");
    require(viewport->session_state().input_state == StudioInputBindingState::Bound,
        "Viewport must store Bound input state.");

    viewport->move(0, 0, 300, 500, false);
    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);

    // 2. ActiveStreaming session state
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(32 * 32 * 4, std::uint8_t{220});
    VideoFrame frame{32, 32, 128, VideoPixelFormat::BGRA8888, 1, 0, pixels};
    consumer->publish_frame(std::move(frame));

    StudioSessionSnapshot streaming_snap = awaiting_snap;
    streaming_snap.phase = StudioSessionPhase::ActiveStreaming;
    streaming_snap.video_state = StudioVideoState::Streaming;
    viewport->update_session_state(streaming_snap);

    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);
    const auto metrics = viewport->presentation_metrics();
    require(metrics.frames_presented >= 1, "Frames presented must be at least 1.");

    // 3. Ambiguous session state
    StudioSessionSnapshot ambig_snap = awaiting_snap;
    ambig_snap.input_state = StudioInputBindingState::AmbiguousOrMultiple;
    viewport->update_session_state(ambig_snap);
    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);
    require(viewport->session_state().input_state == StudioInputBindingState::AmbiguousOrMultiple,
        "Ambiguous input state must be stored.");
}

void test_profile_parsing_validation_and_errors() {
    // 1. Valid profile JSON parsing
    std::string valid_json = "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"custom_profile\",\n"
        "  \"name\": \"Custom Profile\",\n"
        "  \"aim_sensitivity\": 1.5,\n"
        "  \"aim_deadzone\": 0.05,\n"
        "  \"key_bindings\": [\n"
        "    {\"host_key\": 87, \"action_type\": 1, \"direction\": 0, \"name\": \"Move Up\"},\n"
        "    {\"host_key\": 32, \"action_type\": 0, \"android_keycode\": 62, \"name\": \"Jump\"}\n"
        "  ],\n"
        "  \"mouse_bindings\": [\n"
        "    {\"button\": 0, \"action_type\": 4, \"contact_id\": 0, \"name\": \"Primary Touch\"},\n"
        "    {\"button\": 1, \"action_type\": 3, \"name\": \"Toggle Aim\"}\n"
        "  ]\n"
        "}";

    std::string parse_err;
    auto parsed_opt = InputProfile::parse_from_json(valid_json, parse_err);
    require(parsed_opt.has_value(), "Valid JSON should parse into InputProfile: " + parse_err);
    require(parsed_opt->id == "custom_profile", "Profile ID should match.");
    require(parsed_opt->schema_version == 1, "Schema version should be 1.");
    require(std::abs(parsed_opt->aim_sensitivity - 1.5f) < 0.001f, "Aim sensitivity should be 1.5.");
    require(std::abs(parsed_opt->aim_deadzone - 0.05f) < 0.001f, "Aim deadzone should be 0.05.");
    require(parsed_opt->key_bindings.size() == 2, "Should have 2 key bindings.");
    require(parsed_opt->mouse_bindings.size() == 2, "Should have 2 mouse bindings.");

    std::string val_err;
    require(InputProfile::validate(*parsed_opt, val_err), "Parsed profile should pass validation: " + val_err);

    // 2. Round-trip serialization
    std::string serialized = InputProfile::serialize_to_json(*parsed_opt);
    std::string roundtrip_err;
    auto roundtrip_opt = InputProfile::parse_from_json(serialized, roundtrip_err);
    require(roundtrip_opt.has_value(), "Serialized profile should be re-parseable: " + roundtrip_err);
    require(roundtrip_opt->id == parsed_opt->id, "Round-trip profile ID must match.");
    require(roundtrip_opt->key_bindings.size() == parsed_opt->key_bindings.size(), "Key bindings count must match.");

    // 3. Unsupported schema version
    InputProfile v2_profile = *parsed_opt;
    v2_profile.schema_version = 2;
    std::string v2_error;
    require(!InputProfile::validate(v2_profile, v2_error), "Schema version 2 must be rejected.");
    require(v2_error.find("Unsupported schema_version") != std::string::npos, "Validation error should mention version.");

    // 4. Invalid numeric parameters
    InputProfile bad_sens = *parsed_opt;
    bad_sens.aim_sensitivity = -1.0f;
    std::string sens_err;
    require(!InputProfile::validate(bad_sens, sens_err), "Negative sensitivity must be rejected.");

    bad_sens.aim_sensitivity = 20.0f;
    require(!InputProfile::validate(bad_sens, sens_err), "Excessive sensitivity (>10) must be rejected.");

    InputProfile bad_deadzone = *parsed_opt;
    bad_deadzone.aim_deadzone = -0.1f;
    std::string deadzone_err;
    require(!InputProfile::validate(bad_deadzone, deadzone_err), "Negative deadzone must be rejected.");

    bad_deadzone.aim_deadzone = 0.55f;
    require(!InputProfile::validate(bad_deadzone, deadzone_err), "Deadzone >= 0.5 must be rejected.");

    // 5. Invalid keycode
    InputProfile bad_key = *parsed_opt;
    bad_key.key_bindings.push_back(KeyBinding{0, MappedActionType::AndroidKey, 62, MovementDirection::Up, {}, 0, "Bad Key"});
    std::string bad_key_err;
    require(!InputProfile::validate(bad_key, bad_key_err), "Keycode 0 must be rejected.");

    // 6. Duplicate key bindings
    InputProfile dup_key = *parsed_opt;
    dup_key.key_bindings.push_back(KeyBinding{87, MappedActionType::AndroidKey, 62, MovementDirection::Up, {}, 0, "Dup W"});
    std::string dup_key_err;
    require(!InputProfile::validate(dup_key, dup_key_err), "Duplicate key binding must be rejected.");

    // 7. Malformed JSON
    std::string malformed_err;
    require(!InputProfile::parse_from_json("{broken json", malformed_err).has_value(), "Broken JSON must return nullopt.");
    require(!InputProfile::parse_from_json("", malformed_err).has_value(), "Empty string must return nullopt.");
}

void test_default_profile_bindings() {
    auto default_profile = InputProfile::create_default_gaming();
    std::string error;
    require(InputProfile::validate(default_profile, error), "Default gaming profile must validate: " + error);
    require(default_profile.id == "default_gaming", "Default profile ID should be default_gaming.");
    require(default_profile.schema_version == 1, "Default schema version must be 1.");

    // Check WASD directional movement
    const auto* w_binding = default_profile.find_key_binding('W');
    require(w_binding != nullptr, "Binding for W must exist.");
    require(w_binding->action_type == MappedActionType::DirectionalMovement, "W must be DirectionalMovement.");
    require(w_binding->direction == MovementDirection::Up, "W direction must be Up.");

    const auto* s_binding = default_profile.find_key_binding('S');
    require(s_binding != nullptr, "Binding for S must exist.");
    require(s_binding->direction == MovementDirection::Down, "S direction must be Down.");

    const auto* a_binding = default_profile.find_key_binding('A');
    require(a_binding != nullptr, "Binding for A must exist.");
    require(a_binding->direction == MovementDirection::Left, "A direction must be Left.");

    const auto* d_binding = default_profile.find_key_binding('D');
    require(d_binding != nullptr, "Binding for D must exist.");
    require(d_binding->direction == MovementDirection::Right, "D direction must be Right.");

    // Space -> Jump (KEYCODE_SPACE = 62)
    const auto* space_binding = default_profile.find_key_binding(VK_SPACE);
    require(space_binding != nullptr, "Binding for Space must exist.");
    require(space_binding->android_keycode == 62, "Space must map to KEYCODE_SPACE (62).");

    // Mouse buttons
    const auto* left_mouse = default_profile.find_mouse_binding(HostMouseButton::Left);
    require(left_mouse != nullptr, "Left mouse button binding must exist.");
    require(left_mouse->action_type == MappedActionType::TouchContact, "Left mouse must be TouchContact.");

    const auto* right_mouse = default_profile.find_mouse_binding(HostMouseButton::Right);
    require(right_mouse != nullptr, "Right mouse button binding must exist.");
    require(right_mouse->action_type == MappedActionType::AimModeToggle, "Right mouse must be AimModeToggle.");

    // Esc
    const auto* esc_binding = default_profile.find_key_binding(VK_ESCAPE);
    require(esc_binding != nullptr, "Binding for Escape must exist.");
    require(esc_binding->action_type == MappedActionType::AimModeToggle, "Escape must map to AimModeToggle.");
}

void test_key_transitions_and_repeat_suppression() {
    InputMappingEngine engine;
    engine.set_display_dimensions({1080, 1920});

    // 1. Viewport unfocused -> inputs rejected
    engine.set_viewport_focused(false);
    HostKeyboardEvent evt_w_down{'W', true, false, 1000};
    auto res = engine.process_keyboard_event(evt_w_down);
    require(!res.success, "Keydown while unfocused must be rejected.");
    require(res.rejection_reason == MappingRejectionReason::ViewportUnfocused, "Rejection reason must be ViewportUnfocused.");

    // 2. Viewport focused -> valid keydown
    engine.set_viewport_focused(true);
    res = engine.process_keyboard_event(evt_w_down);
    require(res.success, "Keydown while focused must be accepted.");
    require(res.actions.front().type == MappedActionType::DirectionalMovement, "W must produce DirectionalMovement.");
    require(res.actions.front().is_down, "Action must indicate key down.");
    require(engine.is_key_held('W'), "W must be tracked as held.");

    // 3. Typematic repeated keydown -> suppressed
    HostKeyboardEvent evt_w_repeat{'W', true, true, 1050};
    res = engine.process_keyboard_event(evt_w_repeat);
    require(!res.success, "Repeated keydown must be suppressed.");
    require(res.rejection_reason == MappingRejectionReason::RepeatedKeyDownSuppressed, "Rejection reason must be RepeatedKeyDownSuppressed.");
    require(engine.is_key_held('W'), "W must remain held.");

    // 4. Keyup -> valid release
    HostKeyboardEvent evt_w_up{'W', false, false, 1100};
    res = engine.process_keyboard_event(evt_w_up);
    require(res.success, "Keyup must be accepted.");
    require(!res.actions.front().is_down, "Action must indicate key release.");
    require(!engine.is_key_held('W'), "W must no longer be held.");

    // 5. Unmapped key -> rejected
    HostKeyboardEvent evt_unmapped{VK_F12, true, false, 1200};
    res = engine.process_keyboard_event(evt_unmapped);
    require(!res.success, "Unmapped key must be rejected.");
    require(res.rejection_reason == MappingRejectionReason::UnmappedInput, "Reason must be UnmappedInput.");
}

void test_pointer_aim_and_mouse_buttons() {
    InputMappingEngine engine;
    engine.set_viewport_focused(true);
    engine.set_display_dimensions({1080, 1920});
    RECT client_rect{0, 0, 360, 640};

    // 1. Pointer move with aim mode disabled -> rejected
    require(!engine.is_aim_mode_enabled(), "Aim mode must initially be disabled.");
    HostPointerMoveEvent move_evt{180, 320, 15, -8, 2000};
    auto res_move = engine.process_pointer_move(move_evt, client_rect);
    require(!res_move.success, "Pointer move rejected when aim mode is disabled.");
    require(res_move.rejection_reason == MappingRejectionReason::GameplayModeDisabled, "Reason must be GameplayModeDisabled.");

    // 2. Right click -> toggles aim mode ON
    HostPointerButtonEvent rdown{HostMouseButton::Right, true, 180, 320, 2010};
    auto res_rdown = engine.process_pointer_button(rdown, client_rect);
    require(res_rdown.success, "Right click down should be accepted.");
    require(res_rdown.actions.front().type == MappedActionType::AimModeToggle, "Right click must be AimModeToggle.");
    require(engine.is_aim_mode_enabled(), "Aim mode should now be enabled.");

    // 3. Pointer move with aim mode enabled -> produces PointerAim action
    res_move = engine.process_pointer_move(move_evt, client_rect);
    require(res_move.success, "Pointer move should be accepted when aim mode is active.");
    require(res_move.actions.front().type == MappedActionType::PointerAim, "Should produce PointerAim action.");
    require(res_move.actions.front().aim_delta_x > 0.0f, "aim_delta_x should be positive.");
    require(res_move.actions.front().aim_delta_y < 0.0f, "aim_delta_y should be negative.");

    // 4. Deadzone filtering
    HostPointerMoveEvent tiny_move{180, 320, 0, 0, 2020};
    auto res_tiny = engine.process_pointer_move(tiny_move, client_rect);
    require(res_tiny.success, "Zero move is accepted.");
    require(res_tiny.actions.front().aim_delta_x == 0.0f && res_tiny.actions.front().aim_delta_y == 0.0f, "Zero delta must be zero.");

    // 5. Left click down -> TouchContact Down
    HostPointerButtonEvent ldown{HostMouseButton::Left, true, 180, 320, 2030};
    auto res_ldown = engine.process_pointer_button(ldown, client_rect);
    require(res_ldown.success, "Left click down should be accepted.");
    require(res_ldown.actions.front().type == MappedActionType::TouchContact, "Should produce TouchContact.");
    require(res_ldown.actions.front().touch_transition == TouchTransition::Down, "Transition must be Down.");
    require(res_ldown.actions.front().contact_id == 0, "Contact ID should be 0.");
    require(engine.is_button_held(HostMouseButton::Left), "Left button should be recorded as held.");
    require(engine.active_contact_count() == 1, "Touch slot 0 should be active.");

    // 6. Left click up -> TouchContact Up
    HostPointerButtonEvent lup{HostMouseButton::Left, false, 180, 320, 2040};
    auto res_lup = engine.process_pointer_button(lup, client_rect);
    require(res_lup.success, "Left click up should be accepted.");
    require(res_lup.actions.front().touch_transition == TouchTransition::Up, "Transition must be Up.");
    require(!engine.is_button_held(HostMouseButton::Left), "Left button should no longer be held.");
    require(engine.active_contact_count() == 0, "Touch slot 0 should no longer be active.");
}

void test_profile_switching_held_state_cleanup() {
    InputMappingEngine engine;
    engine.set_viewport_focused(true);
    engine.set_display_dimensions({1080, 1920});
    RECT client_rect{0, 0, 360, 640};

    // Hold 'W' and 'A'
    engine.process_keyboard_event({'W', true, false, 3000});
    engine.process_keyboard_event({'A', true, false, 3010});
    // Hold Left mouse button
    engine.process_pointer_button({HostMouseButton::Left, true, 180, 320, 3020}, client_rect);

    require(engine.is_key_held('W'), "W should be held.");
    require(engine.is_key_held('A'), "A should be held.");
    require(engine.is_button_held(HostMouseButton::Left), "Left button should be held.");
    require(engine.active_contact_count() == 1, "1 contact should be active.");

    // Switch profile to custom profile
    InputProfile custom_profile;
    custom_profile.id = "test_switch";
    custom_profile.schema_version = 1;
    custom_profile.aim_sensitivity = 1.0f;
    custom_profile.aim_deadzone = 0.05f;

    auto cleanup_actions = engine.set_profile(custom_profile);
    require(cleanup_actions.size() >= 3, "Profile switch must emit cleanup actions for held controls.");

    // Verify all host state is clean
    require(!engine.is_key_held('W'), "W must not remain held after profile switch.");
    require(!engine.is_key_held('A'), "A must not remain held after profile switch.");
    require(!engine.is_button_held(HostMouseButton::Left), "Left button must not remain held after profile switch.");
    require(engine.active_contact_count() == 0, "No contacts should remain active.");
    require(engine.active_profile().id == "test_switch", "Profile ID should now be test_switch.");
}

void test_focus_loss_and_shutdown_cleanup() {
    InputMappingEngine engine;
    engine.set_viewport_focused(true);
    engine.set_display_dimensions({1080, 1920});
    RECT client_rect{0, 0, 360, 640};

    // Turn on aim mode, hold 'W', hold Left button
    engine.process_pointer_button({HostMouseButton::Right, true, 180, 320, 4000}, client_rect);
    require(engine.is_aim_mode_enabled(), "Aim mode enabled.");
    engine.process_keyboard_event({'W', true, false, 4010});
    engine.process_pointer_button({HostMouseButton::Left, true, 180, 320, 4020}, client_rect);

    // Focus loss
    auto focus_cleanup = engine.process_focus_loss();
    require(!engine.is_viewport_focused(), "Viewport focus should be false.");
    require(!engine.is_aim_mode_enabled(), "Aim mode must be cancelled on focus loss.");
    require(!engine.is_key_held('W'), "W must be released on focus loss.");
    require(!engine.is_button_held(HostMouseButton::Left), "Left button must be released on focus loss.");
    require(engine.active_contact_count() == 0, "Active contacts must be cancelled on focus loss.");

    bool found_w_release = false;
    bool found_contact_cancel = false;
    for (const auto& act : focus_cleanup) {
        if (act.type == MappedActionType::DirectionalMovement && !act.is_down) {
            found_w_release = true;
        }
        if (act.type == MappedActionType::TouchContact && act.touch_transition == TouchTransition::Cancel) {
            found_contact_cancel = true;
        }
    }
    require(found_w_release, "Focus loss must emit release for W.");
    require(found_contact_cancel, "Focus loss must emit cancellation for active contact.");

    // Subsequent reset_all_state
    auto reset_cleanup = engine.reset_all_state();
    require(reset_cleanup.empty(), "Reset after clean state emits no cleanup actions.");
}

void test_session_generation_tracking() {
    auto mock_executor = std::make_shared<MockProcessExecutor>();
    AdbInputDispatcherConfig config;
    config.adb_executable = "test_adb.exe";
    auto dispatcher = std::make_shared<AdbInputDispatcher>(config, mock_executor);
    dispatcher->start();

    auto router = std::make_shared<StudioInputRouter>(dispatcher);
    auto consumer = std::make_shared<StudioVideoConsumer>(2);
    auto transport = std::make_shared<SharedMemoryVideoTransport>(consumer);
    auto mapping_engine = std::make_shared<InputMappingEngine>();

    StudioSessionController session(router, dispatcher, consumer, transport, mapping_engine);

    SupervisorRuntimeSnapshot ready_snap;
    ready_snap.runtime.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_snap.runtime.guest_ready = true;
    ready_snap.runtime.generation_id = "gen-1";
    ready_snap.runtime.adb_serial = "emulator-5554";
    ready_snap.runtime.process_id = 1234;

    auto snap = session.update(ready_snap, ViewportBackend::Direct3D11, 0);
    require(snap.generation_id == "gen-1", "Session must reflect gen-1.");
    require(router->is_guest_ready(), "Router must be guest ready.");

    // Advance generation to gen-2
    SupervisorRuntimeSnapshot switch_snap;
    switch_snap.runtime.state = fang::broker::RuntimeLifecycleState::Starting;
    switch_snap.runtime.generation_id = "gen-2";
    switch_snap.runtime.process_id = 5678;

    snap = session.update(switch_snap, ViewportBackend::Direct3D11, 0);
    require(snap.generation_id == "gen-2", "Session must reflect gen-2.");
    require(!router->is_guest_ready(), "Router must not be guest ready during gen switch.");

    dispatcher->stop();
}

void test_mapping_rejection_guest_unready_and_ambiguous() {
    auto mock_dispatcher = std::make_shared<MockInputDispatcher>();
    StudioInputRouter router(mock_dispatcher, 8);

    MappedAction act;
    act.type = MappedActionType::AndroidKey;
    act.android_keycode = 62;
    act.is_down = true;

    // 1. Guest not ready
    router.set_guest_ready(false);
    router.set_target_ambiguous(false);
    auto res = router.route_mapped_action(act);
    require(res == InputRoutingResult::DroppedGuestNotReady, "Must drop when guest is not ready.");

    // 2. Ambiguous target binding
    router.set_guest_ready(true);
    router.set_target_ambiguous(true);
    res = router.route_mapped_action(act);
    require(res == InputRoutingResult::DroppedAmbiguousTarget, "Must drop when target is ambiguous.");
}

void test_touch_contact_model_coordinate_conversions() {
    DisplayDimensions display{1080, 1920};

    // 1. 1:1 Aspect ratio match
    RECT client_rect{0, 0, 360, 640};

    auto norm_opt = MultiTouchContactModel::map_client_to_normalized({180, 320}, client_rect, display, true);
    require(norm_opt.has_value(), "Center point should map to normalized.");
    require(std::abs(norm_opt->u - 0.5f) < 0.01f, "Normalized U should be ~0.5.");
    require(std::abs(norm_opt->v - 0.5f) < 0.01f, "Normalized V should be ~0.5.");

    auto guest_pt = MultiTouchContactModel::map_normalized_to_guest(*norm_opt, display);
    require(std::abs(guest_pt.x - 540) <= 2, "Guest X should be ~540.");
    require(std::abs(guest_pt.y - 960) <= 2, "Guest Y should be ~960.");

    // 2. Pillarboxing / Letterboxing
    RECT wide_rect{0, 0, 800, 600};

    // Point in pillarbox margin with clamp=false should be nullopt
    auto out_norm = MultiTouchContactModel::map_client_to_normalized({10, 300}, wide_rect, display, false);
    require(!out_norm.has_value(), "Point in pillarbox margin should not map when clamp=false.");

    // Point in pillarbox margin with clamp=true should clamp to [0.0, 1.0]
    auto clamped_norm = MultiTouchContactModel::map_client_to_normalized({10, 300}, wide_rect, display, true);
    require(clamped_norm.has_value(), "Point in pillarbox margin should clamp when clamp=true.");
    require(clamped_norm->u == 0.0f, "Clamped margin U must be 0.0.");

    // 3. Display boundary clamping
    auto bottom_right = MultiTouchContactModel::map_normalized_to_guest({1.0f, 1.0f}, display);
    require(bottom_right.x == 1079, "Max guest X must clamp to 1079.");
    require(bottom_right.y == 1919, "Max guest Y must clamp to 1919.");

    auto top_left = MultiTouchContactModel::map_normalized_to_guest({0.0f, 0.0f}, display);
    require(top_left.x == 0, "Min guest X must be 0.");
    require(top_left.y == 0, "Min guest Y must be 0.");
}

void test_multi_contact_transitions_and_limits() {
    MultiTouchContactModel model;
    DisplayDimensions display{1080, 1920};

    // 1. Begin contacts
    auto res0 = model.begin_contact(0, {0.2f, 0.3f}, display, 5000);
    require(res0.success, "Begin contact 0 should succeed.");
    require(model.active_contact_count() == 1, "Active contacts should be 1.");

    auto res1 = model.begin_contact(1, {0.7f, 0.8f}, display, 5010);
    require(res1.success, "Begin contact 1 should succeed.");
    require(model.active_contact_count() == 2, "Active contacts should be 2.");

    // 2. Move contacts
    auto res_move = model.move_contact(0, {0.25f, 0.35f}, display, 5020);
    require(res_move.success, "Move contact 0 should succeed.");
    require(res_move.actions.front().touch_transition == TouchTransition::Move, "Transition should be Move.");

    // 3. End contact
    auto res_end = model.end_contact(1, 5030);
    require(res_end.success, "End contact 1 should succeed.");
    require(model.active_contact_count() == 1, "Active contacts should be 1.");
    require(!model.is_contact_active(1), "Contact 1 should no longer be active.");

    // 4. Invalid transitions on inactive contact
    auto res_bad_move = model.move_contact(1, {0.8f, 0.8f}, display, 5040);
    require(!res_bad_move.success, "Moving inactive contact must be rejected.");
    require(res_bad_move.rejection_reason == MappingRejectionReason::InvalidContactId, "Reason must be InvalidContactId.");

    auto res_bad_end = model.end_contact(1, 5050);
    require(!res_bad_end.success, "Ending inactive contact must be rejected.");

    // 5. Contact ID exceeds maximum
    auto res_oob = model.begin_contact(99, {0.5f, 0.5f}, display, 5060);
    require(!res_oob.success, "Contact ID 99 must be rejected.");
    require(res_oob.rejection_reason == MappingRejectionReason::InvalidContactId, "Reason must be InvalidContactId.");

    // 6. Cancel all contacts
    auto cancel_acts = model.cancel_all_contacts();
    require(cancel_acts.size() == 1, "Should cancel the 1 remaining active contact.");
    require(cancel_acts[0].contact_id == 0, "Cancelled contact should be 0.");
    require(cancel_acts[0].touch_transition == TouchTransition::Cancel, "Transition must be Cancel.");
    require(model.active_contact_count() == 0, "Active contacts must be 0 after cancel all.");
}

void test_multi_touch_capability_reporting() {
    UnconfiguredAdbGuestInputBridge adb_bridge;
    require(adb_bridge.capability() == MultiTouchBackendCapability::Unsupported,
        "ADB bridge must explicitly report Unsupported for multi-touch capability.");
    require(!adb_bridge.status_diagnostic().empty(), "Status description must be non-empty.");
    require(adb_bridge.status_diagnostic().find("ADB") != std::string::npos,
        "Status description should mention ADB CLI limitations.");

    // Dispatching touch to unsupported backend
    std::vector<TouchContactState> contacts;
    require(!adb_bridge.send_touch_frame(contacts),
        "Sending multi-touch frame to unconfigured ADB bridge must fail safely.");

    // Mapping engine exposes bridge capability
    InputMappingEngine engine;
    require(engine.multi_touch_capability() == MultiTouchBackendCapability::Unsupported,
        "Engine default capability must match unconfigured bridge.");

    require(!engine.contact_model().backend_capability_diagnostic().empty(),
        "Diagnostics should describe backend capability.");
}

void test_input_queue_overflow_no_stuck_state() {
    // Small queue capacity = 2, no immediate dispatcher so items are queued
    StudioInputRouter router(nullptr, 2);
    router.set_adb_serial("emulator-5554");
    router.set_guest_ready(true);
    router.set_target_ambiguous(false);

    InputMappingEngine engine;
    engine.set_viewport_focused(true);
    engine.set_display_dimensions({1080, 1920});

    // Press 'W' -> queued
    auto res_w = engine.process_keyboard_event({'W', true, false, 7000});
    require(res_w.success, "Keydown W should succeed.");
    auto r_res1 = router.route_mapped_action(res_w.actions.front());
    require(r_res1 == InputRoutingResult::Success, "Action 1 should queue successfully.");

    // Press 'A' -> queued
    auto res_a = engine.process_keyboard_event({'A', true, false, 7010});
    require(res_a.success, "Keydown A should succeed.");
    auto r_res2 = router.route_mapped_action(res_a.actions.front());
    require(r_res2 == InputRoutingResult::Success, "Action 2 should queue successfully.");

    // Press 'S' -> queue overflow!
    auto res_s = engine.process_keyboard_event({'S', true, false, 7020});
    require(res_s.success, "Keydown S should succeed in engine.");
    auto r_res3 = router.route_mapped_action(res_s.actions.front());
    require(r_res3 == InputRoutingResult::DroppedQueueFull, "Action 3 must be dropped due to queue overflow.");
    require(router.dropped_event_count() == 1, "Dropped count should be 1.");

    // Even though routing dropped Action 3, engine maintains coherent host state
    require(engine.is_key_held('W'), "W is held.");
    require(engine.is_key_held('A'), "A is held.");
    require(engine.is_key_held('S'), "S is held.");

    // Releasing keys updates engine state cleanly
    engine.process_keyboard_event({'W', false, false, 7030});
    engine.process_keyboard_event({'A', false, false, 7040});
    engine.process_keyboard_event({'S', false, false, 7050});
    require(!engine.is_key_held('W'), "W released.");
    require(!engine.is_key_held('A'), "A released.");
    require(!engine.is_key_held('S'), "S released.");
}

void test_ipc_protocol_encoding_decoding_and_versioning() {
    // 1. WireHeader encoding and decoding
    WireHeader hdr;
    hdr.magic = input_protocol_magic;
    hdr.version = input_protocol_version;
    hdr.type = InputMessageType::TouchFrame;
    hdr.sequence_number = 42;
    hdr.timestamp_ms = 123456789;
    hdr.payload_size = 64;
    hdr.status_or_flags = 0;

    auto hdr_bytes = encode_wire_header(hdr);
    require(hdr_bytes.size() == input_wire_header_size, "Header bytes must equal wire header size.");

    WireHeader decoded_hdr;
    auto hdr_err = decode_wire_header(hdr_bytes, decoded_hdr);
    require(hdr_err == InputProtocolError::None, "Header decode should succeed.");
    require(decoded_hdr.magic == input_protocol_magic, "Header magic should match.");
    require(decoded_hdr.version == input_protocol_version, "Header version should match.");
    require(decoded_hdr.type == InputMessageType::TouchFrame, "Header type should match.");
    require(decoded_hdr.sequence_number == 42, "Header seq should match.");
    require(decoded_hdr.timestamp_ms == 123456789, "Header ts should match.");
    require(decoded_hdr.payload_size == 64, "Header payload size should match.");

    // Version mismatch rejection
    hdr.version = 99;
    auto bad_ver_bytes = encode_wire_header(hdr);
    require(decode_wire_header(bad_ver_bytes, decoded_hdr) == InputProtocolError::UnsupportedVersion,
        "Version mismatch must return UnsupportedVersion.");

    // Magic mismatch rejection
    hdr.version = input_protocol_version;
    hdr.magic = 0xDEADBEEF;
    auto bad_magic_bytes = encode_wire_header(hdr);
    require(decode_wire_header(bad_magic_bytes, decoded_hdr) == InputProtocolError::InvalidMagic,
        "Invalid magic must return InvalidMagic.");

    // Unknown message type rejection
    hdr.magic = input_protocol_magic;
    hdr_bytes = encode_wire_header(hdr);
    hdr_bytes[6] = static_cast<std::byte>(0xFF);
    hdr_bytes[7] = static_cast<std::byte>(0x00);
    require(decode_wire_header(hdr_bytes, decoded_hdr) == InputProtocolError::UnknownMessageType,
        "Unknown message type must return UnknownMessageType.");

    // 2. Hello / HelloAck round-trip
    HelloPayload hello{input_protocol_version, 1080, 1920, "sess-1", "gen-1"};
    auto hello_pkt = encode_hello_message(1, 100, hello);
    require(hello_pkt.size() > input_wire_header_size, "Hello packet must have payload.");
    std::span<const std::byte> hello_payload(hello_pkt.data() + input_wire_header_size, hello_pkt.size() - input_wire_header_size);
    InputProtocolError err = InputProtocolError::None;
    auto dec_hello = decode_hello_message(hello_payload, err);
    require(dec_hello.has_value(), "Hello decode should succeed.");
    require(dec_hello->display_width == 1080 && dec_hello->display_height == 1920, "Display dims match.");
    require(dec_hello->session_id == "sess-1", "Session ID matches.");
    require(dec_hello->generation_id == "gen-1", "Generation ID matches.");

    HelloAckPayload hello_ack{input_protocol_version, InputProtocolError::None, 10, 0x07, "sess-1", "gen-1"};
    auto hello_ack_pkt = encode_hello_ack_message(1, 100, hello_ack);
    std::span<const std::byte> ack_payload(hello_ack_pkt.data() + input_wire_header_size, hello_ack_pkt.size() - input_wire_header_size);
    auto dec_ack = decode_hello_ack_message(ack_payload, err);
    require(dec_ack.has_value(), "HelloAck decode should succeed.");
    require(dec_ack->max_contacts == 10, "Max contacts match.");
    require(dec_ack->session_id == "sess-1", "Session ID match.");

    // 3. Key and Cancel round-trips
    KeyEventPayload key_p{66 /*KEYCODE_ENTER*/, true};
    auto key_pkt = encode_key_event_message(2, 101, key_p);
    std::span<const std::byte> key_span(key_pkt.data() + input_wire_header_size, key_pkt.size() - input_wire_header_size);
    auto dec_key = decode_key_event_message(key_span, err);
    require(dec_key.has_value() && dec_key->android_keycode == 66 && dec_key->is_down, "Key event round-trip.");

    CancelAllPayload cancel_p{2 /*LostFocus*/};
    auto cancel_pkt = encode_cancel_all_message(3, 102, cancel_p);
    std::span<const std::byte> cancel_span(cancel_pkt.data() + input_wire_header_size, cancel_pkt.size() - input_wire_header_size);
    auto dec_cancel = decode_cancel_all_message(cancel_span, err);
    require(dec_cancel.has_value() && dec_cancel->reason_code == 2, "CancelAll round-trip.");
}

void test_ipc_protocol_malformed_truncated_oversized_messages() {
    // Truncated header
    std::vector<std::byte> short_hdr(20, std::byte{0});
    WireHeader wh;
    require(decode_wire_header(short_hdr, wh) == InputProtocolError::TruncatedHeader,
        "Short buffer must return TruncatedHeader.");

    // Oversized payload in header
    wh.magic = input_protocol_magic;
    wh.version = input_protocol_version;
    wh.type = InputMessageType::TouchFrame;
    wh.payload_size = static_cast<uint32_t>(input_maximum_payload_size + 1);
    auto oversized_bytes = encode_wire_header(wh);
    require(decode_wire_header(oversized_bytes, wh) == InputProtocolError::OversizedPayload,
        "Header with payload > 4096 must return OversizedPayload.");

    // Truncated payload for Hello
    std::vector<std::byte> truncated_payload(4, std::byte{0});
    InputProtocolError err = InputProtocolError::None;
    auto res_hello = decode_hello_message(truncated_payload, err);
    require(!res_hello.has_value() && err == InputProtocolError::TruncatedPayload,
        "Truncated hello payload must return TruncatedPayload.");

    // Malformed string length in Hello
    std::vector<std::byte> malformed_hello;
    uint16_t v = input_protocol_version;
    uint32_t w = 1080, h = 1920;
    const auto* p_v = reinterpret_cast<const std::byte*>(&v);
    malformed_hello.insert(malformed_hello.end(), p_v, p_v + 2);
    const auto* p_w = reinterpret_cast<const std::byte*>(&w);
    malformed_hello.insert(malformed_hello.end(), p_w, p_w + 4);
    const auto* p_h = reinterpret_cast<const std::byte*>(&h);
    malformed_hello.insert(malformed_hello.end(), p_h, p_h + 4);
    uint32_t huge_str_len = 999999;
    const auto* p_len = reinterpret_cast<const std::byte*>(&huge_str_len);
    malformed_hello.insert(malformed_hello.end(), p_len, p_len + 4);
    auto res_malformed = decode_hello_message(malformed_hello, err);
    require(!res_malformed.has_value() && err == InputProtocolError::MalformedPayload,
        "Malformed string length must return MalformedPayload.");

    // Truncated TouchFrame payload
    std::vector<std::byte> touch_payload;
    uint32_t claimed_contacts = 5;
    const auto* p_c = reinterpret_cast<const std::byte*>(&claimed_contacts);
    touch_payload.insert(touch_payload.end(), p_c, p_c + 4);
    // Provide only 1 contact instead of 5
    WireTouchContact wc{};
    const auto* p_wc = reinterpret_cast<const std::byte*>(&wc);
    touch_payload.insert(touch_payload.end(), p_wc, p_wc + sizeof(WireTouchContact));
    auto res_touch = decode_touch_frame_message(touch_payload, err);
    require(!res_touch.has_value() && err == InputProtocolError::MalformedPayload,
        "Touch frame with mismatched contact bytes must return MalformedPayload.");
}

void test_ipc_protocol_invalid_contact_transitions_and_coordinates() {
    InputProtocolError err = InputProtocolError::None;

    // Contact ID exceeding maximum limit (10)
    TouchFramePayload tf;
    WireTouchContact c1{};
    c1.contact_id = 15; // invalid! max is 9 (limit 10)
    c1.transition = 0; // Down
    c1.norm_u = 0.5f;
    c1.norm_v = 0.5f;
    c1.guest_x = 540;
    c1.guest_y = 960;
    tf.contacts.push_back(c1);

    auto pkt = encode_touch_frame_message(1, 100, tf);
    std::span<const std::byte> span(pkt.data() + input_wire_header_size, pkt.size() - input_wire_header_size);
    auto res = decode_touch_frame_message(span, err);
    require(!res.has_value() && err == InputProtocolError::InvalidContactId,
        "Contact ID >= 10 must return InvalidContactId.");

    // Invalid transition (> 3)
    c1.contact_id = 0;
    c1.transition = 9; // invalid
    tf.contacts[0] = c1;
    pkt = encode_touch_frame_message(2, 101, tf);
    span = std::span<const std::byte>(pkt.data() + input_wire_header_size, pkt.size() - input_wire_header_size);
    res = decode_touch_frame_message(span, err);
    require(!res.has_value() && err == InputProtocolError::InvalidTransition,
        "Transition > 3 must return InvalidTransition.");

    // Out-of-bounds coordinates (norm_u = 2.5f)
    c1.transition = 1; // Move
    c1.norm_u = 2.5f; // invalid out of bounds
    tf.contacts[0] = c1;
    pkt = encode_touch_frame_message(3, 102, tf);
    span = std::span<const std::byte>(pkt.data() + input_wire_header_size, pkt.size() - input_wire_header_size);
    res = decode_touch_frame_message(span, err);
    require(!res.has_value() && err == InputProtocolError::InvalidCoordinates,
        "norm_u > 1.01 must return InvalidCoordinates.");

    // NaN coordinates
    c1.norm_u = std::numeric_limits<float>::quiet_NaN();
    tf.contacts[0] = c1;
    pkt = encode_touch_frame_message(4, 103, tf);
    span = std::span<const std::byte>(pkt.data() + input_wire_header_size, pkt.size() - input_wire_header_size);
    res = decode_touch_frame_message(span, err);
    require(!res.has_value() && err == InputProtocolError::InvalidCoordinates,
        "NaN coordinates must return InvalidCoordinates.");

    // Valid contact passes
    c1.norm_u = 0.5f;
    c1.norm_v = 0.5f;
    tf.contacts[0] = c1;
    pkt = encode_touch_frame_message(5, 104, tf);
    span = std::span<const std::byte>(pkt.data() + input_wire_header_size, pkt.size() - input_wire_header_size);
    res = decode_touch_frame_message(span, err);
    require(res.has_value() && err == InputProtocolError::None,
        "Valid contact coordinates must decode successfully.");
}

void test_guest_bridge_capability_negotiation() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    FakeGuestInputEndpoint endpoint(transport, "session-1", "gen-1", 1080, 1920);

    GuestInputBridgeClient::Config cfg;
    cfg.session_id = "session-1";
    cfg.generation_id = "gen-1";
    cfg.display_width = 1080;
    cfg.display_height = 1920;
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, cfg);

    require(bridge->capability() == MultiTouchBackendCapability::NotConfigured,
        "Initial capability must be NotConfigured.");
    require(!bridge->is_connected(), "Initial state must be not connected.");

    // Start bridge -> sends Hello
    require(bridge->start(), "start() should succeed and send Hello.");
    require(bridge->connection_state() == GuestInputBridgeClient::ConnectionState::Negotiating,
        "Bridge must transition to Negotiating.");

    // Guest endpoint processes Hello -> sends HelloAck with max_contacts = 10
    auto processed = endpoint.poll_and_process();
    require(processed > 0, "Guest endpoint must process Hello.");

    // Bridge polls acks -> receives HelloAck, transitions to Connected
    bridge->poll_incoming_acks();
    require(bridge->is_connected(), "Bridge must be connected after HelloAck.");
    require(bridge->capability() == MultiTouchBackendCapability::Supported,
        "Capability must be Supported for max_contacts >= 2.");
    require(bridge->negotiated_max_contacts() == 10, "Negotiated contacts should be 10.");

    // Scenario 2: Guest only supports 1 contact -> capability must be Unsupported
    bridge->stop();
    require(!bridge->is_connected(), "Bridge should be disconnected after stop().");
    endpoint.reset();
    endpoint.set_simulated_max_contacts(1);

    require(bridge->start(), "start() should initiate negotiation.");
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();
    require(bridge->is_connected(), "Bridge connected.");
    require(bridge->capability() == MultiTouchBackendCapability::Unsupported,
        "Capability must be Unsupported if guest max contacts is 1.");
}

void test_guest_bridge_multi_touch_stream_and_acks() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    FakeGuestInputEndpoint endpoint(transport, "session-test", "gen-A", 1080, 1920);

    GuestInputBridgeClient::Config cfg;
    cfg.session_id = "session-test";
    cfg.generation_id = "gen-A";
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, cfg);

    bridge->start();
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();
    require(bridge->is_connected(), "Bridge connected.");

    // Send touch frame with 2 contacts: contact 0 and contact 1 Down
    TouchContactState c0;
    c0.contact_id = 0;
    c0.is_active = true;
    c0.start_time_ms = 1000;
    c0.last_move_time_ms = 1000;
    c0.norm_pos = {0.2f, 0.3f};
    c0.guest_x = 216;
    c0.guest_y = 576;

    TouchContactState c1;
    c1.contact_id = 1;
    c1.is_active = true;
    c1.start_time_ms = 1000;
    c1.last_move_time_ms = 1000;
    c1.norm_pos = {0.7f, 0.8f};
    c1.guest_x = 756;
    c1.guest_y = 1536;

    bool sent = bridge->send_touch_frame({c0, c1});
    require(sent, "send_touch_frame should return true when transport accepts.");
    require(bridge->total_sent_count() == 1, "Sent count should be 1.");
    require(bridge->total_acked_count() == 0, "Acked count should be 0 before guest processes.");

    // Guest endpoint processes frame
    std::size_t processed = endpoint.poll_and_process();
    require(processed == 1, "Guest should process 1 frame.");
    require(endpoint.active_contact_count() == 2, "Guest should track 2 active contacts.");
    require(endpoint.is_contact_active(0) && endpoint.is_contact_active(1), "Both contacts active.");

    // Bridge polls acks
    bridge->poll_incoming_acks();
    require(bridge->total_acked_count() == 1, "Acked count should be 1.");
    require(bridge->last_acked_sequence() > 0, "Acked sequence should match.");

    // Send frame: c0 Move, c1 Up
    c0.last_move_time_ms = 1016;
    c0.norm_pos = {0.25f, 0.35f};
    c1.is_active = false; // Up

    sent = bridge->send_touch_frame({c0, c1});
    require(sent, "Move/Up frame sent.");
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();

    require(endpoint.active_contact_count() == 1, "After Up, only 1 contact active.");
    require(endpoint.is_contact_active(0), "Contact 0 active.");
    require(!endpoint.is_contact_active(1), "Contact 1 inactive.");
    require(bridge->total_acked_count() == 2, "Acked count should be 2.");

    // Key event test
    bool key_sent = bridge->send_key_event(66 /*KEYCODE_ENTER*/, true);
    require(key_sent, "Key event should be sent.");
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();
    require(endpoint.total_keys_received() == 1, "Guest received 1 key event.");
    require(bridge->total_acked_count() == 3, "Total acks should be 3.");
}

void test_guest_bridge_stale_generation_and_session_switching() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    // Guest expects gen-2, but client is configured with gen-1
    FakeGuestInputEndpoint endpoint(transport, "sess-1", "gen-2", 1080, 1920);

    GuestInputBridgeClient::Config cfg;
    cfg.session_id = "sess-1";
    cfg.generation_id = "gen-1";
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, cfg);

    bridge->start();
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();

    require(!bridge->is_connected(), "Bridge must reject connection with mismatched generation.");
    require(bridge->connection_state() == GuestInputBridgeClient::ConnectionState::Error,
        "Bridge should be in Error state.");
    require(bridge->last_error() == InputProtocolError::StaleGeneration,
        "Error must be StaleGeneration.");
    require(bridge->capability() == MultiTouchBackendCapability::Unsupported,
        "Capability must be Unsupported.");

    // Reset generation to match guest
    bridge->reset_generation("gen-2");
    require(bridge->connection_state() == GuestInputBridgeClient::ConnectionState::Disconnected,
        "Resetting generation should transition to Disconnected.");

    require(bridge->start(), "start() with correct generation.");
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();
    require(bridge->is_connected(), "Bridge should connect after generation fix.");
}

void test_guest_bridge_disconnect_and_active_contact_cancellation() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    FakeGuestInputEndpoint endpoint(transport, "sess-A", "gen-A", 1080, 1920);

    GuestInputBridgeClient::Config cfg;
    cfg.session_id = "sess-A";
    cfg.generation_id = "gen-A";
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, cfg);

    bridge->start();
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();
    require(bridge->is_connected(), "Connected.");

    // Add active contact
    TouchContactState c0;
    c0.contact_id = 0;
    c0.is_active = true;
    c0.start_time_ms = 500;
    c0.last_move_time_ms = 500;
    c0.norm_pos = {0.5f, 0.5f};
    bridge->send_touch_frame({c0});
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();
    require(endpoint.active_contact_count() == 1, "1 active contact.");

    // Call cancel_all_contacts
    require(bridge->cancel_all_contacts(1), "cancel_all_contacts should succeed.");
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();
    require(endpoint.active_contact_count() == 0, "Contacts should be cleared after CancelAll.");
    require(endpoint.total_cancels_received() == 1, "Total cancels received should be 1.");

    // Sever connection
    transport->set_fault_sever_connection();
    require(!bridge->send_touch_frame({c0}), "Send on severed connection must fail.");
    require(bridge->total_dropped_count() > 0, "Dropped count must increment.");
    require(bridge->last_error() == InputProtocolError::ConnectionClosed,
        "Last error must be ConnectionClosed.");
}

void test_guest_bridge_transport_failure_and_queue_overflow() {
    auto transport = std::make_shared<LoopbackInputTransport>(256 /*very small buffer*/);
    FakeGuestInputEndpoint endpoint(transport, "sess-O", "gen-O", 1080, 1920);

    GuestInputBridgeClient::Config cfg;
    cfg.session_id = "sess-O";
    cfg.generation_id = "gen-O";
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, cfg);

    bridge->start();
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();
    require(bridge->is_connected(), "Connected.");

    // Drop sends fault injection
    transport->set_fault_drop_sends(true);
    TouchContactState c0;
    c0.contact_id = 0;
    c0.is_active = true;
    c0.start_time_ms = 100;
    c0.last_move_time_ms = 100;
    c0.norm_pos = {0.1f, 0.1f};
    require(!bridge->send_touch_frame({c0}), "Send must fail when drop fault is enabled.");
    require(bridge->last_error() == InputProtocolError::QueueOverflow,
        "Error should be QueueOverflow.");
    require(bridge->total_dropped_count() == 1, "Dropped count should increment.");

    transport->set_fault_drop_sends(false);
    // Exceed negotiated contact count (e.g. 15 contacts when max is 10)
    std::vector<TouchContactState> many_contacts;
    for (uint32_t i = 0; i < 15; ++i) {
        TouchContactState c;
        c.contact_id = i;
        c.is_active = true;
        c.norm_pos = {0.1f, 0.1f};
        many_contacts.push_back(c);
    }
    require(!bridge->send_touch_frame(many_contacts), "Sending > max_contacts must fail.");
    require(bridge->last_error() == InputProtocolError::ExceededContactLimit,
        "Error must be ExceededContactLimit.");
}

void test_guest_bridge_mapping_engine_integration() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    FakeGuestInputEndpoint endpoint(transport, "sess-eng", "gen-eng", 1080, 1920);

    GuestInputBridgeClient::Config cfg;
    cfg.session_id = "sess-eng";
    cfg.generation_id = "gen-eng";
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, cfg);

    InputMappingEngine engine;
    engine.set_guest_bridge(bridge);

    // Initial state: not started -> NotConfigured
    require(engine.multi_touch_capability() == MultiTouchBackendCapability::NotConfigured,
        "Engine capability should be NotConfigured before bridge start.");

    // Start bridge & negotiate
    bridge->start();
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();

    // Now bridge is connected -> capability is Supported!
    require(bridge->is_connected(), "Bridge connected.");
    require(engine.multi_touch_capability() == MultiTouchBackendCapability::Supported,
        "Engine capability should dynamically reflect Supported.");

    // Stop bridge -> capability reflects NotConfigured
    bridge->stop();
    require(engine.multi_touch_capability() == MultiTouchBackendCapability::NotConfigured,
        "Engine capability returns to NotConfigured when bridge stops.");
}

void test_guest_bridge_shutdown_with_pending_work() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    FakeGuestInputEndpoint endpoint(transport, "sess-S", "gen-S", 1080, 1920);

    GuestInputBridgeClient::Config cfg;
    cfg.session_id = "sess-S";
    cfg.generation_id = "gen-S";
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, cfg);

    bridge->start();
    endpoint.poll_and_process();
    bridge->poll_incoming_acks();

    // Send multiple touch frames into transport without guest polling
    for (int i = 0; i < 5; ++i) {
        TouchContactState c;
        c.contact_id = 0;
        c.is_active = true;
        c.start_time_ms = 1000 + i * 16;
        c.last_move_time_ms = 1000 + i * 16;
        c.norm_pos = {0.1f * i, 0.1f * i};
        bridge->send_touch_frame({c});
    }

    require(bridge->total_sent_count() == 5, "5 frames sent.");
    require(bridge->total_acked_count() == 0, "No acks yet.");

    // Abrupt shutdown with pending work
    bridge->stop();
    require(!bridge->is_connected(), "Bridge must be disconnected after stop.");
    require(bridge->connection_state() == GuestInputBridgeClient::ConnectionState::Disconnected,
        "State must be Disconnected.");
}

}

int main() {
    try {
        test_coordinate_mapping();
        test_keycode_translation_and_filtering();
        test_input_router_gating_and_queue();
        test_viewport_window_lifecycle_and_focus();
        test_adb_serial_validation();
        test_text_sanitization();
        test_adb_command_argument_formatting();
        test_adb_async_worker_dispatch();
        test_adb_dispatcher_queue_overflow();
        test_adb_dispatcher_executor_failure();
        test_end_to_end_router_with_adb_dispatcher();
        test_viewport_renderer_pixel_format_translation();
        test_viewport_renderer_initialization_and_fallback();
        test_viewport_renderer_frame_validation_and_rejection();
        test_viewport_renderer_resize_handling();
        test_viewport_renderer_metrics_and_telemetry();
        test_player_viewport_renderer_injection_and_fallback_display();
        test_session_target_binding_and_rejection();
        test_session_input_router_ambiguity_and_missing_identity();
        test_session_controller_lifecycle_and_session_switching();
        test_session_controller_ambiguous_devices_rejection();
        test_session_controller_failed_startup_recovery();
        test_session_shutdown_with_active_workers_and_queue();
        test_player_viewport_session_states_and_fallback_rendering();
        test_profile_parsing_validation_and_errors();
        test_default_profile_bindings();
        test_key_transitions_and_repeat_suppression();
        test_pointer_aim_and_mouse_buttons();
        test_profile_switching_held_state_cleanup();
        test_focus_loss_and_shutdown_cleanup();
        test_session_generation_tracking();
        test_mapping_rejection_guest_unready_and_ambiguous();
        test_touch_contact_model_coordinate_conversions();
        test_multi_contact_transitions_and_limits();
        test_multi_touch_capability_reporting();
        test_input_queue_overflow_no_stuck_state();
        test_ipc_protocol_encoding_decoding_and_versioning();
        test_ipc_protocol_malformed_truncated_oversized_messages();
        test_ipc_protocol_invalid_contact_transitions_and_coordinates();
        test_guest_bridge_capability_negotiation();
        test_guest_bridge_multi_touch_stream_and_acks();
        test_guest_bridge_stale_generation_and_session_switching();
        test_guest_bridge_disconnect_and_active_contact_cancellation();
        test_guest_bridge_transport_failure_and_queue_overflow();
        test_guest_bridge_mapping_engine_integration();
        test_guest_bridge_shutdown_with_pending_work();
        std::cout << "All studio player viewport, input forwarding, session integration, input mapping profiles, multi-touch architecture, guest input bridge IPC, and D3D11 presentation tests passed successfully.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
