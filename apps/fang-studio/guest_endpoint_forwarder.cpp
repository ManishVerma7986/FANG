#include "guest_endpoint_forwarder.hpp"
#include "adb_input_dispatcher.hpp"
#include "input_ipc_protocol.hpp"
#include "socket_input_transport.hpp"

#include <sstream>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstdlib>
#include <array>
#include <memory>
#include <span>

namespace fang::studio {

// ============================================================================
// Simple process executor for ADB commands
// ============================================================================

struct ProcessResult {
    int exit_code{-1};
    std::string output;
    std::string error;
    bool succeeded{false};
};

class AdbProcessExecutor {
public:
    ProcessResult execute(const std::filesystem::path& executable, const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
        ProcessResult result;
        
        // Build command line
        std::ostringstream cmd;
        cmd << "\"" << executable.string() << "\"";
        for (const auto& arg : args) {
            cmd << " \"" << arg << "\"";
        }
        std::string command = cmd.str();
        
        // On Windows, use _popen to capture output
        // For simplicity, we'll use a basic approach
        FILE* pipe = _popen(command.c_str(), "r");
        if (!pipe) {
            result.exit_code = -1;
            result.error = "Failed to execute command";
            result.succeeded = false;
            return result;
        }
        
        std::array<char, 128> buffer;
        std::string output;
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            output += buffer.data();
        }
        
        int exit_code = _pclose(pipe);
        result.exit_code = exit_code;
        result.output = output;
        result.succeeded = (exit_code == 0);
        
        return result;
    }
};

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
// AdbTransport
// ============================================================================

AdbTransport::AdbTransport(const AdbTransportConfig& config)
    : config_(config), live_authorized_(!config.require_explicit_authorization) {
    // Discover ADB executable if not provided
    if (config_.adb_executable.empty()) {
        config_.adb_executable = discover_adb_executable();
    }
}

std::filesystem::path AdbTransport::discover_adb_executable() {
    // Check common locations
    const char* android_home = std::getenv("ANDROID_HOME");
    if (android_home) {
        std::filesystem::path adb = std::filesystem::path(android_home) / "platform-tools" / "adb.exe";
        if (std::filesystem::exists(adb)) return adb;
    }
    
    const char* local_appdata = std::getenv("LOCALAPPDATA");
    if (local_appdata) {
        std::filesystem::path adb = std::filesystem::path(local_appdata) / "Android" / "Sdk" / "platform-tools" / "adb.exe";
        if (std::filesystem::exists(adb)) return adb;
    }
    
    // Check PATH
    return "adb.exe"; // Will be found via PATH
}

bool AdbTransport::verify_adb_available() const {
    if (!std::filesystem::exists(config_.adb_executable)) {
        return false;
    }
    auto result = execute_adb_command({"version"}, std::chrono::milliseconds(5000));
    return result.success;
}

std::string AdbTransport::format_adb_forward_cmd(const std::string& serial, uint16_t host_port, uint16_t guest_port) {
    std::ostringstream ss;
    ss << "adb -s " << serial << " forward tcp:" << host_port << " tcp:" << guest_port;
    return ss.str();
}

std::string AdbTransport::format_adb_push_cmd(const std::string& serial, const std::string& local, const std::string& remote) {
    std::ostringstream ss;
    ss << "adb -s " << serial << " push \"" << local << "\" \"" << remote << "\"";
    return ss.str();
}

std::string AdbTransport::format_adb_shell_cmd(const std::string& serial, const std::string& command) {
    std::ostringstream ss;
    ss << "adb -s " << serial << " shell " << command;
    return ss.str();
}

TransportResult AdbTransport::execute_adb_command(const std::vector<std::string>& args, std::chrono::milliseconds timeout, std::string* output) const {
    if (!verify_adb_available()) {
        return TransportResult::fail("ADB_NOT_FOUND", "ADB executable not found or not functional", config_.adb_executable.string());
    }

    std::vector<std::string> full_args = {"adb"};
    if (!config_.target_serial.empty()) {
        full_args.push_back("-s");
        full_args.push_back(config_.target_serial);
    }
    full_args.insert(full_args.end(), args.begin(), args.end());

    AdbProcessExecutor executor;
    auto result = executor.execute(config_.adb_executable, full_args, timeout);
    
    if (!result.succeeded) {
        return TransportResult::fail("ADB_COMMAND_FAILED", "ADB command failed with exit code " + std::to_string(result.exit_code), result.error.empty() ? result.output : result.error);
    }
    if (output) *output = result.output;
    return TransportResult::ok("Command succeeded");
}

bool AdbTransport::setup_forwarding(
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

    // Allocate host port
    uint16_t host_p = config_.guest_port;
    if (config_.target_serial.empty()) {
        host_p = guest_port;
    }

    out_mapping.host_address = "127.0.0.1";
    out_mapping.host_port = host_p;
    out_mapping.guest_port = guest_port;
    out_mapping.mechanism = ForwardingMechanism::AdbPortForward;

    if (!live_authorized_) {
        out_mapping.state = ForwardingState::Blocked;
        out_mapping.is_live_forwarding_active = false;
        out_mapping.diagnostic = "ADB port forwarding contract formulated (" +
            format_adb_forward_cmd(serial_or_avd, host_p, guest_port) +
            "); live ADB server execution requires explicit authorization.";
        return false;
    }

    // Execute live ADB forward
    auto result = execute_adb_command({"forward", "tcp:" + std::to_string(host_p), "tcp:" + std::to_string(guest_port)}, config_.command_timeout);
    if (!result.success) {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.is_live_forwarding_active = false;
        out_mapping.diagnostic = "ADB forward failed: " + result.diagnostic + " (" + result.detail + ")";
        return false;
    }

    out_mapping.state = ForwardingState::Active;
    out_mapping.is_live_forwarding_active = true;
    out_mapping.diagnostic = "ADB port forwarding active: " + format_adb_forward_cmd(serial_or_avd, host_p, guest_port);
    return true;
}

void AdbTransport::teardown_forwarding(GuestEndpointMapping& mapping) {
    if (live_authorized_ && mapping.is_live_forwarding_active && !config_.target_serial.empty()) {
        execute_adb_command({"forward", "--remove", "tcp:" + std::to_string(mapping.host_port)}, config_.command_timeout);
    }
    mapping.state = ForwardingState::NotConfigured;
    mapping.is_live_forwarding_active = false;
    mapping.diagnostic = "ADB port forwarding released.";
}

bool AdbTransport::is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept {
    return mapping.state == ForwardingState::Active && mapping.is_live_forwarding_active;
}

TransportResult AdbTransport::push_binary(
    const std::string& serial_or_avd,
    const std::filesystem::path& local_artifact_path,
    const std::string& guest_staging_path,
    uint64_t expected_size,
    const std::string& expected_sha256) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Binary push requires explicit authorization", "Set authorization token to enable live operations");
    }
    if (!std::filesystem::exists(local_artifact_path)) {
        return TransportResult::fail("ARTIFACT_NOT_FOUND", "Local artifact not found", local_artifact_path.string());
    }
    if (std::filesystem::file_size(local_artifact_path) != expected_size) {
        return TransportResult::fail("SIZE_MISMATCH", "Local artifact size mismatch", 
            "Expected: " + std::to_string(expected_size) + ", Actual: " + std::to_string(std::filesystem::file_size(local_artifact_path)));
    }

    // Push binary
    auto result = execute_adb_command({"push", local_artifact_path.string(), guest_staging_path}, config_.push_timeout);
    if (!result.success) {
        return TransportResult::fail("PUSH_FAILED", "ADB push failed", result.detail);
    }

    // Verify remote integrity
    return verify_remote_artifact(serial_or_avd, guest_staging_path, expected_size, expected_sha256);
}

TransportResult AdbTransport::verify_remote_artifact(
    const std::string& serial_or_avd,
    const std::string& guest_staging_path,
    uint64_t expected_size,
    const std::string& expected_sha256) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Remote verification requires explicit authorization", "");
    }

    // Check file size
    std::string output;
    auto size_result = execute_adb_command({"shell", "stat", "-c", "%s", guest_staging_path}, config_.command_timeout, &output);
    if (!size_result.success) {
        return TransportResult::fail("STAT_FAILED", "Failed to stat remote file", size_result.detail);
    }
    
    uint64_t remote_size = 0;
    try {
        remote_size = std::stoull(output);
    } catch (...) {
        return TransportResult::fail("PARSE_FAILED", "Failed to parse remote file size", output);
    }
    
    if (remote_size != expected_size) {
        return TransportResult::fail("SIZE_MISMATCH", "Remote artifact size mismatch", 
            "Expected: " + std::to_string(expected_size) + ", Actual: " + std::to_string(remote_size));
    }

    // Check SHA-256
    auto sha_result = execute_adb_command({"shell", "sha256sum", guest_staging_path}, config_.command_timeout, &output);
    if (!sha_result.success) {
        return TransportResult::fail("SHA_FAILED", "Failed to compute remote SHA-256", sha_result.detail);
    }
    
    // Parse sha256sum output: "hash  filepath"
    std::istringstream iss(output);
    std::string remote_sha;
    iss >> remote_sha;
    
    if (remote_sha != expected_sha256) {
        return TransportResult::fail("SHA_MISMATCH", "Remote artifact SHA-256 mismatch", 
            "Expected: " + expected_sha256 + ", Actual: " + remote_sha);
    }

    return TransportResult::ok("Remote artifact verified: size=" + std::to_string(remote_size) + ", sha256=" + remote_sha);
}

TransportResult AdbTransport::start_daemon(
    const std::string& serial_or_avd,
    const std::string& guest_staging_path,
    const std::string& guest_port,
    const std::string& session_id,
    const std::string& generation_id) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Daemon startup requires explicit authorization", "");
    }

    // Make executable
    auto chmod_result = execute_adb_command({"shell", "chmod", "755", guest_staging_path}, config_.command_timeout);
    if (!chmod_result.success) {
        return TransportResult::fail("CHMOD_FAILED", "Failed to make daemon executable", chmod_result.detail);
    }

    // Start daemon - the artifact is a native ELF binary, not a Java class
    // Execute directly with --port argument and required configuration
    // Use session_id and generation_id from deployment context for protocol handshake
    std::string cmd = guest_staging_path + " --port " + guest_port + 
        " --session-id " + session_id + 
        " --generation-id " + generation_id + 
        " --backend recording";
    auto result = execute_adb_command({"shell", cmd + " &"}, config_.startup_timeout);
    if (!result.success) {
        return TransportResult::fail("DAEMON_START_FAILED", "Failed to start guest daemon", result.detail);
    }

    // Give the daemon a moment to bind to the port
    execute_adb_command({"shell", "sleep", "1"}, std::chrono::milliseconds(2000));

    // Verify the process is actually running by checking the process list
    std::string ps_cmd = "ps -A | grep " + guest_staging_path;
    auto ps_result = execute_adb_command({"shell", ps_cmd}, config_.command_timeout);
    if (!ps_result.success || ps_result.detail.empty()) {
        return TransportResult::fail("DAEMON_PROCESS_NOT_FOUND", 
            "Daemon process not found after startup", 
            "Command: " + cmd);
    }

    return TransportResult::ok("Daemon startup verified: " + cmd + " (PID found)");
}

TransportResult AdbTransport::verify_connection(
    const std::string& serial_or_avd,
    uint16_t host_port,
    std::chrono::milliseconds timeout,
    const std::string& session_id,
    const std::string& generation_id) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Connection verification requires explicit authorization", "");
    }

    // Connect to the forwarded port and perform Hello/HelloAck handshake
    // This proves the daemon is listening, initialized, and protocol-compatible

    SocketTransportConfig transport_config;
    transport_config.host = "127.0.0.1";
    transport_config.port = host_port;
    transport_config.connect_timeout_ms = static_cast<uint32_t>(timeout.count());
    transport_config.send_timeout_ms = 1000;
    transport_config.tcp_nodelay = true;

    SocketInputTransport transport(transport_config);

    // Connect with timeout
    if (!transport.connect()) {
        return TransportResult::fail("CONNECTION_FAILED", 
            "Failed to connect to guest daemon on forwarded port " + std::to_string(host_port),
            transport.error_diagnostic());
    }

    // Build Hello message with expected session/generation IDs
    // These should match what the deployment coordinator configured
    HelloPayload hello_payload;
    hello_payload.client_version = input_protocol_version;
    hello_payload.display_width = 1080;
    hello_payload.display_height = 1920;
    hello_payload.session_id = session_id;
    hello_payload.generation_id = generation_id;

    uint64_t seq = 1;
    uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    auto hello_packet = encode_hello_message(seq, ts, hello_payload);

    // Send Hello
    std::span<const std::byte> hello_span(hello_packet.data(), hello_packet.data() + hello_packet.size());
    if (!transport.send_bytes(hello_span)) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_SEND_FAILED", 
            "Failed to send Hello message to guest daemon",
            transport.error_diagnostic());
    }

    // Receive HelloAck with timeout
    auto response = transport.receive_bytes(4096);
    if (response.empty()) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_TIMEOUT", 
            "No response from guest daemon within timeout",
            "Waited " + std::to_string(timeout.count()) + "ms");
    }

    // Decode response header
    if (response.size() < input_wire_header_size) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_MALFORMED", 
            "Response too small for wire header",
            "Got " + std::to_string(response.size()) + " bytes, need at least " + std::to_string(input_wire_header_size));
    }

    WireHeader resp_header;
    std::span<const std::byte> header_span(response.data(), response.data() + input_wire_header_size);
    auto decode_result = decode_wire_header(header_span, resp_header);
    if (decode_result != InputProtocolError::None) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_INVALID_HEADER", 
            "Failed to decode response header: " + std::string(input_protocol_error_name(decode_result)),
            "Magic: " + std::to_string(resp_header.magic) + ", Version: " + std::to_string(resp_header.version));
    }

    if (resp_header.type != InputMessageType::HelloAck) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_UNEXPECTED_MESSAGE", 
            "Expected HelloAck, got " + std::string(input_message_type_name(resp_header.type)),
            "Sequence: " + std::to_string(resp_header.sequence_number));
    }

    // Decode HelloAck payload
    std::span<const std::byte> payload_span(response.data() + input_wire_header_size, response.data() + input_wire_header_size + resp_header.payload_size);
    InputProtocolError payload_error;
    auto hello_ack = decode_hello_ack_message(payload_span, payload_error);
    if (!hello_ack.has_value()) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_DECODE_FAILED", 
            "Failed to decode HelloAck payload: " + std::string(input_protocol_error_name(payload_error)),
            "");
    }

    // Validate handshake response
    if (hello_ack->status != InputProtocolError::None) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_REJECTED", 
            "Guest daemon rejected handshake: " + std::string(input_protocol_error_name(hello_ack->status)),
            "Session: " + hello_ack->session_id + ", Generation: " + hello_ack->generation_id);
    }

    if (hello_ack->session_id != session_id) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_SESSION_MISMATCH", 
            "Session ID mismatch in HelloAck",
            "Expected: " + session_id + ", Got: " + hello_ack->session_id);
    }

    if (hello_ack->generation_id != generation_id) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_GENERATION_MISMATCH", 
            "Generation ID mismatch in HelloAck",
            "Expected: " + generation_id + ", Got: " + hello_ack->generation_id);
    }

    if (hello_ack->guest_version != input_protocol_version) {
        transport.disconnect();
        return TransportResult::fail("HANDSHAKE_VERSION_MISMATCH", 
            "Protocol version mismatch",
            "Expected: " + std::to_string(input_protocol_version) + ", Got: " + std::to_string(hello_ack->guest_version));
    }

    // Handshake successful - daemon is ready
    transport.disconnect();
    return TransportResult::ok("Readiness handshake successful: Hello/HelloAck exchanged, session=" + hello_ack->session_id + ", generation=" + hello_ack->generation_id + ", max_contacts=" + std::to_string(hello_ack->max_contacts));
}

TransportResult AdbTransport::validate_target(
    const std::string& serial_or_avd,
    fang::guest::GuestTargetAbi expected_abi) {

    if (!live_authorized_) {
        return TransportResult::fail("LIVE_OP_UNAUTHORIZED", "Target validation requires explicit authorization", "");
    }

    // Check device is online
    std::string output;
    auto devices_result = execute_adb_command({"devices"}, config_.command_timeout, &output);
    if (!devices_result.success) {
        return TransportResult::fail("ADB_DEVICES_FAILED", "Failed to list ADB devices", devices_result.detail);
    }

    // Check our target serial is in the list and online
    bool found = false;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find(serial_or_avd) == 0 && line.find("\tdevice") != std::string::npos) {
            found = true;
            break;
        }
    }
    if (!found) {
        return TransportResult::fail("DEVICE_OFFLINE", "Target device not found or offline", "Serial: " + serial_or_avd);
    }

    // Check ABI via ro.product.cpu.abi
    auto abi_result = execute_adb_command({"shell", "getprop", "ro.product.cpu.abi"}, config_.command_timeout, &output);
    if (!abi_result.success) {
        return TransportResult::fail("ABI_CHECK_FAILED", "Failed to get device ABI", abi_result.detail);
    }

    std::string device_abi = output;
    device_abi.erase(std::remove_if(device_abi.begin(), device_abi.end(), ::isspace), device_abi.end());

    std::string expected_abi_str;
    switch (expected_abi) {
    case fang::guest::GuestTargetAbi::X86_64: expected_abi_str = "x86_64"; break;
    case fang::guest::GuestTargetAbi::Arm64_v8a: expected_abi_str = "arm64-v8a"; break;
    case fang::guest::GuestTargetAbi::X86: expected_abi_str = "x86"; break;
    case fang::guest::GuestTargetAbi::Armeabi_v7a: expected_abi_str = "armeabi-v7a"; break;
    default: expected_abi_str = "unknown"; break;
    }

    if (device_abi != expected_abi_str) {
        return TransportResult::fail("ABI_MISMATCH", "Device ABI does not match expected artifact ABI", 
            "Expected: " + expected_abi_str + ", Device: " + device_abi);
    }

    return TransportResult::ok("Target validated: " + device_abi);
}

// ============================================================================
// AdbTransport::set_authorization
// ============================================================================

void AdbTransport::set_authorization(std::string token) {
    if (config_.require_explicit_authorization && !config_.authorization_token.empty()) {
        live_authorized_ = (token == config_.authorization_token);
    } else if (!config_.require_explicit_authorization) {
        live_authorized_ = true;
    }
}

// ============================================================================
// FakeAdbTransport
// ============================================================================

bool FakeAdbTransport::setup_forwarding(
    const std::string& serial_or_avd,
    uint16_t guest_port,
    GuestEndpointMapping& out_mapping) {

    log_call("setup_forwarding", serial_or_avd, "guest_port=" + std::to_string(guest_port));

    // Basic serial validation (match AdbTransport behavior)
    if (serial_or_avd.empty() || serial_or_avd.find(';') != std::string::npos || 
        serial_or_avd.find('&') != std::string::npos) {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.mechanism = ForwardingMechanism::AdbPortForward;
        out_mapping.diagnostic = "Invalid target ADB serial: '" + serial_or_avd + "'";
        out_mapping.is_live_forwarding_active = false;
        return false;
    }

    if (setup_success_) {
        out_mapping.host_address = "127.0.0.1";
        out_mapping.host_port = guest_port;
        out_mapping.guest_port = guest_port;
        out_mapping.state = ForwardingState::Active;
        out_mapping.mechanism = ForwardingMechanism::AdbPortForward;
        out_mapping.diagnostic = setup_diag_;
        out_mapping.is_live_forwarding_active = true;
        return true;
    } else {
        out_mapping.state = ForwardingState::Failed;
        out_mapping.mechanism = ForwardingMechanism::AdbPortForward;
        out_mapping.diagnostic = setup_diag_;
        out_mapping.is_live_forwarding_active = false;
        return false;
    }
}

void FakeAdbTransport::teardown_forwarding(GuestEndpointMapping& mapping) {
    log_call("teardown_forwarding", "", "host_port=" + std::to_string(mapping.host_port));
    mapping.state = ForwardingState::NotConfigured;
    mapping.is_live_forwarding_active = false;
    mapping.diagnostic = "Fake ADB forwarding released.";
}

bool FakeAdbTransport::is_forwarding_active(const GuestEndpointMapping& mapping) const noexcept {
    return mapping.state == ForwardingState::Active && mapping.is_live_forwarding_active;
}

TransportResult FakeAdbTransport::push_binary(
    const std::string& serial_or_avd,
    const std::filesystem::path& local_artifact_path,
    const std::string& guest_staging_path,
    uint64_t expected_size,
    const std::string& expected_sha256) {

    log_call("push_binary", serial_or_avd, local_artifact_path.string() + " -> " + guest_staging_path);
    return push_result_;
}

TransportResult FakeAdbTransport::verify_remote_artifact(
    const std::string& serial_or_avd,
    const std::string& guest_staging_path,
    uint64_t expected_size,
    const std::string& expected_sha256) {

    log_call("verify_remote_artifact", serial_or_avd, guest_staging_path);
    return verify_result_;
}

TransportResult FakeAdbTransport::start_daemon(
    const std::string& serial_or_avd,
    const std::string& guest_staging_path,
    const std::string& guest_port,
    const std::string& session_id,
    const std::string& generation_id) {

    log_call("start_daemon", serial_or_avd, guest_staging_path + " port=" + guest_port + " session=" + session_id + " gen=" + generation_id);
    return start_daemon_result_;
}

TransportResult FakeAdbTransport::verify_connection(
    const std::string& serial_or_avd,
    uint16_t host_port,
    std::chrono::milliseconds timeout,
    const std::string& session_id,
    const std::string& generation_id) {

    log_call("verify_connection", serial_or_avd, "host_port=" + std::to_string(host_port) + " timeout=" + std::to_string(timeout.count()) + " session=" + session_id + " gen=" + generation_id);
    
    if (!handshake_success_) {
        return TransportResult::fail(handshake_error_code_, handshake_diagnostic_, "");
    }
    
    return verify_connection_result_;
}

TransportResult FakeAdbTransport::validate_target(
    const std::string& serial_or_avd,
    fang::guest::GuestTargetAbi expected_abi) {

    log_call("validate_target", serial_or_avd, "expected_abi=" + std::to_string(static_cast<int>(expected_abi)));
    
    if (!device_online_) {
        return TransportResult::fail("DEVICE_OFFLINE", "Fake device is offline", "");
    }
    
    if (serial_or_avd != device_serial_) {
        return TransportResult::fail("DEVICE_NOT_FOUND", "Fake device serial mismatch", "Expected: " + device_serial_ + ", Got: " + serial_or_avd);
    }
    
    std::string expected_abi_str;
    switch (expected_abi) {
    case fang::guest::GuestTargetAbi::X86_64: expected_abi_str = "x86_64"; break;
    case fang::guest::GuestTargetAbi::Arm64_v8a: expected_abi_str = "arm64-v8a"; break;
    case fang::guest::GuestTargetAbi::X86: expected_abi_str = "x86"; break;
    case fang::guest::GuestTargetAbi::Armeabi_v7a: expected_abi_str = "armeabi-v7a"; break;
    default: expected_abi_str = "unknown"; break;
    }
    
    std::string device_abi_str;
    switch (device_abi_) {
    case fang::guest::GuestTargetAbi::X86_64: device_abi_str = "x86_64"; break;
    case fang::guest::GuestTargetAbi::Arm64_v8a: device_abi_str = "arm64-v8a"; break;
    case fang::guest::GuestTargetAbi::X86: device_abi_str = "x86"; break;
    case fang::guest::GuestTargetAbi::Armeabi_v7a: device_abi_str = "armeabi-v7a"; break;
    default: device_abi_str = "unknown"; break;
    }
    
    if (device_abi_str != expected_abi_str) {
        return TransportResult::fail("ABI_MISMATCH", "Fake device ABI mismatch", "Expected: " + expected_abi_str + ", Device: " + device_abi_str);
    }
    
    return validate_target_result_;
}

void FakeAdbTransport::log_call(const std::string& op, const std::string& serial, const std::string& detail) {
    CallLog log;
    log.operation = op;
    log.serial = serial;
    log.detail = detail;
    log.timestamp = std::chrono::system_clock::now();
    call_log_.push_back(log);
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