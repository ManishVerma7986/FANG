#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <string_view>

namespace fang::studio {

// Explicit host keyboard event representation (independent of Win32 HWND)
struct HostKeyboardEvent {
    uint32_t key_code{0};     // Virtual key code (e.g. 'W', 'A', 'S', 'D', VK_SPACE)
    bool is_down{false};      // true = pressed, false = released
    bool is_repeat{false};    // true = OS typematic repeat
    uint64_t timestamp_ms{0}; // Monotonic millisecond timestamp
};

// Pointer buttons
enum class HostMouseButton : uint8_t {
    Left,
    Right,
    Middle,
    XButton1,
    XButton2
};

// Explicit host pointer button event
struct HostPointerButtonEvent {
    HostMouseButton button{HostMouseButton::Left};
    bool is_down{false};
    int client_x{0};
    int client_y{0};
    uint64_t timestamp_ms{0};
};

// Explicit host pointer movement event
struct HostPointerMoveEvent {
    int client_x{0};
    int client_y{0};
    int delta_x{0};           // Relative mouse movement (for pointer aim)
    int delta_y{0};
    uint64_t timestamp_ms{0};
};

// Explicit host pointer wheel event
struct HostPointerWheelEvent {
    int delta{0};             // Wheel delta (+120 / -120)
    int client_x{0};
    int client_y{0};
    uint64_t timestamp_ms{0};
};

// Mapped Action types
enum class MappedActionType : uint8_t {
    AndroidKey,             // Keycode sent to Android (with is_down)
    DirectionalMovement,    // Movement direction (Up, Down, Left, Right; with is_down)
    PointerAim,             // Aim delta (delta_x, delta_y; scaled by sensitivity)
    AimModeToggle,          // Toggles aim mode (pointer lock / mouselook)
    TouchContact,           // Touch contact event (Down, Move, Up, Cancel)
    CustomAction            // Named custom action string
};

// Directional movement enumeration
enum class MovementDirection : uint8_t {
    Up,
    Down,
    Left,
    Right
};

// Multi-touch transition
enum class TouchTransition : uint8_t {
    Down,
    Move,
    Up,
    Cancel
};

// Normalized touch coordinate (0.0 to 1.0)
struct NormalizedPoint {
    float u{0.0f};
    float v{0.0f};

    bool operator==(const NormalizedPoint& other) const noexcept {
        return u == other.u && v == other.v;
    }
    bool operator!=(const NormalizedPoint& other) const noexcept {
        return !(*this == other);
    }
};

// Mapped Action payload emitted by the mapping engine
struct MappedAction {
    MappedActionType type{MappedActionType::AndroidKey};
    bool is_down{false};                  // For keys, buttons, directional movement
    int32_t android_keycode{0};           // Android keycode (for AndroidKey)
    MovementDirection direction{MovementDirection::Up}; // For DirectionalMovement
    float aim_delta_x{0.0f};              // For PointerAim
    float aim_delta_y{0.0f};
    uint32_t contact_id{0};               // For TouchContact (stable identifier)
    TouchTransition touch_transition{TouchTransition::Down};
    NormalizedPoint touch_norm_pos{};     // Normalized [0.0, 1.0]
    int guest_x{0};                       // Mapped guest pixels
    int guest_y{0};
    std::string custom_action_name;
};

// Rejection reasons for mapping
enum class MappingRejectionReason : uint8_t {
    None,
    ViewportUnfocused,
    GameplayModeDisabled,
    SessionNotReady,
    AmbiguousOrStaleTarget,
    UnmappedInput,
    RepeatedKeyDownSuppressed,
    InvalidContactId,
    CoordinatesOutOfBounds,
    DisplayDimensionsUnknown,
    BackendCapabilityGap
};

// Result of an input mapping operation
struct MappingResult {
    bool success{false};
    std::vector<MappedAction> actions;
    MappingRejectionReason rejection_reason{MappingRejectionReason::None};
    std::string diagnostic_message;

    static MappingResult ok(std::vector<MappedAction> acts) {
        MappingResult res;
        res.success = true;
        res.actions = std::move(acts);
        return res;
    }

    static MappingResult ok(MappedAction act) {
        MappingResult res;
        res.success = true;
        res.actions.push_back(std::move(act));
        return res;
    }

    static MappingResult rejected(MappingRejectionReason reason, std::string message) {
        MappingResult res;
        res.success = false;
        res.rejection_reason = reason;
        res.diagnostic_message = std::move(message);
        return res;
    }
};

// Multi-Touch Backend Capability Boundary
enum class MultiTouchBackendCapability : uint8_t {
    NotConfigured, // No guest input daemon or socket bridge configured
    Unsupported,   // Underlying transport (e.g. ADB CLI input) cannot represent simultaneous touch
    Supported      // Fast guest kernel / socket bridge active
};

// Multi-touch contact internal state
struct TouchContactState {
    uint32_t contact_id{0};
    bool is_active{false};
    NormalizedPoint norm_pos{0.0f, 0.0f};
    int guest_x{0};
    int guest_y{0};
    uint64_t start_time_ms{0};
    uint64_t last_move_time_ms{0};
};

} // namespace fang::studio
