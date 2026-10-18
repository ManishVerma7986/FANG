#include "input_mapping_engine.hpp"

#include <cmath>
#include <sstream>
#include <stdexcept>

namespace fang::studio {

InputMappingEngine::InputMappingEngine(
    InputProfile profile,
    std::shared_ptr<IGuestInputBridge> guest_bridge)
    : active_profile_(std::move(profile)),
      guest_bridge_(std::move(guest_bridge)) {
    if (!guest_bridge_) {
        guest_bridge_ = std::make_shared<UnconfiguredAdbGuestInputBridge>();
    }
    contact_model_.set_backend_capability(guest_bridge_->capability());
}

std::vector<MappedAction> InputMappingEngine::set_profile(const InputProfile& profile) {
    std::string err;
    if (!InputProfile::validate(profile, err)) {
        throw std::invalid_argument("Invalid InputProfile: " + err);
    }

    // Cancel all currently held controls before switching profiles to prevent stuck state
    auto releases = reset_all_state();
    active_profile_ = profile;
    return releases;
}

void InputMappingEngine::set_gameplay_mode_enabled(bool enabled) noexcept {
    if (gameplay_mode_enabled_ != enabled) {
        if (!enabled) {
            reset_all_state();
        }
        gameplay_mode_enabled_ = enabled;
    }
}

void InputMappingEngine::set_aim_mode_enabled(bool enabled) noexcept {
    aim_mode_enabled_ = enabled;
}

void InputMappingEngine::set_viewport_focused(bool focused) noexcept {
    if (viewport_focused_ != focused) {
        if (!focused) {
            process_focus_loss();
        }
        viewport_focused_ = focused;
    }
}

void InputMappingEngine::set_guest_bridge(std::shared_ptr<IGuestInputBridge> bridge) noexcept {
    if (bridge) {
        guest_bridge_ = std::move(bridge);
    } else {
        guest_bridge_ = std::make_shared<UnconfiguredAdbGuestInputBridge>();
    }
    contact_model_.set_backend_capability(guest_bridge_->capability());
}

MultiTouchBackendCapability InputMappingEngine::multi_touch_capability() const noexcept {
    if (guest_bridge_) {
        return guest_bridge_->capability();
    }
    return contact_model_.backend_capability();
}

void InputMappingEngine::set_multi_touch_capability(MultiTouchBackendCapability cap) noexcept {
    contact_model_.set_backend_capability(cap);
}

void InputMappingEngine::record_rejection(MappingRejectionReason reason, std::string diagnostic) {
    last_rejection_reason_ = reason;
    last_rejection_diagnostic_ = std::move(diagnostic);
    ++total_actions_dropped_;
}

MappingResult InputMappingEngine::process_keyboard_event(const HostKeyboardEvent& event) {
    ++total_events_processed_;

    if (!viewport_focused_) {
        record_rejection(MappingRejectionReason::ViewportUnfocused,
                         "Viewport is not focused; ignoring host keyboard event.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }

    if (!gameplay_mode_enabled_) {
        record_rejection(MappingRejectionReason::GameplayModeDisabled,
                         "Gameplay input mapping mode is disabled.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }

    if (event.is_down) {
        if (active_held_keys_.find(event.key_code) != active_held_keys_.end()) {
            record_rejection(MappingRejectionReason::RepeatedKeyDownSuppressed,
                             "OS typematic key-repeat suppressed for key code " +
                             std::to_string(event.key_code) + ".");
            return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
        }
        active_held_keys_.insert(event.key_code);
    } else {
        active_held_keys_.erase(event.key_code);
    }

    const auto* binding = active_profile_.find_key_binding(event.key_code);
    if (!binding) {
        record_rejection(MappingRejectionReason::UnmappedInput,
                         "Host key code " + std::to_string(event.key_code) +
                         " has no binding in profile '" + active_profile_.id + "'.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }

    switch (binding->action_type) {
    case MappedActionType::DirectionalMovement: {
        const uint32_t mask_bit = (binding->direction == MovementDirection::Up) ? 1 :
                                  (binding->direction == MovementDirection::Down) ? 2 :
                                  (binding->direction == MovementDirection::Left) ? 4 : 8;
        if (event.is_down) {
            active_movement_mask_ |= mask_bit;
        } else {
            active_movement_mask_ &= ~mask_bit;
        }

        MappedAction act;
        act.type = MappedActionType::DirectionalMovement;
        act.direction = binding->direction;
        act.is_down = event.is_down;
        act.custom_action_name = binding->name;
        ++total_actions_emitted_;
        return MappingResult::ok(std::move(act));
    }
    case MappedActionType::AimModeToggle: {
        if (event.is_down) {
            aim_mode_enabled_ = !aim_mode_enabled_;
        }
        MappedAction act;
        act.type = MappedActionType::AimModeToggle;
        act.is_down = event.is_down;
        act.custom_action_name = binding->name;
        ++total_actions_emitted_;
        return MappingResult::ok(std::move(act));
    }
    case MappedActionType::AndroidKey: {
        MappedAction act;
        act.type = MappedActionType::AndroidKey;
        act.android_keycode = binding->android_keycode;
        act.is_down = event.is_down;
        act.custom_action_name = binding->name;
        ++total_actions_emitted_;
        return MappingResult::ok(std::move(act));
    }
    case MappedActionType::TouchContact: {
        if (event.is_down) {
            auto res = contact_model_.begin_contact(
                binding->contact_id, binding->touch_norm_pos, display_dims_, event.timestamp_ms);
            if (res.success) {
                total_actions_emitted_ += res.actions.size();
            } else {
                record_rejection(res.rejection_reason, res.diagnostic_message);
            }
            return res;
        } else {
            auto res = contact_model_.end_contact(binding->contact_id, event.timestamp_ms);
            if (res.success) {
                total_actions_emitted_ += res.actions.size();
            } else {
                record_rejection(res.rejection_reason, res.diagnostic_message);
            }
            return res;
        }
    }
    default:
        record_rejection(MappingRejectionReason::UnmappedInput,
                         "Unsupported action type for key binding.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }
}

MappingResult InputMappingEngine::process_pointer_button(
    const HostPointerButtonEvent& event, RECT client_rect) {
    ++total_events_processed_;

    if (!viewport_focused_) {
        record_rejection(MappingRejectionReason::ViewportUnfocused,
                         "Viewport is not focused; ignoring pointer button event.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }

    if (!gameplay_mode_enabled_) {
        record_rejection(MappingRejectionReason::GameplayModeDisabled,
                         "Gameplay input mapping mode is disabled.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }

    const auto btn_idx = static_cast<uint8_t>(event.button);
    if (event.is_down) {
        active_held_buttons_.insert(btn_idx);
    } else {
        active_held_buttons_.erase(btn_idx);
    }

    const auto* binding = active_profile_.find_mouse_binding(event.button);
    if (!binding) {
        record_rejection(MappingRejectionReason::UnmappedInput,
                         "Pointer button has no binding in active profile.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }

    switch (binding->action_type) {
    case MappedActionType::AimModeToggle: {
        if (event.is_down) {
            aim_mode_enabled_ = !aim_mode_enabled_;
        }
        MappedAction act;
        act.type = MappedActionType::AimModeToggle;
        act.is_down = event.is_down;
        act.custom_action_name = binding->name;
        ++total_actions_emitted_;
        return MappingResult::ok(std::move(act));
    }
    case MappedActionType::TouchContact: {
        NormalizedPoint norm_pt = binding->touch_norm_pos;
        if (norm_pt.u == 0.0f && norm_pt.v == 0.0f) {
            // Dynamic position from pointer client coordinates
            const auto mapped = MultiTouchContactModel::map_client_to_normalized(
                POINT{event.client_x, event.client_y}, client_rect, display_dims_, false);
            if (!mapped.has_value()) {
                record_rejection(MappingRejectionReason::CoordinatesOutOfBounds,
                                 "Pointer coordinate lies outside rendered display area.");
                return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
            }
            norm_pt = *mapped;
        }

        if (event.is_down) {
            auto res = contact_model_.begin_contact(
                binding->contact_id, norm_pt, display_dims_, event.timestamp_ms);
            if (res.success) {
                total_actions_emitted_ += res.actions.size();
            } else {
                record_rejection(res.rejection_reason, res.diagnostic_message);
            }
            return res;
        } else {
            auto res = contact_model_.end_contact(binding->contact_id, event.timestamp_ms);
            if (res.success) {
                total_actions_emitted_ += res.actions.size();
            } else {
                record_rejection(res.rejection_reason, res.diagnostic_message);
            }
            return res;
        }
    }
    case MappedActionType::AndroidKey: {
        MappedAction act;
        act.type = MappedActionType::AndroidKey;
        act.android_keycode = binding->android_keycode;
        act.is_down = event.is_down;
        act.custom_action_name = binding->name;
        ++total_actions_emitted_;
        return MappingResult::ok(std::move(act));
    }
    default:
        record_rejection(MappingRejectionReason::UnmappedInput,
                         "Unsupported action type for mouse binding.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }
}

MappingResult InputMappingEngine::process_pointer_move(
    const HostPointerMoveEvent& event, RECT client_rect) {
    ++total_events_processed_;

    if (!viewport_focused_) {
        record_rejection(MappingRejectionReason::ViewportUnfocused,
                         "Viewport is not focused; ignoring pointer move.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }

    if (!gameplay_mode_enabled_) {
        record_rejection(MappingRejectionReason::GameplayModeDisabled,
                         "Gameplay input mapping mode is disabled.");
        return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
    }

    if (aim_mode_enabled_) {
        const float dx = static_cast<float>(event.delta_x) * active_profile_.aim_sensitivity;
        const float dy = static_cast<float>(event.delta_y) * active_profile_.aim_sensitivity;

        float final_dx = dx;
        float final_dy = dy;
        if (std::abs(dx) < active_profile_.aim_deadzone) final_dx = 0.0f;
        if (std::abs(dy) < active_profile_.aim_deadzone) final_dy = 0.0f;

        MappedAction act;
        act.type = MappedActionType::PointerAim;
        act.is_down = true;
        act.aim_delta_x = final_dx;
        act.aim_delta_y = final_dy;
        act.custom_action_name = "Pointer Aim";
        ++total_actions_emitted_;
        return MappingResult::ok(std::move(act));
    }

    // If left mouse button is held down, update active touch contact 0
    if (is_button_held(HostMouseButton::Left) && contact_model_.is_contact_active(0)) {
        const auto mapped = MultiTouchContactModel::map_client_to_normalized(
            POINT{event.client_x, event.client_y}, client_rect, display_dims_, true /*clamp*/);
        if (mapped.has_value()) {
            auto res = contact_model_.move_contact(0, *mapped, display_dims_, event.timestamp_ms);
            if (res.success) {
                total_actions_emitted_ += res.actions.size();
            }
            return res;
        }
    }

    record_rejection(MappingRejectionReason::GameplayModeDisabled,
                     "Pointer move rejected: Aim mode is disabled and no touch contact is active.");
    return MappingResult::rejected(last_rejection_reason_, last_rejection_diagnostic_);
}

MappingResult InputMappingEngine::process_pointer_wheel(const HostPointerWheelEvent& /*event*/) {
    ++total_events_processed_;
    return MappingResult::ok(std::vector<MappedAction>{});
}

std::vector<MappedAction> InputMappingEngine::release_all_held_keys() {
    std::vector<MappedAction> releases;
    for (uint32_t key_code : active_held_keys_) {
        const auto* binding = active_profile_.find_key_binding(key_code);
        if (binding) {
            if (binding->action_type == MappedActionType::DirectionalMovement) {
                MappedAction act;
                act.type = MappedActionType::DirectionalMovement;
                act.direction = binding->direction;
                act.is_down = false;
                act.custom_action_name = binding->name;
                releases.push_back(std::move(act));
            } else if (binding->action_type == MappedActionType::AndroidKey) {
                MappedAction act;
                act.type = MappedActionType::AndroidKey;
                act.android_keycode = binding->android_keycode;
                act.is_down = false;
                act.custom_action_name = binding->name;
                releases.push_back(std::move(act));
            }
        }
    }
    active_held_keys_.clear();
    active_movement_mask_ = 0;
    return releases;
}

std::vector<MappedAction> InputMappingEngine::release_all_held_buttons() {
    std::vector<MappedAction> releases;
    for (uint8_t btn_idx : active_held_buttons_) {
        const auto btn = static_cast<HostMouseButton>(btn_idx);
        const auto* binding = active_profile_.find_mouse_binding(btn);
        if (binding && binding->action_type == MappedActionType::AndroidKey) {
            MappedAction act;
            act.type = MappedActionType::AndroidKey;
            act.android_keycode = binding->android_keycode;
            act.is_down = false;
            act.custom_action_name = binding->name;
            releases.push_back(std::move(act));
        }
    }
    active_held_buttons_.clear();
    return releases;
}

std::vector<MappedAction> InputMappingEngine::process_focus_loss() {
    viewport_focused_ = false;
    std::vector<MappedAction> cancellations;

    auto key_releases = release_all_held_keys();
    cancellations.insert(cancellations.end(),
                         std::make_move_iterator(key_releases.begin()),
                         std::make_move_iterator(key_releases.end()));

    auto button_releases = release_all_held_buttons();
    cancellations.insert(cancellations.end(),
                         std::make_move_iterator(button_releases.begin()),
                         std::make_move_iterator(button_releases.end()));

    auto touch_cancels = contact_model_.cancel_all_contacts();
    cancellations.insert(cancellations.end(),
                         std::make_move_iterator(touch_cancels.begin()),
                         std::make_move_iterator(touch_cancels.end()));

    aim_mode_enabled_ = false;
    total_actions_emitted_ += cancellations.size();
    return cancellations;
}

std::vector<MappedAction> InputMappingEngine::reset_all_state() {
    return process_focus_loss();
}

bool InputMappingEngine::is_key_held(uint32_t host_key) const noexcept {
    return active_held_keys_.find(host_key) != active_held_keys_.end();
}

bool InputMappingEngine::is_button_held(HostMouseButton button) const noexcept {
    return active_held_buttons_.find(static_cast<uint8_t>(button)) != active_held_buttons_.end();
}

std::wstring InputMappingEngine::status_line() const {
    std::wostringstream ws;
    ws << L"Profile: " << std::wstring(active_profile_.name.begin(), active_profile_.name.end())
       << L" | Gameplay: " << (gameplay_mode_enabled_ ? L"ON" : L"OFF")
       << L" | Aim Mode: " << (aim_mode_enabled_ ? L"ON" : L"OFF")
       << L" | Contacts: " << contact_model_.active_contact_count()
       << L" | " << std::wstring(contact_model_.backend_capability_diagnostic().begin(),
                                contact_model_.backend_capability_diagnostic().end());
    return ws.str();
}

} // namespace fang::studio
