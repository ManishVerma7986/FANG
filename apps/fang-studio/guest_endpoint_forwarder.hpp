#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace fang::studio {

enum class ForwardingState : uint8_t {
    NotConfigured,
    Active,
    Failed,
    Blocked
};

constexpr std::string_view forwarding_state_name(ForwardingState state) noexcept {
    switch (state) {
    case ForwardingState::NotConfigured: return "NotConfigured";
    case ForwardingState::Active:        return "Active";
    case ForwardingState::Failed:        return "Failed";
    case ForwardingState::Blocked:       return "Blocked";
    default:                             return "Unknown";
    }
}

enum class ForwardingMechanism : uint8_t {
    StubLoopback,
    AdbPortForward,
    QemuHostForward,
    Blocked
};

constexpr std::string_view forwarding_mechanism_name(ForwardingMechanism mechanism) noexcept {
    switch (mechanism) {
    case ForwardingMechanism::StubLoopback:   return "StubLoopback";
    case ForwardingMechanism::AdbPortForward:  return "AdbPortForward";
    case ForwardingMechanism::QemuHostForward: return "QemuHostForward";
    case ForwardingMechanism::Blocked:         return "Blocked";
    default:                                   return "Unknown";
    }
}

struct GuestEndpointMapping {
    std::string host_address{"127.0.0.1"};
    uint16_t host_port{0};
    uint16_t guest_port{24800};
    ForwardingState state{ForwardingState::NotConfigured};
    ForwardingMechanism mechanism{ForwardingMechanism::Blocked};
    std::string diagnostic;
    bool is_live_forwarding_active{false};
};

// Abstract host-to-guest reachability and port-forwarding contract.
// Resolves how a TCP listener bound inside an Android guest (127.0.0.1:guest_port)
// is reached from the Windows host application.
class IGuestEndpointForwarder {
public:
    virtual ~IGuestEndpointForwarder() = default;

    virtual bool setup_forwarding(
        const std::string& serial_or_avd,
        uint16_t guest_port,
        GuestEndpointMapping& out_mapping) = 0;

    virtual void teardown_forwarding(GuestEndpointMapping& mapping) = 0;
    virtual bool is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept = 0;
    virtual std::string_view forwarder_name() const noexcept = 0;
};

// Deterministic in-memory / loopback forwarder for offline unit testing and test daemons.
class LoopbackTestForwarder final : public IGuestEndpointForwarder {
public:
    explicit LoopbackTestForwarder(uint16_t local_test_port = 24800);

    bool setup_forwarding(
        const std::string& serial_or_avd,
        uint16_t guest_port,
        GuestEndpointMapping& out_mapping) override;

    void teardown_forwarding(GuestEndpointMapping& mapping) override;
    bool is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept override;
    std::string_view forwarder_name() const noexcept override { return "loopback-test"; }

    void set_local_test_port(uint16_t port) noexcept { local_test_port_ = port; }
    void set_simulate_failure(bool fail) noexcept { simulate_failure_ = fail; }

private:
    uint16_t local_test_port_{24800};
    bool simulate_failure_{false};
};

// Concrete ADB port forwarder contract for reference runtime emulator sessions.
// Evaluates target serial and prepares forwarding mapping without unauthorized live ADB execution.
class AdbPortForwarder final : public IGuestEndpointForwarder {
public:
    explicit AdbPortForwarder(uint16_t preferred_host_port = 0);

    bool setup_forwarding(
        const std::string& serial_or_avd,
        uint16_t guest_port,
        GuestEndpointMapping& out_mapping) override;

    void teardown_forwarding(GuestEndpointMapping& mapping) override;
    bool is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept override;
    std::string_view forwarder_name() const noexcept override { return "adb-forward"; }

    // Generates the deterministic ADB CLI command string for reference
    static std::string format_adb_forward_cmd(const std::string& serial, uint16_t host_port, uint16_t guest_port);

private:
    uint16_t preferred_host_port_{0};
};

// Concrete forwarder when guest reachability is blocked pending runtime/network provisioning.
class BlockedGuestForwarder final : public IGuestEndpointForwarder {
public:
    explicit BlockedGuestForwarder(std::string blocker_reason = "Guest runtime networking is not yet provisioned; live port forwarding blocked.");

    bool setup_forwarding(
        const std::string& serial_or_avd,
        uint16_t guest_port,
        GuestEndpointMapping& out_mapping) override;

    void teardown_forwarding(GuestEndpointMapping& mapping) override;
    bool is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept override;
    std::string_view forwarder_name() const noexcept override { return "blocked"; }

private:
    std::string blocker_reason_;
};

} // namespace fang::studio
