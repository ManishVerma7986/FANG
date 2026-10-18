#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>

#include "guest_manifest.hpp"

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

// Structured result for transport operations
struct TransportResult {
    bool success{false};
    std::string error_code;
    std::string diagnostic;
    std::string detail;
    
    static TransportResult ok(std::string diag = "") {
        TransportResult r; r.success = true; r.diagnostic = std::move(diag); return r;
    }
    static TransportResult fail(std::string code, std::string diag, std::string detail = "") {
        TransportResult r; r.success = false; r.error_code = std::move(code); r.diagnostic = std::move(diag); r.detail = std::move(detail); return r;
    }
};

// Guest daemon protocol constants
struct GuestDaemonProtocol {
    static constexpr uint16_t default_port = 24800;
    static constexpr std::string_view guest_staging_path = "/data/local/tmp/fang-input-daemon";
    static constexpr std::string_view daemon_binary_name = "fang-input-daemon";
    // Correct startup: direct execution of the native ELF binary with --port argument
    // The daemon is a standalone native executable, NOT a Java class for app_process
    static constexpr std::string_view start_cmd_template = 
        "/data/local/tmp/fang-input-daemon --port {port} --session-id {session_id} --generation-id {generation_id} --backend {backend}";
    // Readiness protocol: Hello/HelloAck handshake
    static constexpr uint32_t handshake_magic = 0x46495043; // "FIPC"
    static constexpr uint16_t handshake_version = 1;
    static constexpr std::size_t wire_header_size = 32;
    static constexpr std::chrono::milliseconds default_handshake_timeout{5000};
    static constexpr std::chrono::milliseconds default_startup_timeout{15000};
};

// Configuration for live transport operations
struct AdbTransportConfig {
    std::filesystem::path adb_executable;
    std::string target_serial;
    std::string guest_staging_path = "/data/local/tmp/fang-input-daemon";
    uint16_t guest_port = GuestDaemonProtocol::default_port;
    std::chrono::milliseconds command_timeout{10000};
    std::chrono::milliseconds push_timeout{30000};
    std::chrono::milliseconds startup_timeout{15000};
    std::chrono::milliseconds connection_timeout{5000};
    bool require_explicit_authorization{true};  // Default: require explicit opt-in
    std::string authorization_token;             // Must match configured token to enable live ops
};

// Abstract host-to-guest transport contract.
// Extends IGuestEndpointForwarder with binary transfer, daemon lifecycle, and connection verification.
class IGuestTransport : public IGuestEndpointForwarder {
public:
    virtual ~IGuestTransport() = default;

    // Binary transfer: push the daemon artifact to the guest staging path
    virtual TransportResult push_binary(
        const std::string& serial_or_avd,
        const std::filesystem::path& local_artifact_path,
        const std::string& guest_staging_path,
        uint64_t expected_size,
        const std::string& expected_sha256) = 0;

    // Verify remote artifact integrity
    virtual TransportResult verify_remote_artifact(
        const std::string& serial_or_avd,
        const std::string& guest_staging_path,
        uint64_t expected_size,
        const std::string& expected_sha256) = 0;

    // Start the guest daemon with session and generation IDs for protocol handshake
    virtual TransportResult start_daemon(
        const std::string& serial_or_avd,
        const std::string& guest_staging_path,
        const std::string& guest_port,
        const std::string& session_id,
        const std::string& generation_id) = 0;

    // Verify daemon is reachable and responsive with protocol handshake
    virtual TransportResult verify_connection(
        const std::string& serial_or_avd,
        uint16_t host_port,
        std::chrono::milliseconds timeout,
        const std::string& session_id,
        const std::string& generation_id) = 0;

    // Check if target device is online and compatible
    virtual TransportResult validate_target(
        const std::string& serial_or_avd,
        fang::guest::GuestTargetAbi expected_abi) = 0;

    // Get transport-specific configuration
    virtual std::string_view transport_name() const noexcept = 0;
    
    // Check if live operations are authorized
    virtual bool is_live_authorized() const noexcept = 0;
    
    // Set authorization token
    virtual void set_authorization(std::string token) = 0;
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

// Concrete ADB transport implementation for live guest operations.
// Requires explicit authorization token to enable live ADB execution.
class AdbTransport final : public IGuestTransport {
public:
    explicit AdbTransport(const AdbTransportConfig& config = {});
    
    // IGuestEndpointForwarder interface
    bool setup_forwarding(
        const std::string& serial_or_avd,
        uint16_t guest_port,
        GuestEndpointMapping& out_mapping) override;
    
    void teardown_forwarding(GuestEndpointMapping& mapping) override;
    bool is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept override;
    std::string_view forwarder_name() const noexcept override { return "adb-transport"; }

    // IGuestTransport interface
    TransportResult push_binary(
        const std::string& serial_or_avd,
        const std::filesystem::path& local_artifact_path,
        const std::string& guest_staging_path,
        uint64_t expected_size,
        const std::string& expected_sha256) override;
    
    TransportResult verify_remote_artifact(
        const std::string& serial_or_avd,
        const std::string& guest_staging_path,
        uint64_t expected_size,
        const std::string& expected_sha256) override;
    
    TransportResult start_daemon(
        const std::string& serial_or_avd,
        const std::string& guest_staging_path,
        const std::string& guest_port,
        const std::string& session_id,
        const std::string& generation_id) override;
    
    TransportResult verify_connection(
        const std::string& serial_or_avd,
        uint16_t host_port,
        std::chrono::milliseconds timeout,
        const std::string& session_id,
        const std::string& generation_id) override;
    
    TransportResult validate_target(
        const std::string& serial_or_avd,
        fang::guest::GuestTargetAbi expected_abi) override;
    
    std::string_view transport_name() const noexcept override { return "adb-transport"; }
    bool is_live_authorized() const noexcept override { return live_authorized_; }
    void set_authorization(std::string token) override;

    // Static helper for ADB command formatting
    static std::string format_adb_forward_cmd(const std::string& serial, uint16_t host_port, uint16_t guest_port);
    static std::string format_adb_push_cmd(const std::string& serial, const std::string& local, const std::string& remote);
    static std::string format_adb_shell_cmd(const std::string& serial, const std::string& command);

private:
    AdbTransportConfig config_;
    std::string authorization_token_;
    bool live_authorized_{false};
    
    static std::filesystem::path discover_adb_executable();
    TransportResult execute_adb_command(const std::vector<std::string>& args, std::chrono::milliseconds timeout, std::string* output = nullptr) const;
    bool verify_adb_available() const;
};

// Deterministic fake ADB transport for offline unit testing.
// Supports controlled outcomes for every operation.
class FakeAdbTransport final : public IGuestTransport {
public:
    explicit FakeAdbTransport() = default;
    
    // IGuestEndpointForwarder interface
    bool setup_forwarding(
        const std::string& serial_or_avd,
        uint16_t guest_port,
        GuestEndpointMapping& out_mapping) override;
    
    void teardown_forwarding(GuestEndpointMapping& mapping) override;
    bool is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept override;
    std::string_view forwarder_name() const noexcept override { return "fake-adb-transport"; }

    // IGuestTransport interface
    TransportResult push_binary(
        const std::string& serial_or_avd,
        const std::filesystem::path& local_artifact_path,
        const std::string& guest_staging_path,
        uint64_t expected_size,
        const std::string& expected_sha256) override;
    
    TransportResult verify_remote_artifact(
        const std::string& serial_or_avd,
        const std::string& guest_staging_path,
        uint64_t expected_size,
        const std::string& expected_sha256) override;
    
    TransportResult start_daemon(
        const std::string& serial_or_avd,
        const std::string& guest_staging_path,
        const std::string& guest_port,
        const std::string& session_id,
        const std::string& generation_id) override;
    
    TransportResult verify_connection(
        const std::string& serial_or_avd,
        uint16_t host_port,
        std::chrono::milliseconds timeout,
        const std::string& session_id,
        const std::string& generation_id) override;
    
    TransportResult validate_target(
        const std::string& serial_or_avd,
        fang::guest::GuestTargetAbi expected_abi) override;
    
    std::string_view transport_name() const noexcept override { return "fake-adb-transport"; }
    bool is_live_authorized() const noexcept override { return true; }  // Always authorized for testing
    void set_authorization(std::string /*token*/) override {}

    // Test control interface
    void set_push_result(TransportResult result) { push_result_ = std::move(result); }
    void set_verify_result(TransportResult result) { verify_result_ = std::move(result); }
    void set_start_daemon_result(TransportResult result) { start_daemon_result_ = std::move(result); }
    void set_verify_connection_result(TransportResult result) { verify_connection_result_ = std::move(result); }
    void set_validate_target_result(TransportResult result) { validate_target_result_ = std::move(result); }
    void set_setup_forwarding_result(bool success, std::string diag = "") { setup_success_ = success; setup_diag_ = std::move(diag); }
    
    // Handshake simulation control
    void set_handshake_success(bool success) { handshake_success_ = success; }
    void set_handshake_error_code(const std::string& code) { handshake_error_code_ = code; }
    void set_handshake_diagnostic(const std::string& diag) { handshake_diagnostic_ = diag; }
    
    // Track calls for verification
    struct CallLog {
        std::string operation;
        std::string serial;
        std::string detail;
        std::chrono::system_clock::time_point timestamp;
    };
    const std::vector<CallLog>& call_log() const noexcept { return call_log_; }
    void clear_call_log() { call_log_.clear(); }
    
    // Simulate device state
    void set_device_online(bool online) { device_online_ = online; }
    void set_device_abi(fang::guest::GuestTargetAbi abi) { device_abi_ = abi; }
    void set_device_serial(const std::string& serial) { device_serial_ = serial; }

private:
    TransportResult push_result_ = TransportResult::ok("Fake push succeeded");
    TransportResult verify_result_ = TransportResult::ok("Fake verify succeeded");
    TransportResult start_daemon_result_ = TransportResult::ok("Fake daemon started");
    TransportResult verify_connection_result_ = TransportResult::ok("Fake connection verified");
    TransportResult validate_target_result_ = TransportResult::ok("Fake target validated");
    bool setup_success_ = true;
    std::string setup_diag_ = "Fake forwarding active";
    std::vector<CallLog> call_log_;
    bool device_online_ = true;
    fang::guest::GuestTargetAbi device_abi_ = fang::guest::GuestTargetAbi::X86_64;
    std::string device_serial_ = "emulator-5554";
    
    // Handshake simulation
    bool handshake_success_ = true;
    std::string handshake_error_code_ = "HANDSHAKE_FAILED";
    std::string handshake_diagnostic_ = "Fake handshake failed";
    
    void log_call(const std::string& op, const std::string& serial, const std::string& detail);
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