#include "guest_endpoint_forwarder.hpp"
#include "adb_input_dispatcher.hpp"

#include <sstream>

namespace fang::studio {

// ============================================================================
// LoopbackTestForwarder
// ============================================================================

LoopbackTestForwarder::LoopbackTestForwarder(uint16_t local_test_port)
    : local_test_port_(local_test_port) {}

bool LoopbackTestForwarder::setup_forwarding(
    const std::string& serial_or_avd,
    uint16_t guest_port,
    GuestEndpointMapping& out_mapping) {
    if (simulate_failure_) {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.mechanism = ForwardingMechanism::StubLoopback;
        out_mapping.diagnostic = "Simulated port forwarding failure.";
        out_mapping.is_live_forwarding_active = false;
        return false;
    }

    out_mapping.host_address = "127.0.0.1";
    out_mapping.host_port = local_test_port_;
    out_mapping.guest_port = guest_port;
    out_mapping.state = ForwardingState::Active;
    out_mapping.mechanism = ForwardingMechanism::StubLoopback;
    out_mapping.diagnostic = "Loopback test forwarder active for target " + serial_or_avd;
    out_mapping.is_live_forwarding_active = true;
    return true;
}

void LoopbackTestForwarder::teardown_forwarding(GuestEndpointMapping& mapping) {
    mapping.state = ForwardingState::NotConfigured;
    mapping.is_live_forwarding_active = false;
    mapping.diagnostic = "Loopback test forwarder torn down.";
}

bool LoopbackTestForwarder::is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept {
    return mapping.state == ForwardingState::Active && mapping.is_live_forwarding_active;
}

// ============================================================================
// AdbPortForwarder
// ============================================================================

AdbPortForwarder::AdbPortForwarder(uint16_t preferred_host_port)
    : preferred_host_port_(preferred_host_port) {}

std::string AdbPortForwarder::format_adb_forward_cmd(
    const std::string& serial, uint16_t host_port, uint16_t guest_port) {
    std::ostringstream ss;
    ss << "adb -s " << serial << " forward tcp:" << host_port << " tcp:" << guest_port;
    return ss.str();
}

bool AdbPortForwarder::setup_forwarding(
    const std::string& serial_or_avd,
    uint16_t guest_port,
    GuestEndpointMapping& out_mapping) {
    // Validate target serial syntax
    if (serial_or_avd.empty() || !AdbInputDispatcher::is_valid_adb_serial(serial_or_avd)) {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.mechanism = ForwardingMechanism::AdbPortForward;
        out_mapping.diagnostic = "Invalid target ADB serial: '" + serial_or_avd + "'";
        out_mapping.is_live_forwarding_active = false;
        return false;
    }

    // Allocate host port: if preferred is 0, use standard offset or convention
    uint16_t host_p = preferred_host_port_;
    if (host_p == 0) {
        host_p = guest_port; // default 24800
    }

    out_mapping.host_address = "127.0.0.1";
    out_mapping.host_port = host_p;
    out_mapping.guest_port = guest_port;
    out_mapping.mechanism = ForwardingMechanism::AdbPortForward;

    // Under offline / safety policy, live ADB invocation requires separate explicit user authorization
    out_mapping.state = ForwardingState::Blocked;
    out_mapping.is_live_forwarding_active = false;
    out_mapping.diagnostic = "ADB port forwarding contract formulated (" +
        format_adb_forward_cmd(serial_or_avd, host_p, guest_port) +
        "); live ADB server execution remains unauthorized.";

    return false;
}

void AdbPortForwarder::teardown_forwarding(GuestEndpointMapping& mapping) {
    mapping.state = ForwardingState::NotConfigured;
    mapping.is_live_forwarding_active = false;
    mapping.diagnostic = "ADB port forwarding released.";
}

bool AdbPortForwarder::is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept {
    return mapping.state == ForwardingState::Active && mapping.is_live_forwarding_active;
}

// ============================================================================
// BlockedGuestForwarder
// ============================================================================

BlockedGuestForwarder::BlockedGuestForwarder(std::string blocker_reason)
    : blocker_reason_(std::move(blocker_reason)) {}

bool BlockedGuestForwarder::setup_forwarding(
    const std::string& /*serial_or_avd*/,
    uint16_t guest_port,
    GuestEndpointMapping& out_mapping) {
    out_mapping.host_address = "127.0.0.1";
    out_mapping.host_port = 0;
    out_mapping.guest_port = guest_port;
    out_mapping.state = ForwardingState::Blocked;
    out_mapping.mechanism = ForwardingMechanism::Blocked;
    out_mapping.is_live_forwarding_active = false;
    out_mapping.diagnostic = blocker_reason_;
    return false;
}

void BlockedGuestForwarder::teardown_forwarding(GuestEndpointMapping& mapping) {
    mapping.state = ForwardingState::NotConfigured;
    mapping.is_live_forwarding_active = false;
}

bool BlockedGuestForwarder::is_forwarding_active(const GuestEndpointMapping& /*mapping*/) const noexcept {
    return false;
}

} // namespace fang::studio
