#include "guest_input_backend.hpp"

#include <algorithm>
#include <sstream>

#if defined(__linux__) || defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <cstring>
#include <cerrno>
#endif

namespace fang::guest {

// ============================================================================
// RecordingGuestInputBackend
// ============================================================================

bool RecordingGuestInputBackend::initialize(uint32_t display_width, uint32_t display_height, uint32_t max_contacts) {
    display_width_ = display_width;
    display_height_ = display_height;
    max_contacts_ = std::min(max_contacts, 10u);
    active_contacts_.assign(max_contacts_, false);
    held_keys_.clear();
    events_.clear();
    sync_count_ = 0;
    ready_ = true;
    return true;
}

void RecordingGuestInputBackend::shutdown() {
    cancel_all_contacts_and_keys();
    ready_ = false;
}

bool RecordingGuestInputBackend::emit_touch_contact(const WireTouchContact& contact) {
    if (!ready_ || fail_touch_) {
        return false;
    }

    if (contact.contact_id < active_contacts_.size()) {
        if (contact.transition == 0 /*Down*/ || contact.transition == 1 /*Move*/) {
            active_contacts_[contact.contact_id] = true;
        } else if (contact.transition == 2 /*Up*/ || contact.transition == 3 /*Cancel*/) {
            active_contacts_[contact.contact_id] = false;
        }
    }

    RecordedEvent evt;
    evt.type = EventType::TouchContact;
    evt.touch = contact;
    events_.push_back(evt);
    return true;
}

bool RecordingGuestInputBackend::emit_key_event(int32_t android_keycode, bool is_down) {
    if (!ready_ || fail_key_) {
        return false;
    }

    auto it = std::find(held_keys_.begin(), held_keys_.end(), android_keycode);
    if (is_down) {
        if (it == held_keys_.end()) {
            held_keys_.push_back(android_keycode);
        }
    } else {
        if (it != held_keys_.end()) {
            held_keys_.erase(it);
        }
    }

    RecordedEvent evt;
    evt.type = EventType::KeyEvent;
    evt.keycode = android_keycode;
    evt.is_down = is_down;
    events_.push_back(evt);
    return true;
}

bool RecordingGuestInputBackend::emit_sync() {
    if (!ready_ || fail_sync_) {
        return false;
    }

    ++sync_count_;
    RecordedEvent evt;
    evt.type = EventType::Sync;
    events_.push_back(evt);
    return true;
}

void RecordingGuestInputBackend::cancel_all_contacts_and_keys() {
    std::fill(active_contacts_.begin(), active_contacts_.end(), false);
    held_keys_.clear();

    RecordedEvent evt;
    evt.type = EventType::CancelAll;
    events_.push_back(evt);
}

std::string RecordingGuestInputBackend::status_diagnostic() const {
    std::ostringstream ss;
    ss << "Recording backend (ready=" << (ready_ ? "true" : "false")
       << ", active_contacts=" << active_contact_count()
       << ", held_keys=" << held_keys_.size()
       << ", syncs=" << sync_count_ << ")";
    return ss.str();
}

uint32_t RecordingGuestInputBackend::active_contact_count() const noexcept {
    uint32_t count = 0;
    for (bool active : active_contacts_) {
        if (active) {
            ++count;
        }
    }
    return count;
}

uint32_t RecordingGuestInputBackend::active_contact_mask() const noexcept {
    uint32_t mask = 0;
    for (std::size_t i = 0; i < active_contacts_.size(); ++i) {
        if (active_contacts_[i]) {
            mask |= (1u << static_cast<uint32_t>(i));
        }
    }
    return mask;
}

bool RecordingGuestInputBackend::is_contact_active(uint32_t contact_id) const noexcept {
    if (contact_id < active_contacts_.size()) {
        return active_contacts_[contact_id];
    }
    return false;
}

bool RecordingGuestInputBackend::is_key_held(int32_t keycode) const noexcept {
    return std::find(held_keys_.begin(), held_keys_.end(), keycode) != held_keys_.end();
}

std::size_t RecordingGuestInputBackend::held_key_count() const noexcept {
    return held_keys_.size();
}

void RecordingGuestInputBackend::clear_events() {
    events_.clear();
}

// ============================================================================
// LinuxUinputGuestInputBackend
// ============================================================================

LinuxUinputGuestInputBackend::LinuxUinputGuestInputBackend(std::string device_path)
    : device_path_(std::move(device_path)) {}

LinuxUinputGuestInputBackend::~LinuxUinputGuestInputBackend() {
    shutdown();
}

#if defined(__linux__) || defined(__ANDROID__)

bool LinuxUinputGuestInputBackend::initialize(uint32_t display_width, uint32_t display_height, uint32_t max_contacts) {
    shutdown();

    display_width_ = display_width;
    display_height_ = display_height;
    max_contacts_ = std::min(max_contacts, 10u);

    fd_ = open(device_path_.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        diagnostic_msg_ = "Failed to open " + device_path_ + ": " + std::strerror(errno);
        return false;
    }

    // Enable EV_SYN, EV_KEY, EV_ABS
    if (ioctl(fd_, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd_, UI_SET_EVBIT, EV_ABS) < 0) {
        diagnostic_msg_ = "ioctl(UI_SET_EVBIT) failed: " + std::string(std::strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }

    // Enable multi-touch and key properties
    ioctl(fd_, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd_, UI_SET_KEYBIT, BTN_TOOL_FINGER);

    ioctl(fd_, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd_, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd_, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd_, UI_SET_ABSBIT, ABS_MT_POSITION_Y);

    struct uinput_user_dev uidev{};
    std::strncpy(uidev.name, "fang-virtual-touchscreen", UINPUT_MAX_NAME_SIZE - 1);
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor  = 0x18d1; // Google
    uidev.id.product = 0x0001;
    uidev.id.version = 1;

    uidev.absmin[ABS_MT_SLOT] = 0;
    uidev.absmax[ABS_MT_SLOT] = static_cast<int32_t>(max_contacts_ - 1);
    uidev.absmin[ABS_MT_TRACKING_ID] = 0;
    uidev.absmax[ABS_MT_TRACKING_ID] = 65535;
    uidev.absmin[ABS_MT_POSITION_X] = 0;
    uidev.absmax[ABS_MT_POSITION_X] = static_cast<int32_t>(display_width_ - 1);
    uidev.absmin[ABS_MT_POSITION_Y] = 0;
    uidev.absmax[ABS_MT_POSITION_Y] = static_cast<int32_t>(display_height_ - 1);

    if (write(fd_, &uidev, sizeof(uidev)) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0) {
        diagnostic_msg_ = "Failed to create uinput device: " + std::string(std::strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }

    initialized_ = true;
    diagnostic_msg_ = "Linux uinput virtual touchscreen created successfully at " + device_path_;
    return true;
}

void LinuxUinputGuestInputBackend::shutdown() {
    if (initialized_ && fd_ >= 0) {
        cancel_all_contacts_and_keys();
        ioctl(fd_, UI_DEV_DESTROY);
        close(fd_);
        fd_ = -1;
        initialized_ = false;
    }
}

bool LinuxUinputGuestInputBackend::is_ready() const noexcept {
    return initialized_ && (fd_ >= 0);
}

uint32_t LinuxUinputGuestInputBackend::supported_capabilities() const noexcept {
    return 0x07; // Touch | Key | PointerAim
}

static bool write_event(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == sizeof(ev);
}

bool LinuxUinputGuestInputBackend::emit_touch_contact(const WireTouchContact& contact) {
    if (!is_ready()) {
        return false;
    }

    write_event(fd_, EV_ABS, ABS_MT_SLOT, static_cast<int32_t>(contact.contact_id));

    if (contact.transition == 0 /*Down*/) {
        int tracking_id = tracking_id_counter_++;
        if (tracking_id_counter_ > 65000) tracking_id_counter_ = 1;
        write_event(fd_, EV_ABS, ABS_MT_TRACKING_ID, tracking_id);
        write_event(fd_, EV_ABS, ABS_MT_POSITION_X, contact.guest_x);
        write_event(fd_, EV_ABS, ABS_MT_POSITION_Y, contact.guest_y);
        write_event(fd_, EV_KEY, BTN_TOUCH, 1);
    } else if (contact.transition == 1 /*Move*/) {
        write_event(fd_, EV_ABS, ABS_MT_POSITION_X, contact.guest_x);
        write_event(fd_, EV_ABS, ABS_MT_POSITION_Y, contact.guest_y);
    } else if (contact.transition == 2 /*Up*/ || contact.transition == 3 /*Cancel*/) {
        write_event(fd_, EV_ABS, ABS_MT_TRACKING_ID, -1);
    }

    return true;
}

bool LinuxUinputGuestInputBackend::emit_key_event(int32_t android_keycode, bool is_down) {
    if (!is_ready()) {
        return false;
    }
    return write_event(fd_, EV_KEY, static_cast<uint16_t>(android_keycode), is_down ? 1 : 0);
}

bool LinuxUinputGuestInputBackend::emit_sync() {
    if (!is_ready()) {
        return false;
    }
    return write_event(fd_, EV_SYN, SYN_REPORT, 0);
}

void LinuxUinputGuestInputBackend::cancel_all_contacts_and_keys() {
    if (!is_ready()) {
        return;
    }
    for (uint32_t slot = 0; slot < max_contacts_; ++slot) {
        write_event(fd_, EV_ABS, ABS_MT_SLOT, static_cast<int32_t>(slot));
        write_event(fd_, EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    write_event(fd_, EV_KEY, BTN_TOUCH, 0);
    write_event(fd_, EV_SYN, SYN_REPORT, 0);
}

std::string LinuxUinputGuestInputBackend::status_diagnostic() const {
    return diagnostic_msg_;
}

#else // Non-Linux (Windows host MSVC)

bool LinuxUinputGuestInputBackend::initialize(uint32_t /*display_width*/, uint32_t /*display_height*/, uint32_t /*max_contacts*/) {
    diagnostic_msg_ = "Linux /dev/uinput backend is not available on non-Linux host OS; requires a guest Linux/Android kernel.";
    return false;
}

void LinuxUinputGuestInputBackend::shutdown() {
    initialized_ = false;
}

bool LinuxUinputGuestInputBackend::is_ready() const noexcept {
    return false;
}

uint32_t LinuxUinputGuestInputBackend::supported_capabilities() const noexcept {
    return 0;
}

bool LinuxUinputGuestInputBackend::emit_touch_contact(const WireTouchContact& /*contact*/) {
    return false;
}

bool LinuxUinputGuestInputBackend::emit_key_event(int32_t /*android_keycode*/, bool /*is_down*/) {
    return false;
}

bool LinuxUinputGuestInputBackend::emit_sync() {
    return false;
}

void LinuxUinputGuestInputBackend::cancel_all_contacts_and_keys() {}

std::string LinuxUinputGuestInputBackend::status_diagnostic() const {
    return "Linux /dev/uinput backend is not available on non-Linux host OS; requires a guest Linux/Android kernel.";
}

#endif

} // namespace fang::guest
