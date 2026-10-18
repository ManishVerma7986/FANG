#pragma once

#include "input_ipc_protocol.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fang::guest {

using studio::WireTouchContact;
using studio::InputProtocolError;

// Guest-side input injection abstraction
class IGuestInputBackend {
public:
    virtual ~IGuestInputBackend() = default;

    virtual bool initialize(uint32_t display_width, uint32_t display_height, uint32_t max_contacts) = 0;
    virtual void shutdown() = 0;
    virtual bool is_ready() const noexcept = 0;
    virtual std::string_view backend_name() const noexcept = 0;
    virtual uint32_t supported_capabilities() const noexcept = 0; // Bitmask: Touch=1, Key=2, PointerAim=4

    virtual bool emit_touch_contact(const WireTouchContact& contact) = 0;
    virtual bool emit_key_event(int32_t android_keycode, bool is_down) = 0;
    virtual bool emit_sync() = 0;
    virtual void cancel_all_contacts_and_keys() = 0;

    virtual std::string status_diagnostic() const = 0;
};

// Deterministic recording backend for offline testing, conformance, and protocol validation
class RecordingGuestInputBackend final : public IGuestInputBackend {
public:
    enum class EventType : uint8_t {
        TouchContact,
        KeyEvent,
        Sync,
        CancelAll
    };

    struct RecordedEvent {
        EventType type{EventType::Sync};
        WireTouchContact touch{};
        int32_t keycode{0};
        bool is_down{false};
    };

    RecordingGuestInputBackend() = default;
    ~RecordingGuestInputBackend() override = default;

    bool initialize(uint32_t display_width, uint32_t display_height, uint32_t max_contacts) override;
    void shutdown() override;
    bool is_ready() const noexcept override { return ready_; }
    std::string_view backend_name() const noexcept override { return "recording"; }
    uint32_t supported_capabilities() const noexcept override { return 0x07; } // Touch | Key | PointerAim

    bool emit_touch_contact(const WireTouchContact& contact) override;
    bool emit_key_event(int32_t android_keycode, bool is_down) override;
    bool emit_sync() override;
    void cancel_all_contacts_and_keys() override;

    std::string status_diagnostic() const override;

    // Inspection interface for tests
    const std::vector<RecordedEvent>& recorded_events() const noexcept { return events_; }
    std::size_t event_count() const noexcept { return events_.size(); }
    uint32_t active_contact_count() const noexcept;
    uint32_t active_contact_mask() const noexcept;
    bool is_contact_active(uint32_t contact_id) const noexcept;
    bool is_key_held(int32_t keycode) const noexcept;
    std::size_t held_key_count() const noexcept;
    uint64_t sync_count() const noexcept { return sync_count_; }
    void clear_events();

    // Fault injection for tests
    void set_fail_on_touch(bool fail) noexcept { fail_touch_ = fail; }
    void set_fail_on_key(bool fail) noexcept { fail_key_ = fail; }
    void set_fail_on_sync(bool fail) noexcept { fail_sync_ = fail; }

private:
    bool ready_{false};
    uint32_t display_width_{1080};
    uint32_t display_height_{1920};
    uint32_t max_contacts_{10};

    std::vector<RecordedEvent> events_;
    std::vector<bool> active_contacts_;
    std::vector<int32_t> held_keys_;
    uint64_t sync_count_{0};

    bool fail_touch_{false};
    bool fail_key_{false};
    bool fail_sync_{false};
};

// Real Linux /dev/uinput multi-touch backend
// On Linux/Android, opens /dev/uinput and writes input_events.
// On non-Linux (Windows MSVC host), compiles cleanly as a reported unavailable fallback.
class LinuxUinputGuestInputBackend final : public IGuestInputBackend {
public:
    explicit LinuxUinputGuestInputBackend(std::string device_path = "/dev/uinput");
    ~LinuxUinputGuestInputBackend() override;

    bool initialize(uint32_t display_width, uint32_t display_height, uint32_t max_contacts) override;
    void shutdown() override;
    bool is_ready() const noexcept override;
    std::string_view backend_name() const noexcept override { return "linux-uinput"; }
    uint32_t supported_capabilities() const noexcept override;

    bool emit_touch_contact(const WireTouchContact& contact) override;
    bool emit_key_event(int32_t android_keycode, bool is_down) override;
    bool emit_sync() override;
    void cancel_all_contacts_and_keys() override;

    std::string status_diagnostic() const override;

private:
    std::string device_path_;
    int fd_{-1};
    bool initialized_{false};
    uint32_t display_width_{1080};
    uint32_t display_height_{1920};
    uint32_t max_contacts_{10};
    int tracking_id_counter_{1};
    std::string diagnostic_msg_;
};

// Fallback backend when no guest injection mechanism is available
class UnavailableGuestInputBackend final : public IGuestInputBackend {
public:
    explicit UnavailableGuestInputBackend(std::string reason = "No guest input injection backend configured.")
        : reason_(std::move(reason)) {}

    bool initialize(uint32_t /*display_width*/, uint32_t /*display_height*/, uint32_t /*max_contacts*/) override {
        return false;
    }
    void shutdown() override {}
    bool is_ready() const noexcept override { return false; }
    std::string_view backend_name() const noexcept override { return "unavailable"; }
    uint32_t supported_capabilities() const noexcept override { return 0; }

    bool emit_touch_contact(const WireTouchContact& /*contact*/) override { return false; }
    bool emit_key_event(int32_t /*android_keycode*/, bool /*is_down*/) override { return false; }
    bool emit_sync() override { return false; }
    void cancel_all_contacts_and_keys() override {}

    std::string status_diagnostic() const override { return reason_; }

private:
    std::string reason_;
};

} // namespace fang::guest
