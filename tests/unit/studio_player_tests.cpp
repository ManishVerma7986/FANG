#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "player_viewport.hpp"
#include "studio_input_router.hpp"
#include "adb_input_dispatcher.hpp"
#include "studio_session.hpp"

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
        std::cout << "All studio player viewport, input forwarding, session integration, and D3D11 presentation tests passed successfully.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
