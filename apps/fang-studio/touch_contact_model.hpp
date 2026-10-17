#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "input_mapping_types.hpp"
#include "studio_input_router.hpp" // For DisplayDimensions

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fang::studio {

class MultiTouchContactModel final {
public:
    static constexpr uint32_t max_concurrent_contacts = 10;

    MultiTouchContactModel();

    // Lifecycle transitions for touch contacts
    MappingResult begin_contact(uint32_t contact_id, NormalizedPoint norm_pt,
                               DisplayDimensions display_dims, uint64_t timestamp_ms);
    MappingResult move_contact(uint32_t contact_id, NormalizedPoint norm_pt,
                              DisplayDimensions display_dims, uint64_t timestamp_ms);
    MappingResult end_contact(uint32_t contact_id, uint64_t timestamp_ms);

    // Cancel all currently active contacts (emits TouchTransition::Cancel for each)
    std::vector<MappedAction> cancel_all_contacts();

    // Contact queries
    bool is_contact_active(uint32_t contact_id) const noexcept;
    std::optional<TouchContactState> contact_state(uint32_t contact_id) const;
    uint32_t active_contact_count() const noexcept;

    // Coordinate mapping utilities
    static std::optional<NormalizedPoint> map_client_to_normalized(
        POINT client_pt, RECT client_rect, DisplayDimensions display, bool clamp_if_out_of_bounds = false);
    static POINT map_normalized_to_guest(NormalizedPoint norm_pt, DisplayDimensions display);
    static std::optional<POINT> map_client_to_guest(
        POINT client_pt, RECT client_rect, DisplayDimensions display, bool clamp_if_out_of_bounds = false);

    // Backend capability boundary
    MultiTouchBackendCapability backend_capability() const noexcept {
        return backend_capability_;
    }
    void set_backend_capability(MultiTouchBackendCapability cap) noexcept {
        backend_capability_ = cap;
    }
    std::string backend_capability_diagnostic() const;

private:
    std::array<TouchContactState, max_concurrent_contacts> contacts_{};
    uint32_t active_count_{0};
    MultiTouchBackendCapability backend_capability_{MultiTouchBackendCapability::NotConfigured};
};

} // namespace fang::studio
