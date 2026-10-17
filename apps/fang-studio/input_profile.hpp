#pragma once

#include "input_mapping_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fang::studio {

struct KeyBinding {
    uint32_t host_key{0};                               // Windows virtual key code
    MappedActionType action_type{MappedActionType::AndroidKey};
    int32_t android_keycode{0};                         // E.g. KEYCODE_SPACE
    MovementDirection direction{MovementDirection::Up}; // For DirectionalMovement
    NormalizedPoint touch_norm_pos{0.0f, 0.0f};         // For TouchContact
    uint32_t contact_id{0};                             // Contact identifier
    std::string name;                                   // Friendly name
};

struct MouseButtonBinding {
    HostMouseButton button{HostMouseButton::Left};
    MappedActionType action_type{MappedActionType::TouchContact};
    int32_t android_keycode{0};
    NormalizedPoint touch_norm_pos{0.0f, 0.0f};
    uint32_t contact_id{0};
    std::string name;
};

class InputProfile final {
public:
    static constexpr uint32_t current_schema_version = 1;

    uint32_t schema_version{current_schema_version};
    std::string id{"default_gaming"};
    std::string name{"Default Gaming Profile"};
    float aim_sensitivity{1.0f};
    float aim_deadzone{0.02f};

    std::vector<KeyBinding> key_bindings;
    std::vector<MouseButtonBinding> mouse_bindings;

    // Factory for built-in default profile
    static InputProfile create_default_gaming();

    // Query helpers
    const KeyBinding* find_key_binding(uint32_t host_key) const noexcept;
    const MouseButtonBinding* find_mouse_binding(HostMouseButton button) const noexcept;

    // Validation
    static bool validate(const InputProfile& profile, std::string& out_error);

    // Serialization & Parsing
    static std::string serialize_to_json(const InputProfile& profile);
    static std::optional<InputProfile> parse_from_json(std::string_view json, std::string& out_error);
};

} // namespace fang::studio
