#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "guest_input_bridge.hpp"
#include "input_mapping_types.hpp"
#include "input_profile.hpp"
#include "studio_input_router.hpp"
#include "touch_contact_model.hpp"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace fang::studio {

class InputMappingEngine final {
public:
    explicit InputMappingEngine(
        InputProfile profile = InputProfile::create_default_gaming(),
        std::shared_ptr<IGuestInputBridge> guest_bridge = nullptr);

    // Profile management
    const InputProfile& active_profile() const noexcept { return active_profile_; }
    std::vector<MappedAction> set_profile(const InputProfile& profile);

    // Display configuration
    void set_display_dimensions(DisplayDimensions dims) noexcept { display_dims_ = dims; }
    DisplayDimensions display_dimensions() const noexcept { return display_dims_; }

    // Mode toggles
    void set_gameplay_mode_enabled(bool enabled) noexcept;
    bool is_gameplay_mode_enabled() const noexcept { return gameplay_mode_enabled_; }

    void set_aim_mode_enabled(bool enabled) noexcept;
    bool is_aim_mode_enabled() const noexcept { return aim_mode_enabled_; }

    void set_viewport_focused(bool focused) noexcept;
    bool is_viewport_focused() const noexcept { return viewport_focused_; }

    // Event processors
    MappingResult process_keyboard_event(const HostKeyboardEvent& event);
    MappingResult process_pointer_button(const HostPointerButtonEvent& event, RECT client_rect);
    MappingResult process_pointer_move(const HostPointerMoveEvent& event, RECT client_rect);
    MappingResult process_pointer_wheel(const HostPointerWheelEvent& event);

    // Lifecycle releases
    std::vector<MappedAction> process_focus_loss();
    std::vector<MappedAction> reset_all_state();

    // Query active state
    bool is_key_held(uint32_t host_key) const noexcept;
    bool is_button_held(HostMouseButton button) const noexcept;
    uint32_t active_contact_count() const noexcept { return contact_model_.active_contact_count(); }
    const MultiTouchContactModel& contact_model() const noexcept { return contact_model_; }

    // Multi-touch capability
    MultiTouchBackendCapability multi_touch_capability() const noexcept;
    void set_multi_touch_capability(MultiTouchBackendCapability cap) noexcept;

    // Diagnostics & Metrics
    uint64_t total_events_processed() const noexcept { return total_events_processed_; }
    uint64_t total_actions_emitted() const noexcept { return total_actions_emitted_; }
    uint64_t total_actions_dropped() const noexcept { return total_actions_dropped_; }
    MappingRejectionReason last_rejection_reason() const noexcept { return last_rejection_reason_; }
    const std::string& last_rejection_diagnostic() const noexcept { return last_rejection_diagnostic_; }

    std::wstring status_line() const;

private:
    void record_rejection(MappingRejectionReason reason, std::string diagnostic);
    std::vector<MappedAction> release_all_held_keys();
    std::vector<MappedAction> release_all_held_buttons();

    InputProfile active_profile_;
    MultiTouchContactModel contact_model_;
    std::shared_ptr<IGuestInputBridge> guest_bridge_;
    DisplayDimensions display_dims_{1080, 1920};

    bool gameplay_mode_enabled_{true};
    bool aim_mode_enabled_{false};
    bool viewport_focused_{true};

    std::unordered_set<uint32_t> active_held_keys_;
    std::unordered_set<uint8_t> active_held_buttons_;
    uint32_t active_movement_mask_{0}; // Bitmask for Up(1), Down(2), Left(4), Right(8)

    uint64_t total_events_processed_{0};
    uint64_t total_actions_emitted_{0};
    uint64_t total_actions_dropped_{0};
    MappingRejectionReason last_rejection_reason_{MappingRejectionReason::None};
    std::string last_rejection_diagnostic_;
};

} // namespace fang::studio
