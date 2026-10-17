#include "touch_contact_model.hpp"

#include <algorithm>
#include <cmath>

namespace fang::studio {

MultiTouchContactModel::MultiTouchContactModel() {
    for (uint32_t i = 0; i < max_concurrent_contacts; ++i) {
        contacts_[i].contact_id = i;
        contacts_[i].is_active = false;
    }
}

MappingResult MultiTouchContactModel::begin_contact(
    uint32_t contact_id, NormalizedPoint norm_pt,
    DisplayDimensions display_dims, uint64_t timestamp_ms) {
    if (contact_id >= max_concurrent_contacts) {
        return MappingResult::rejected(
            MappingRejectionReason::InvalidContactId,
            "Contact ID " + std::to_string(contact_id) + " exceeds maximum supported (" +
            std::to_string(max_concurrent_contacts) + ").");
    }

    if (display_dims.width <= 0 || display_dims.height <= 0) {
        return MappingResult::rejected(
            MappingRejectionReason::DisplayDimensionsUnknown,
            "Guest display dimensions unknown or invalid.");
    }

    if (norm_pt.u < 0.0f || norm_pt.u > 1.0f || norm_pt.v < 0.0f || norm_pt.v > 1.0f) {
        return MappingResult::rejected(
            MappingRejectionReason::CoordinatesOutOfBounds,
            "Normalized coordinates (" + std::to_string(norm_pt.u) + ", " +
            std::to_string(norm_pt.v) + ") out of bounds [0.0, 1.0].");
    }

    auto& slot = contacts_[contact_id];
    if (!slot.is_active) {
        slot.is_active = true;
        ++active_count_;
    }

    slot.norm_pos = norm_pt;
    const POINT guest_pt = map_normalized_to_guest(norm_pt, display_dims);
    slot.guest_x = guest_pt.x;
    slot.guest_y = guest_pt.y;
    slot.start_time_ms = timestamp_ms;
    slot.last_move_time_ms = timestamp_ms;

    MappedAction act;
    act.type = MappedActionType::TouchContact;
    act.is_down = true;
    act.contact_id = contact_id;
    act.touch_transition = TouchTransition::Down;
    act.touch_norm_pos = norm_pt;
    act.guest_x = guest_pt.x;
    act.guest_y = guest_pt.y;

    return MappingResult::ok(std::move(act));
}

MappingResult MultiTouchContactModel::move_contact(
    uint32_t contact_id, NormalizedPoint norm_pt,
    DisplayDimensions display_dims, uint64_t timestamp_ms) {
    if (contact_id >= max_concurrent_contacts) {
        return MappingResult::rejected(
            MappingRejectionReason::InvalidContactId,
            "Contact ID " + std::to_string(contact_id) + " exceeds maximum supported.");
    }

    auto& slot = contacts_[contact_id];
    if (!slot.is_active) {
        return MappingResult::rejected(
            MappingRejectionReason::InvalidContactId,
            "Cannot move inactive contact ID " + std::to_string(contact_id) + ".");
    }

    if (display_dims.width <= 0 || display_dims.height <= 0) {
        return MappingResult::rejected(
            MappingRejectionReason::DisplayDimensionsUnknown,
            "Guest display dimensions unknown or invalid.");
    }

    // Clamp coordinates during movement to keep gestures within display area
    const float clamped_u = std::clamp(norm_pt.u, 0.0f, 1.0f);
    const float clamped_v = std::clamp(norm_pt.v, 0.0f, 1.0f);
    const NormalizedPoint clamped_pt{clamped_u, clamped_v};

    slot.norm_pos = clamped_pt;
    const POINT guest_pt = map_normalized_to_guest(clamped_pt, display_dims);
    slot.guest_x = guest_pt.x;
    slot.guest_y = guest_pt.y;
    slot.last_move_time_ms = timestamp_ms;

    MappedAction act;
    act.type = MappedActionType::TouchContact;
    act.is_down = true;
    act.contact_id = contact_id;
    act.touch_transition = TouchTransition::Move;
    act.touch_norm_pos = clamped_pt;
    act.guest_x = guest_pt.x;
    act.guest_y = guest_pt.y;

    return MappingResult::ok(std::move(act));
}

MappingResult MultiTouchContactModel::end_contact(uint32_t contact_id, uint64_t /*timestamp_ms*/) {
    if (contact_id >= max_concurrent_contacts) {
        return MappingResult::rejected(
            MappingRejectionReason::InvalidContactId,
            "Contact ID " + std::to_string(contact_id) + " exceeds maximum supported.");
    }

    auto& slot = contacts_[contact_id];
    if (!slot.is_active) {
        return MappingResult::rejected(
            MappingRejectionReason::InvalidContactId,
            "Cannot release inactive contact ID " + std::to_string(contact_id) + ".");
    }

    slot.is_active = false;
    if (active_count_ > 0) {
        --active_count_;
    }

    MappedAction act;
    act.type = MappedActionType::TouchContact;
    act.is_down = false;
    act.contact_id = contact_id;
    act.touch_transition = TouchTransition::Up;
    act.touch_norm_pos = slot.norm_pos;
    act.guest_x = slot.guest_x;
    act.guest_y = slot.guest_y;

    return MappingResult::ok(std::move(act));
}

std::vector<MappedAction> MultiTouchContactModel::cancel_all_contacts() {
    std::vector<MappedAction> cancellations;
    for (uint32_t i = 0; i < max_concurrent_contacts; ++i) {
        auto& slot = contacts_[i];
        if (slot.is_active) {
            slot.is_active = false;

            MappedAction act;
            act.type = MappedActionType::TouchContact;
            act.is_down = false;
            act.contact_id = i;
            act.touch_transition = TouchTransition::Cancel;
            act.touch_norm_pos = slot.norm_pos;
            act.guest_x = slot.guest_x;
            act.guest_y = slot.guest_y;
            cancellations.push_back(std::move(act));
        }
    }
    active_count_ = 0;
    return cancellations;
}

bool MultiTouchContactModel::is_contact_active(uint32_t contact_id) const noexcept {
    if (contact_id >= max_concurrent_contacts) return false;
    return contacts_[contact_id].is_active;
}

std::optional<TouchContactState> MultiTouchContactModel::contact_state(uint32_t contact_id) const {
    if (contact_id >= max_concurrent_contacts) return std::nullopt;
    if (!contacts_[contact_id].is_active) return std::nullopt;
    return contacts_[contact_id];
}

uint32_t MultiTouchContactModel::active_contact_count() const noexcept {
    return active_count_;
}

std::optional<NormalizedPoint> MultiTouchContactModel::map_client_to_normalized(
    POINT client_pt, RECT client_rect, DisplayDimensions display, bool clamp_if_out_of_bounds) {
    const RECT display_rect = calculate_display_viewport_rect(client_rect, display);
    const int display_box_width = display_rect.right - display_rect.left;
    const int display_box_height = display_rect.bottom - display_rect.top;

    if (display_box_width <= 0 || display_box_height <= 0 || display.width <= 0 || display.height <= 0) {
        return std::nullopt;
    }

    if (!clamp_if_out_of_bounds) {
        if (client_pt.x < display_rect.left || client_pt.x > display_rect.right ||
            client_pt.y < display_rect.top || client_pt.y > display_rect.bottom) {
            return std::nullopt;
        }
    }

    const int clamped_x = std::clamp(static_cast<int>(client_pt.x),
                                     static_cast<int>(display_rect.left),
                                     static_cast<int>(display_rect.right));
    const int clamped_y = std::clamp(static_cast<int>(client_pt.y),
                                     static_cast<int>(display_rect.top),
                                     static_cast<int>(display_rect.bottom));

    const float u = static_cast<float>(clamped_x - display_rect.left) / static_cast<float>(display_box_width);
    const float v = static_cast<float>(clamped_y - display_rect.top) / static_cast<float>(display_box_height);

    return NormalizedPoint{std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f)};
}

POINT MultiTouchContactModel::map_normalized_to_guest(NormalizedPoint norm_pt, DisplayDimensions display) {
    if (display.width <= 0 || display.height <= 0) {
        return POINT{0, 0};
    }
    const int guest_x = std::clamp(static_cast<int>(norm_pt.u * display.width), 0, display.width - 1);
    const int guest_y = std::clamp(static_cast<int>(norm_pt.v * display.height), 0, display.height - 1);
    return POINT{guest_x, guest_y};
}

std::optional<POINT> MultiTouchContactModel::map_client_to_guest(
    POINT client_pt, RECT client_rect, DisplayDimensions display, bool clamp_if_out_of_bounds) {
    const auto norm = map_client_to_normalized(client_pt, client_rect, display, clamp_if_out_of_bounds);
    if (!norm.has_value()) {
        return std::nullopt;
    }
    return map_normalized_to_guest(*norm, display);
}

std::string MultiTouchContactModel::backend_capability_diagnostic() const {
    switch (backend_capability_) {
    case MultiTouchBackendCapability::Supported:
        return "Multi-Touch: Supported (Direct guest touch stream active)";
    case MultiTouchBackendCapability::Unsupported:
        return "Multi-Touch: Unsupported by ADB shell input; single-contact emulation active";
    case MultiTouchBackendCapability::NotConfigured:
    default:
        return "Multi-Touch: Not configured; pending dedicated guest bridge";
    }
}

} // namespace fang::studio
