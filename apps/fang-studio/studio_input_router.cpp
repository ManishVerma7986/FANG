#include "studio_input_router.hpp"

#include <algorithm>

namespace fang::studio {

RECT calculate_display_viewport_rect(RECT client_rect, DisplayDimensions display) {
    const int available_width = client_rect.right - client_rect.left;
    const int available_height = client_rect.bottom - client_rect.top;
    if (available_width <= 0 || available_height <= 0 || display.width <= 0 || display.height <= 0) {
        return client_rect;
    }

    const double display_aspect = static_cast<double>(display.width) / static_cast<double>(display.height);
    const double client_aspect = static_cast<double>(available_width) / static_cast<double>(available_height);

    int target_width = 0;
    int target_height = 0;

    if (client_aspect > display_aspect) {
        target_height = available_height;
        target_width = static_cast<int>(available_height * display_aspect);
    } else {
        target_width = available_width;
        target_height = static_cast<int>(available_width / display_aspect);
    }

    const int offset_x = client_rect.left + (available_width - target_width) / 2;
    const int offset_y = client_rect.top + (available_height - target_height) / 2;

    return RECT{offset_x, offset_y, offset_x + target_width, offset_y + target_height};
}

std::optional<POINT> map_client_to_display(POINT client_pt, RECT client_rect, DisplayDimensions display) {
    const RECT display_rect = calculate_display_viewport_rect(client_rect, display);
    const int display_box_width = display_rect.right - display_rect.left;
    const int display_box_height = display_rect.bottom - display_rect.top;

    if (display_box_width <= 0 || display_box_height <= 0) {
        return std::nullopt;
    }

    if (client_pt.x < display_rect.left || client_pt.x > display_rect.right ||
        client_pt.y < display_rect.top || client_pt.y > display_rect.bottom) {
        return std::nullopt;
    }

    const double norm_x = static_cast<double>(client_pt.x - display_rect.left) / display_box_width;
    const double norm_y = static_cast<double>(client_pt.y - display_rect.top) / display_box_height;

    const int guest_x = std::clamp(static_cast<int>(norm_x * display.width), 0, display.width - 1);
    const int guest_y = std::clamp(static_cast<int>(norm_y * display.height), 0, display.height - 1);

    return POINT{guest_x, guest_y};
}

std::optional<int> map_virtual_key_to_android(WPARAM vkey) noexcept {
    switch (vkey) {
    case VK_BACK: return 4;        // KEYCODE_BACK
    case VK_RETURN: return 66;     // KEYCODE_ENTER
    case VK_SPACE: return 62;      // KEYCODE_SPACE
    case VK_HOME: return 3;        // KEYCODE_HOME
    case VK_LEFT: return 21;       // KEYCODE_DPAD_LEFT
    case VK_UP: return 19;         // KEYCODE_DPAD_UP
    case VK_RIGHT: return 22;      // KEYCODE_DPAD_RIGHT
    case VK_DOWN: return 20;       // KEYCODE_DPAD_DOWN
    case VK_DELETE: return 112;    // KEYCODE_FORWARD_DEL
    default:
        break;
    }

    if (vkey >= 0x30 && vkey <= 0x39) {
        // '0' - '9' -> KEYCODE_0 (7) - KEYCODE_9 (16)
        return 7 + static_cast<int>(vkey - 0x30);
    }
    if (vkey >= 0x41 && vkey <= 0x5A) {
        // 'A' - 'Z' -> KEYCODE_A (29) - KEYCODE_Z (54)
        return 29 + static_cast<int>(vkey - 0x41);
    }

    return std::nullopt;
}

bool is_reserved_host_key(WPARAM vkey) noexcept {
    if (vkey == VK_ESCAPE || vkey == VK_TAB || vkey == VK_MENU || vkey == VK_CONTROL ||
        vkey == VK_SHIFT || vkey == VK_LWIN || vkey == VK_RWIN) {
        return true;
    }
    if (vkey >= VK_F1 && vkey <= VK_F12) {
        return true;
    }
    return false;
}

bool MockInputDispatcher::dispatch(const std::string& /*adb_serial*/, const InputEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
    return true;
}

std::vector<InputEvent> MockInputDispatcher::events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

std::size_t MockInputDispatcher::event_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
}

void MockInputDispatcher::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
}

StudioInputRouter::StudioInputRouter(std::shared_ptr<IInputDispatcher> dispatcher,
    std::size_t max_queue_depth)
    : dispatcher_(std::move(dispatcher)), max_queue_depth_(max_queue_depth) {}

void StudioInputRouter::set_dispatcher(std::shared_ptr<IInputDispatcher> dispatcher) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_ = std::move(dispatcher);
}

void StudioInputRouter::set_adb_serial(std::string serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    adb_serial_ = std::move(serial);
}

void StudioInputRouter::set_guest_ready(bool ready) {
    std::lock_guard<std::mutex> lock(mutex_);
    guest_ready_ = ready;
    if (!ready) {
        queue_.clear();
    }
}

bool StudioInputRouter::is_guest_ready() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return guest_ready_;
}

InputRoutingResult StudioInputRouter::route_pointer_down(POINT client_pt, RECT client_rect,
    DisplayDimensions display) {
    const auto mapped = map_client_to_display(client_pt, client_rect, display);
    if (!mapped.has_value()) {
        return InputRoutingResult::IgnoredOutOfRange;
    }

    InputEvent event;
    event.type = InputEventType::TouchDown;
    event.x = mapped->x;
    event.y = mapped->y;
    return enqueue_or_dispatch(event);
}

InputRoutingResult StudioInputRouter::route_pointer_up(POINT client_pt, RECT client_rect,
    DisplayDimensions display) {
    const auto mapped = map_client_to_display(client_pt, client_rect, display);
    if (!mapped.has_value()) {
        return InputRoutingResult::IgnoredOutOfRange;
    }

    InputEvent event;
    event.type = InputEventType::Tap;
    event.x = mapped->x;
    event.y = mapped->y;
    return enqueue_or_dispatch(event);
}

InputRoutingResult StudioInputRouter::route_pointer_drag(POINT start_pt, POINT end_pt,
    RECT client_rect, int duration_ms, DisplayDimensions display) {
    const auto mapped_start = map_client_to_display(start_pt, client_rect, display);
    const auto mapped_end = map_client_to_display(end_pt, client_rect, display);
    if (!mapped_start.has_value() || !mapped_end.has_value()) {
        return InputRoutingResult::IgnoredOutOfRange;
    }

    InputEvent event;
    event.type = InputEventType::Swipe;
    event.x = mapped_start->x;
    event.y = mapped_start->y;
    event.end_x = mapped_end->x;
    event.end_y = mapped_end->y;
    event.duration_ms = duration_ms;
    return enqueue_or_dispatch(event);
}

InputRoutingResult StudioInputRouter::route_key_down(WPARAM vkey) {
    if (is_reserved_host_key(vkey)) {
        return InputRoutingResult::IgnoredUnmappedKey;
    }

    const auto keycode = map_virtual_key_to_android(vkey);
    if (!keycode.has_value()) {
        return InputRoutingResult::IgnoredUnmappedKey;
    }

    InputEvent event;
    event.type = InputEventType::Key;
    event.keycode = *keycode;
    return enqueue_or_dispatch(event);
}

std::size_t StudioInputRouter::queued_event_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t StudioInputRouter::dropped_event_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_events_;
}

void StudioInputRouter::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dispatcher_ || !guest_ready_) {
        queue_.clear();
        return;
    }

    while (!queue_.empty()) {
        const auto event = queue_.front();
        queue_.pop_front();
        dispatcher_->dispatch(adb_serial_, event);
    }
}

InputRoutingResult StudioInputRouter::enqueue_or_dispatch(InputEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!guest_ready_) {
        ++dropped_events_;
        return InputRoutingResult::DroppedGuestNotReady;
    }

    if (queue_.size() >= max_queue_depth_) {
        ++dropped_events_;
        return InputRoutingResult::DroppedQueueFull;
    }

    if (dispatcher_) {
        dispatcher_->dispatch(adb_serial_, event);
        return InputRoutingResult::Success;
    }

    queue_.push_back(event);
    return InputRoutingResult::Success;
}

}
