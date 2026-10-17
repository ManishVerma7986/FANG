#include "input_profile.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace fang::studio {

namespace {

std::string action_type_to_string(MappedActionType type) {
    switch (type) {
    case MappedActionType::AndroidKey: return "AndroidKey";
    case MappedActionType::DirectionalMovement: return "DirectionalMovement";
    case MappedActionType::PointerAim: return "PointerAim";
    case MappedActionType::AimModeToggle: return "AimModeToggle";
    case MappedActionType::TouchContact: return "TouchContact";
    case MappedActionType::CustomAction: return "CustomAction";
    }
    return "Unknown";
}

std::optional<MappedActionType> action_type_from_string(std::string_view str) {
    if (str == "AndroidKey") return MappedActionType::AndroidKey;
    if (str == "DirectionalMovement") return MappedActionType::DirectionalMovement;
    if (str == "PointerAim") return MappedActionType::PointerAim;
    if (str == "AimModeToggle") return MappedActionType::AimModeToggle;
    if (str == "TouchContact") return MappedActionType::TouchContact;
    if (str == "CustomAction") return MappedActionType::CustomAction;
    return std::nullopt;
}

std::string direction_to_string(MovementDirection dir) {
    switch (dir) {
    case MovementDirection::Up: return "Up";
    case MovementDirection::Down: return "Down";
    case MovementDirection::Left: return "Left";
    case MovementDirection::Right: return "Right";
    }
    return "Up";
}

std::optional<MovementDirection> direction_from_string(std::string_view str) {
    if (str == "Up") return MovementDirection::Up;
    if (str == "Down") return MovementDirection::Down;
    if (str == "Left") return MovementDirection::Left;
    if (str == "Right") return MovementDirection::Right;
    return std::nullopt;
}

std::string mouse_button_to_string(HostMouseButton btn) {
    switch (btn) {
    case HostMouseButton::Left: return "Left";
    case HostMouseButton::Right: return "Right";
    case HostMouseButton::Middle: return "Middle";
    case HostMouseButton::XButton1: return "XButton1";
    case HostMouseButton::XButton2: return "XButton2";
    }
    return "Left";
}

std::optional<HostMouseButton> mouse_button_from_string(std::string_view str) {
    if (str == "Left") return HostMouseButton::Left;
    if (str == "Right") return HostMouseButton::Right;
    if (str == "Middle") return HostMouseButton::Middle;
    if (str == "XButton1") return HostMouseButton::XButton1;
    if (str == "XButton2") return HostMouseButton::XButton2;
    return std::nullopt;
}

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

// Simple JSON extraction helper
std::string trim(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

std::optional<std::string> extract_string_field(std::string_view json, std::string_view key) {
    const std::string search_key = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search_key);
    if (pos == std::string_view::npos) return std::nullopt;

    const auto colon_pos = json.find(':', pos + search_key.size());
    if (colon_pos == std::string_view::npos) return std::nullopt;

    size_t start = colon_pos + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n')) {
        ++start;
    }
    if (start >= json.size() || json[start] != '"') {
        return std::nullopt;
    }

    const auto quote_end = json.find('"', start + 1);
    if (quote_end == std::string_view::npos) return std::nullopt;

    return std::string(json.substr(start + 1, quote_end - start - 1));
}

std::optional<double> extract_number_field(std::string_view json, std::string_view key) {
    const std::string search_key = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search_key);
    if (pos == std::string_view::npos) return std::nullopt;

    const auto colon_pos = json.find(':', pos + search_key.size());
    if (colon_pos == std::string_view::npos) return std::nullopt;

    size_t start = colon_pos + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n')) {
        ++start;
    }

    size_t end = start;
    while (end < json.size() && (json[end] == '-' || json[end] == '+' || json[end] == '.' ||
                                (json[end] >= '0' && json[end] <= '9'))) {
        ++end;
    }

    if (start == end) return std::nullopt;
    try {
        return std::stod(std::string(json.substr(start, end - start)));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

InputProfile InputProfile::create_default_gaming() {
    InputProfile profile;
    profile.schema_version = current_schema_version;
    profile.id = "default_gaming";
    profile.name = "Default Gaming (WASD+Aim)";
    profile.aim_sensitivity = 1.0f;
    profile.aim_deadzone = 0.02f;

    // Movement (WASD)
    profile.key_bindings.push_back(KeyBinding{
        'W', MappedActionType::DirectionalMovement, 0, MovementDirection::Up, {}, 1, "Move Forward"});
    profile.key_bindings.push_back(KeyBinding{
        'S', MappedActionType::DirectionalMovement, 0, MovementDirection::Down, {}, 1, "Move Backward"});
    profile.key_bindings.push_back(KeyBinding{
        'A', MappedActionType::DirectionalMovement, 0, MovementDirection::Left, {}, 1, "Move Left"});
    profile.key_bindings.push_back(KeyBinding{
        'D', MappedActionType::DirectionalMovement, 0, MovementDirection::Right, {}, 1, "Move Right"});

    // Actions
    profile.key_bindings.push_back(KeyBinding{
        0x20 /*VK_SPACE*/, MappedActionType::AndroidKey, 62 /*KEYCODE_SPACE*/, {}, {}, 0, "Jump"});
    profile.key_bindings.push_back(KeyBinding{
        'C', MappedActionType::AndroidKey, 31 /*KEYCODE_C*/, {}, {}, 0, "Crouch"});
    profile.key_bindings.push_back(KeyBinding{
        'R', MappedActionType::AndroidKey, 46 /*KEYCODE_R*/, {}, {}, 0, "Reload"});
    profile.key_bindings.push_back(KeyBinding{
        'F', MappedActionType::AndroidKey, 34 /*KEYCODE_F*/, {}, {}, 0, "Interact"});
    profile.key_bindings.push_back(KeyBinding{
        0x1B /*VK_ESCAPE*/, MappedActionType::AimModeToggle, 0, {}, {}, 0, "Toggle Aim / Exit Focus"});

    // Mouse buttons
    profile.mouse_bindings.push_back(MouseButtonBinding{
        HostMouseButton::Left, MappedActionType::TouchContact, 0, {}, 0, "Primary Fire / Tap"});
    profile.mouse_bindings.push_back(MouseButtonBinding{
        HostMouseButton::Right, MappedActionType::AimModeToggle, 0, {}, 0, "Toggle Pointer Aim"});
    profile.mouse_bindings.push_back(MouseButtonBinding{
        HostMouseButton::Middle, MappedActionType::AndroidKey, 4 /*KEYCODE_BACK*/, {}, 0, "Back"});

    return profile;
}

const KeyBinding* InputProfile::find_key_binding(uint32_t host_key) const noexcept {
    for (const auto& binding : key_bindings) {
        if (binding.host_key == host_key) {
            return &binding;
        }
    }
    return nullptr;
}

const MouseButtonBinding* InputProfile::find_mouse_binding(HostMouseButton button) const noexcept {
    for (const auto& binding : mouse_bindings) {
        if (binding.button == button) {
            return &binding;
        }
    }
    return nullptr;
}

bool InputProfile::validate(const InputProfile& profile, std::string& out_error) {
    if (profile.schema_version != current_schema_version) {
        out_error = "Unsupported schema_version: " + std::to_string(profile.schema_version) +
                    "; expected version " + std::to_string(current_schema_version) + ".";
        return false;
    }

    if (profile.id.empty()) {
        out_error = "Profile ID cannot be empty.";
        return false;
    }

    if (profile.name.empty()) {
        out_error = "Profile name cannot be empty.";
        return false;
    }

    if (profile.aim_sensitivity < 0.05f || profile.aim_sensitivity > 10.0f) {
        out_error = "Aim sensitivity " + std::to_string(profile.aim_sensitivity) +
                    " out of valid range [0.05, 10.0].";
        return false;
    }

    if (profile.aim_deadzone < 0.0f || profile.aim_deadzone >= 0.5f) {
        out_error = "Aim deadzone " + std::to_string(profile.aim_deadzone) +
                    " out of valid range [0.0, 0.5).";
        return false;
    }

    // Check duplicate or conflicting key bindings
    std::unordered_set<uint32_t> seen_keys;
    for (const auto& kb : profile.key_bindings) {
        if (kb.host_key == 0) {
            out_error = "Invalid key binding with host key code 0.";
            return false;
        }
        if (static_cast<uint32_t>(kb.action_type) > static_cast<uint32_t>(MappedActionType::TouchContact)) {
            out_error = "Key binding has unknown action type: " + std::to_string(static_cast<uint32_t>(kb.action_type));
            return false;
        }
        if (static_cast<uint32_t>(kb.direction) > static_cast<uint32_t>(MovementDirection::Right)) {
            out_error = "Key binding has unknown movement direction: " + std::to_string(static_cast<uint32_t>(kb.direction));
            return false;
        }
        if (!seen_keys.insert(kb.host_key).second) {
            out_error = "Duplicate or conflicting key binding for host key code " +
                        std::to_string(kb.host_key) + " (" + kb.name + ").";
            return false;
        }
        if (kb.action_type == MappedActionType::TouchContact) {
            if (kb.touch_norm_pos.u < 0.0f || kb.touch_norm_pos.u > 1.0f ||
                kb.touch_norm_pos.v < 0.0f || kb.touch_norm_pos.v > 1.0f) {
                out_error = "Key binding touch position out of bounds [0.0, 1.0].";
                return false;
            }
        }
    }

    // Check duplicate mouse button bindings
    std::unordered_set<uint8_t> seen_buttons;
    for (const auto& mb : profile.mouse_bindings) {
        if (static_cast<uint32_t>(mb.button) > static_cast<uint32_t>(HostMouseButton::XButton2)) {
            out_error = "Mouse binding has unknown button code: " + std::to_string(static_cast<uint32_t>(mb.button));
            return false;
        }
        if (static_cast<uint32_t>(mb.action_type) > static_cast<uint32_t>(MappedActionType::TouchContact)) {
            out_error = "Mouse binding has unknown action type: " + std::to_string(static_cast<uint32_t>(mb.action_type));
            return false;
        }
        const auto btn_idx = static_cast<uint8_t>(mb.button);
        if (!seen_buttons.insert(btn_idx).second) {
            out_error = "Duplicate or conflicting mouse button binding for button " +
                        mouse_button_to_string(mb.button) + " (" + mb.name + ").";
            return false;
        }
        if (mb.action_type == MappedActionType::TouchContact) {
            if (mb.touch_norm_pos.u < 0.0f || mb.touch_norm_pos.u > 1.0f ||
                mb.touch_norm_pos.v < 0.0f || mb.touch_norm_pos.v > 1.0f) {
                out_error = "Mouse binding touch position out of bounds [0.0, 1.0].";
                return false;
            }
        }
    }

    out_error.clear();
    return true;
}

std::string InputProfile::serialize_to_json(const InputProfile& profile) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schema_version\": " << profile.schema_version << ",\n";
    ss << "  \"id\": \"" << escape_json(profile.id) << "\",\n";
    ss << "  \"name\": \"" << escape_json(profile.name) << "\",\n";
    ss << "  \"aim_sensitivity\": " << std::fixed << std::setprecision(3) << profile.aim_sensitivity << ",\n";
    ss << "  \"aim_deadzone\": " << std::fixed << std::setprecision(3) << profile.aim_deadzone << ",\n";

    ss << "  \"key_bindings\": [\n";
    for (size_t i = 0; i < profile.key_bindings.size(); ++i) {
        const auto& kb = profile.key_bindings[i];
        ss << "    {\n";
        ss << "      \"host_key\": " << kb.host_key << ",\n";
        ss << "      \"action_type\": \"" << action_type_to_string(kb.action_type) << "\",\n";
        ss << "      \"android_keycode\": " << kb.android_keycode << ",\n";
        ss << "      \"direction\": \"" << direction_to_string(kb.direction) << "\",\n";
        ss << "      \"touch_u\": " << std::fixed << std::setprecision(4) << kb.touch_norm_pos.u << ",\n";
        ss << "      \"touch_v\": " << std::fixed << std::setprecision(4) << kb.touch_norm_pos.v << ",\n";
        ss << "      \"contact_id\": " << kb.contact_id << ",\n";
        ss << "      \"name\": \"" << escape_json(kb.name) << "\"\n";
        ss << "    }" << (i + 1 < profile.key_bindings.size() ? "," : "") << "\n";
    }
    ss << "  ],\n";

    ss << "  \"mouse_bindings\": [\n";
    for (size_t i = 0; i < profile.mouse_bindings.size(); ++i) {
        const auto& mb = profile.mouse_bindings[i];
        ss << "    {\n";
        ss << "      \"button\": \"" << mouse_button_to_string(mb.button) << "\",\n";
        ss << "      \"action_type\": \"" << action_type_to_string(mb.action_type) << "\",\n";
        ss << "      \"android_keycode\": " << mb.android_keycode << ",\n";
        ss << "      \"touch_u\": " << std::fixed << std::setprecision(4) << mb.touch_norm_pos.u << ",\n";
        ss << "      \"touch_v\": " << std::fixed << std::setprecision(4) << mb.touch_norm_pos.v << ",\n";
        ss << "      \"contact_id\": " << mb.contact_id << ",\n";
        ss << "      \"name\": \"" << escape_json(mb.name) << "\"\n";
        ss << "    }" << (i + 1 < profile.mouse_bindings.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

std::optional<InputProfile> InputProfile::parse_from_json(std::string_view json, std::string& out_error) {
    if (json.empty() || json.find('{') == std::string_view::npos) {
        out_error = "Malformed JSON: Empty or missing root object.";
        return std::nullopt;
    }

    const auto version_opt = extract_number_field(json, "schema_version");
    if (!version_opt.has_value()) {
        out_error = "Malformed JSON: Missing 'schema_version' field.";
        return std::nullopt;
    }

    InputProfile profile;
    profile.schema_version = static_cast<uint32_t>(*version_opt);

    const auto id_opt = extract_string_field(json, "id");
    if (!id_opt.has_value()) {
        out_error = "Malformed JSON: Missing 'id' field.";
        return std::nullopt;
    }
    profile.id = *id_opt;

    const auto name_opt = extract_string_field(json, "name");
    if (!name_opt.has_value()) {
        out_error = "Malformed JSON: Missing 'name' field.";
        return std::nullopt;
    }
    profile.name = *name_opt;

    const auto sens_opt = extract_number_field(json, "aim_sensitivity");
    profile.aim_sensitivity = sens_opt.has_value() ? static_cast<float>(*sens_opt) : 1.0f;

    const auto deadzone_opt = extract_number_field(json, "aim_deadzone");
    profile.aim_deadzone = deadzone_opt.has_value() ? static_cast<float>(*deadzone_opt) : 0.0f;

    // Parse key_bindings array
    const auto kb_array_pos = json.find("\"key_bindings\"");
    if (kb_array_pos != std::string_view::npos) {
        const auto start_bracket = json.find('[', kb_array_pos);
        const auto end_bracket = json.find(']', start_bracket);
        if (start_bracket != std::string_view::npos && end_bracket != std::string_view::npos) {
            std::string_view array_content = json.substr(start_bracket + 1, end_bracket - start_bracket - 1);
            size_t obj_pos = 0;
            while ((obj_pos = array_content.find('{', obj_pos)) != std::string_view::npos) {
                const auto obj_end = array_content.find('}', obj_pos);
                if (obj_end == std::string_view::npos) break;
                std::string_view obj_str = array_content.substr(obj_pos, obj_end - obj_pos + 1);

                KeyBinding kb;
                const auto hk = extract_number_field(obj_str, "host_key");
                if (hk.has_value()) kb.host_key = static_cast<uint32_t>(*hk);

                const auto at_str = extract_string_field(obj_str, "action_type");
                if (at_str.has_value()) {
                    const auto parsed_at = action_type_from_string(*at_str);
                    if (parsed_at.has_value()) kb.action_type = *parsed_at;
                } else {
                    const auto at_num = extract_number_field(obj_str, "action_type");
                    if (at_num.has_value()) kb.action_type = static_cast<MappedActionType>(static_cast<int>(*at_num));
                }

                const auto ak = extract_number_field(obj_str, "android_keycode");
                if (ak.has_value()) kb.android_keycode = static_cast<int32_t>(*ak);

                const auto dir_str = extract_string_field(obj_str, "direction");
                if (dir_str.has_value()) {
                    const auto parsed_dir = direction_from_string(*dir_str);
                    if (parsed_dir.has_value()) kb.direction = *parsed_dir;
                } else {
                    const auto dir_num = extract_number_field(obj_str, "direction");
                    if (dir_num.has_value()) kb.direction = static_cast<MovementDirection>(static_cast<int>(*dir_num));
                }

                const auto tu = extract_number_field(obj_str, "touch_u");
                const auto tv = extract_number_field(obj_str, "touch_v");
                if (tu.has_value() && tv.has_value()) {
                    kb.touch_norm_pos = NormalizedPoint{static_cast<float>(*tu), static_cast<float>(*tv)};
                }

                const auto cid = extract_number_field(obj_str, "contact_id");
                if (cid.has_value()) kb.contact_id = static_cast<uint32_t>(*cid);

                const auto kbn = extract_string_field(obj_str, "name");
                if (kbn.has_value()) kb.name = *kbn;

                profile.key_bindings.push_back(std::move(kb));
                obj_pos = obj_end + 1;
            }
        }
    }

    // Parse mouse_bindings array
    const auto mb_array_pos = json.find("\"mouse_bindings\"");
    if (mb_array_pos != std::string_view::npos) {
        const auto start_bracket = json.find('[', mb_array_pos);
        const auto end_bracket = json.find(']', start_bracket);
        if (start_bracket != std::string_view::npos && end_bracket != std::string_view::npos) {
            std::string_view array_content = json.substr(start_bracket + 1, end_bracket - start_bracket - 1);
            size_t obj_pos = 0;
            while ((obj_pos = array_content.find('{', obj_pos)) != std::string_view::npos) {
                const auto obj_end = array_content.find('}', obj_pos);
                if (obj_end == std::string_view::npos) break;
                std::string_view obj_str = array_content.substr(obj_pos, obj_end - obj_pos + 1);

                MouseButtonBinding mb;
                const auto btn_str = extract_string_field(obj_str, "button");
                if (btn_str.has_value()) {
                    const auto parsed_btn = mouse_button_from_string(*btn_str);
                    if (parsed_btn.has_value()) mb.button = *parsed_btn;
                } else {
                    const auto btn_num = extract_number_field(obj_str, "button");
                    if (btn_num.has_value()) mb.button = static_cast<HostMouseButton>(static_cast<int>(*btn_num));
                }

                const auto at_str = extract_string_field(obj_str, "action_type");
                if (at_str.has_value()) {
                    const auto parsed_at = action_type_from_string(*at_str);
                    if (parsed_at.has_value()) mb.action_type = *parsed_at;
                } else {
                    const auto at_num = extract_number_field(obj_str, "action_type");
                    if (at_num.has_value()) mb.action_type = static_cast<MappedActionType>(static_cast<int>(*at_num));
                }

                const auto ak = extract_number_field(obj_str, "android_keycode");
                if (ak.has_value()) mb.android_keycode = static_cast<int32_t>(*ak);

                const auto tu = extract_number_field(obj_str, "touch_u");
                const auto tv = extract_number_field(obj_str, "touch_v");
                if (tu.has_value() && tv.has_value()) {
                    mb.touch_norm_pos = NormalizedPoint{static_cast<float>(*tu), static_cast<float>(*tv)};
                }

                const auto cid = extract_number_field(obj_str, "contact_id");
                if (cid.has_value()) mb.contact_id = static_cast<uint32_t>(*cid);

                const auto mbn = extract_string_field(obj_str, "name");
                if (mbn.has_value()) mb.name = *mbn;

                profile.mouse_bindings.push_back(std::move(mb));
                obj_pos = obj_end + 1;
            }
        }
    }

    if (!validate(profile, out_error)) {
        return std::nullopt;
    }

    return profile;
}

} // namespace fang::studio
