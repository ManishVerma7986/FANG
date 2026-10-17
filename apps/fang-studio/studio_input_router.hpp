#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fang::studio {

enum class InputEventType {
    TouchDown,
    TouchMove,
    TouchUp,
    Tap,
    Swipe,
    Key,
    Text
};

struct InputEvent {
    InputEventType type{};
    int x{};
    int y{};
    int end_x{};
    int end_y{};
    int duration_ms{};
    int keycode{};
    std::string text;
};

enum class InputRoutingResult {
    Success,
    DroppedGuestNotReady,
    DroppedQueueFull,
    DroppedNoTargetIdentity,
    DroppedAmbiguousTarget,
    IgnoredUnmappedKey,
    IgnoredOutOfRange
};

struct DisplayDimensions {
    int width{1080};
    int height{1920};
};

RECT calculate_display_viewport_rect(RECT client_rect, DisplayDimensions display);
std::optional<POINT> map_client_to_display(POINT client_pt, RECT client_rect, DisplayDimensions display);
std::optional<int> map_virtual_key_to_android(WPARAM vkey) noexcept;
bool is_reserved_host_key(WPARAM vkey) noexcept;

class IInputDispatcher {
public:
    virtual ~IInputDispatcher() = default;
    virtual bool dispatch(const std::string& adb_serial, const InputEvent& event) = 0;
};

class MockInputDispatcher final : public IInputDispatcher {
public:
    bool dispatch(const std::string& adb_serial, const InputEvent& event) override;
    std::vector<InputEvent> events() const;
    std::size_t event_count() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<InputEvent> events_;
};

class StudioInputRouter final {
public:
    explicit StudioInputRouter(std::shared_ptr<IInputDispatcher> dispatcher = nullptr,
        std::size_t max_queue_depth = 16);

    void set_dispatcher(std::shared_ptr<IInputDispatcher> dispatcher);
    void set_adb_serial(std::string serial);
    std::string adb_serial() const;
    void set_target_ambiguous(bool ambiguous) noexcept;
    bool is_target_ambiguous() const noexcept;
    void set_guest_ready(bool ready);
    bool is_guest_ready() const noexcept;

    InputRoutingResult route_pointer_down(POINT client_pt, RECT client_rect,
        DisplayDimensions display = {});
    InputRoutingResult route_pointer_up(POINT client_pt, RECT client_rect,
        DisplayDimensions display = {});
    InputRoutingResult route_pointer_drag(POINT start_pt, POINT end_pt, RECT client_rect,
        int duration_ms = 100, DisplayDimensions display = {});
    InputRoutingResult route_key_down(WPARAM vkey);

    std::size_t queued_event_count() const;
    std::size_t dropped_event_count() const;
    void clear_queue();
    void flush();

private:
    InputRoutingResult enqueue_or_dispatch(InputEvent event);

    std::shared_ptr<IInputDispatcher> dispatcher_;
    std::string adb_serial_;
    const std::size_t max_queue_depth_;
    mutable std::mutex mutex_;
    bool guest_ready_{};
    bool target_ambiguous_{};
    std::size_t dropped_events_{};
    std::deque<InputEvent> queue_;
};

}
