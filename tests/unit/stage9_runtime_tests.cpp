#include "guest_endpoint_forwarder.hpp"
#include "guest_input_backend.hpp"
#include "guest_input_daemon.hpp"
#include "guest_input_bridge.hpp"
#include "guest_manifest.hpp"
#include "guest_deployment_coordinator.hpp"
#include "input_ipc_protocol.hpp"
#include "input_mapping_engine.hpp"
#include "socket_input_transport.hpp"
#include "studio_session.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace fang::studio;
using namespace fang::guest;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename F>
bool poll_until(F&& condition, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000), std::chrono::milliseconds step = std::chrono::milliseconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(step);
    }
    return condition();
}

// RAII guard for deterministic thread cleanup under normal exit or exception
struct ThreadJoiner {
    std::atomic<bool>& flag;
    std::thread& th;
    ~ThreadJoiner() {
        flag.store(false);
        if (th.joinable()) {
            th.join();
        }
    }
};

// ----------------------------------------------------------------------------
// 1. Forwarder Abstraction Tests
// ----------------------------------------------------------------------------

void test_forwarder_abstractions() {
    std::cout << "  [RUN] test_forwarder_abstractions..." << std::endl;

    // Loopback test forwarder
    {
        LoopbackTestForwarder forwarder(24800);
        require(forwarder.forwarder_name() == "loopback-test", "Forwarder name check");
        GuestEndpointMapping mapping;
        require(mapping.state == ForwardingState::NotConfigured, "Initial state NotConfigured");

        bool ok = forwarder.setup_forwarding("emulator-5554", 24800, mapping);
        require(ok, "Loopback setup succeeds");
        require(mapping.state == ForwardingState::Active, "Active state");
        require(mapping.host_port == 24800, "Port assigned");
        require(mapping.is_live_forwarding_active, "Forwarding active");
        require(forwarder.is_forwarding_active(mapping), "is_forwarding_active true");

        forwarder.teardown_forwarding(mapping);
        require(mapping.state == ForwardingState::NotConfigured, "Torn down");
        require(!forwarder.is_forwarding_active(mapping), "is_forwarding_active false");

        // Failure simulation
        forwarder.set_simulate_failure(true);
        ok = forwarder.setup_forwarding("emulator-5554", 24800, mapping);
        require(!ok, "Simulated failure returns false");
        require(mapping.state == ForwardingState::Failed, "Failed state");
    }

    // AdbPortForwarder
    {
        AdbPortForwarder adb_fwd(24800);
        require(adb_fwd.forwarder_name() == "adb-forward", "Adb forwarder name");
        std::string cmd = AdbPortForwarder::format_adb_forward_cmd("emulator-5554", 24800, 24800);
        require(cmd == "adb -s emulator-5554 forward tcp:24800 tcp:24800", "Formatted adb cmd check");

        GuestEndpointMapping mapping;
        // Invalid serial
        bool ok = adb_fwd.setup_forwarding("bad;serial&injection", 24800, mapping);
        require(!ok, "Invalid serial rejected");
        require(mapping.state == ForwardingState::Failed, "Failed state on invalid serial");

        // Valid serial: offline policy must safely mark as Blocked rather than executing live adb
        ok = adb_fwd.setup_forwarding("emulator-5554", 24800, mapping);
        require(!ok, "Live adb execution blocked offline");
        require(mapping.state == ForwardingState::Blocked, "Blocked state");
        require(!mapping.is_live_forwarding_active, "Not live active");
        require(mapping.diagnostic.find("unauthorized") != std::string::npos, "Diagnostic identifies unauthorized live ADB");
    }

    // BlockedGuestForwarder
    {
        BlockedGuestForwarder blocked_fwd("QEMU user networking not yet provisioned.");
        GuestEndpointMapping mapping;
        bool ok = blocked_fwd.setup_forwarding("emulator-5554", 24800, mapping);
        require(!ok, "Blocked returns false");
        require(mapping.state == ForwardingState::Blocked, "Blocked state");
        require(mapping.diagnostic == "QEMU user networking not yet provisioned.", "Diagnostic matches reason");
    }

    std::cout << "  [PASS] test_forwarder_abstractions" << std::endl;
}

// ----------------------------------------------------------------------------
// 2. Guest Manifest & Toolchain Detector Tests
// ----------------------------------------------------------------------------

void test_guest_manifest_and_toolchain_detector() {
    std::cout << "  [RUN] test_guest_manifest_and_toolchain_detector..." << std::endl;

    // ABI parser
    require(parse_guest_target_abi("x86_64") == GuestTargetAbi::X86_64, "x86_64 parsed");
    require(parse_guest_target_abi("arm64-v8a") == GuestTargetAbi::Arm64_v8a, "arm64-v8a parsed");
    require(parse_guest_target_abi("host-native") == GuestTargetAbi::HostNative, "host-native parsed");

    // Environment inspection
    auto env = GuestToolchainDetector::inspect_environment();
    if (!env.ndk_found) {
        require(!env.missing_tools.empty(), "Missing prerequisites reported when NDK absent");
    }

    // Host Native manifest
    auto host_manifest = GuestToolchainDetector::generate_manifest_for_target(
        GuestTargetAbi::HostNative, "Release", "RecordingStub", false);
    require(host_manifest.validate(), "Host manifest validates");
    require(!host_manifest.is_android_elf_binary, "Host manifest does not claim to be Android ELF");
#if defined(_WIN32)
    require(host_manifest.artifact_format == "PE-COFF", "Windows host is PE-COFF");
    require(host_manifest.target_os == "windows", "Target OS is windows");
#endif

    // Android x86_64 manifest
    auto android_manifest = GuestToolchainDetector::generate_manifest_for_target(
        GuestTargetAbi::X86_64, "Release", "RecordingStub", false);
    require(android_manifest.target_os == "android", "Target OS android");
    require(android_manifest.target_abi == "x86_64", "Target ABI x86_64");

    if (!env.ndk_found || !env.clang_found) {
        require(android_manifest.toolchain_status == ToolchainStatus::MissingNdk, "Reports MissingNdk when toolchain absent");
        require(!android_manifest.is_android_elf_binary, "Does not claim Android binary when toolchain missing");
        require(android_manifest.artifact_format == "Unavailable (Missing Toolchain)", "Artifact marked unavailable");
    }

    // JSON serialization and round-trip
    std::string json = host_manifest.to_json();
    require(json.find("\"artifact_name\":") != std::string::npos, "JSON has artifact_name");
    require(json.find("\"protocol_version\": 1") != std::string::npos, "JSON has protocol version");

    GuestArtifactManifest parsed;
    std::string err;
    require(GuestArtifactManifest::from_json(json, parsed, &err), "Manifest from_json roundtrip succeeds: " + err);
    require(parsed.artifact_name == host_manifest.artifact_name, "Parsed artifact name matches");
    require(parsed.protocol_version == host_manifest.protocol_version, "Parsed protocol version matches");

    // Invariant validation: ensure fraudulent manifest claiming Windows PE is Android ELF fails validation
    GuestArtifactManifest fraudulent = host_manifest;
    fraudulent.is_android_elf_binary = true; // Illegal combination for PE-COFF
    std::string fraud_err;
    require(!fraudulent.validate(&fraud_err), "Fraudulent manifest rejected");
    require(fraud_err.find("Invalid manifest invariant") != std::string::npos, "Reason captured");

    std::cout << "  [PASS] test_guest_manifest_and_toolchain_detector" << std::endl;
}

// ----------------------------------------------------------------------------
// 3. Integrated Studio Session Lifecycle with Forwarder and Bridge
// ----------------------------------------------------------------------------

void test_studio_session_integrated_lifecycle() {
    std::cout << "  [RUN] test_studio_session_integrated_lifecycle..." << std::endl;

    // Start local daemon on ephemeral port
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Daemon server started");
    const uint16_t daemon_port = server.bound_port();

    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon::Config dcfg;
    dcfg.expected_session_id = "stage9_session";
    dcfg.expected_generation_id = "gen-stage9";

    // Forwarder pointing to daemon's ephemeral port
    auto forwarder = std::make_shared<LoopbackTestForwarder>(daemon_port);

    // Transport and bridge
    SocketTransportConfig tcfg;
    tcfg.host = "127.0.0.1";
    tcfg.port = daemon_port;
    tcfg.connect_timeout_ms = 1000;
    auto transport = std::make_shared<SocketInputTransport>(tcfg);

    GuestInputBridgeClient::Config bcfg;
    bcfg.session_id = "stage9_session";
    bcfg.generation_id = "gen-stage9";
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, bcfg);

    auto mapping_engine = std::make_shared<InputMappingEngine>();
    mapping_engine->set_gameplay_mode_enabled(true);
    mapping_engine->set_viewport_focused(true);
    mapping_engine->set_display_dimensions({1080, 1920});

    StudioSessionController controller(
        nullptr, nullptr, nullptr, nullptr,
        mapping_engine, forwarder, bridge);

    // 1. Initial state
    auto snap = controller.snapshot();
    require(snap.phase == StudioSessionPhase::NotStarted, "Initial NotStarted phase");
    require(snap.bridge_state == StudioBridgeState::Unconfigured, "Bridge unconfigured");

    // 2. Supervisor transitions to Starting
    SupervisorRuntimeSnapshot starting_snap;
    starting_snap.runtime.state = fang::broker::RuntimeLifecycleState::Starting;
    starting_snap.runtime.generation_id = "gen-stage9";
    starting_snap.runtime.process_id = 9999;
    starting_snap.runtime.avd_name = "Pixel_7_API_34";
    starting_snap.runtime.adb_serial = "emulator-5554";
    starting_snap.runtime.guest_ready = false;

    snap = controller.update(starting_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
    require(snap.phase == StudioSessionPhase::Starting, "Starting phase");
    require(snap.bridge_state == StudioBridgeState::Unconfigured, "Bridge not yet active while guest booting");

    // 3. Supervisor transitions to Ready with guest_ready = true
    SupervisorRuntimeSnapshot ready_snap = starting_snap;
    ready_snap.runtime.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_snap.runtime.guest_ready = true;

    // Run a background thread to accept and pump daemon packets
    std::atomic<bool> run_daemon{true};
    std::thread daemon_thread([&]() {
        auto client_transport = server.accept_client(2000);
        if (client_transport) {
            GuestInputDaemon daemon(client_transport, backend, dcfg);
            daemon.start();
            while (run_daemon.load()) {
                daemon.poll_and_process();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            daemon.shutdown();
        }
    });
    ThreadJoiner joiner(run_daemon, daemon_thread);

    snap = controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
    require(snap.is_forwarding_active, "Forwarding became active");
    require(snap.forwarded_host_port == daemon_port, "Forwarded host port matches");

    // Wait for handshake to complete via update() calls
    bool connected = poll_until([&]() {
        snap = controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
        return snap.bridge_state == StudioBridgeState::Connected;
    }, std::chrono::milliseconds(3000));

    require(connected, "Bridge reached Connected state via update()");
    require(snap.bridge_capability == MultiTouchBackendCapability::Supported, "Capability is Supported");
    require(mapping_engine->multi_touch_capability() == MultiTouchBackendCapability::Supported, "Mapping engine updated capability");

    // 4. Send a multi-touch frame through mapping engine
    mapping_engine->set_viewport_focused(true);
    HostPointerButtonEvent press_evt;
    press_evt.button = HostMouseButton::Left;
    press_evt.is_down = true;
    press_evt.client_x = 200;
    press_evt.client_y = 300;
    RECT rect{0, 0, 1080, 1920};
    auto res = mapping_engine->process_pointer_button(press_evt, rect);
    require(res.success, "Touch press accepted: " + res.diagnostic_message);

    // Collect active contacts from model and send touch contact frame through bridge
    std::vector<TouchContactState> active_contacts;
    for (uint32_t i = 0; i < MultiTouchContactModel::max_concurrent_contacts; ++i) {
        auto st = mapping_engine->contact_model().contact_state(i);
        if (st && st->is_active) {
            active_contacts.push_back(*st);
        }
    }
    require(!active_contacts.empty(), "At least one active contact in model");
    require(bridge->send_touch_frame(active_contacts), "Bridge accepts touch contact frame");

    // Allow daemon to process touch
    bool received = poll_until([&]() {
        return backend->event_count() > 0;
    }, std::chrono::milliseconds(2000));
    require(received, "Daemon backend received touch frame from integrated session");

    // 5. Test shutdown cleanup
    controller.notify_shutdown();
    snap = controller.snapshot();
    require(snap.phase == StudioSessionPhase::Stopped, "Phase is Stopped");
    require(snap.bridge_state == StudioBridgeState::Unconfigured, "Bridge unconfigured on shutdown");
    require(!forwarder->is_forwarding_active(GuestEndpointMapping{}), "Forwarder torn down");

    run_daemon.store(false);
    if (daemon_thread.joinable()) {
        daemon_thread.join();
    }
    server.stop();

    std::cout << "  [PASS] test_studio_session_integrated_lifecycle" << std::endl;
}

// ----------------------------------------------------------------------------
// 4. Blocked Reachability and Generation Reset Tests
// ----------------------------------------------------------------------------

void test_blocked_reachability_and_generation_reset() {
    std::cout << "  [RUN] test_blocked_reachability_and_generation_reset..." << std::endl;

    auto blocked_forwarder = std::make_shared<BlockedGuestForwarder>("Guest networking blocked pending driver provisioning.");
    auto mapping_engine = std::make_shared<InputMappingEngine>();

    StudioSessionController controller(
        nullptr, nullptr, nullptr, nullptr,
        mapping_engine, blocked_forwarder, nullptr);

    SupervisorRuntimeSnapshot ready_snap;
    ready_snap.runtime.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_snap.runtime.guest_ready = true;
    ready_snap.runtime.adb_serial = "emulator-5554";
    ready_snap.runtime.generation_id = "gen-block-1";

    auto snap = controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
    require(snap.bridge_state == StudioBridgeState::Blocked, "Bridge marked Blocked");
    require(snap.bridge_diagnostic.find("driver provisioning") != std::string::npos, "Diagnostic shows blocker reason");
    require(!snap.is_forwarding_active, "Forwarding not active");

    // Generation Change / Invalidation
    SupervisorRuntimeSnapshot gen2_snap = ready_snap;
    gen2_snap.runtime.generation_id = "gen-block-2";

    snap = controller.update(gen2_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
    require(snap.generation_id == "gen-block-2", "Generation updated to gen-block-2");
    require(snap.bridge_state == StudioBridgeState::Blocked, "Stays safely blocked");

    std::cout << "  [PASS] test_blocked_reachability_and_generation_reset" << std::endl;
}

// ----------------------------------------------------------------------------
// 5. Peer Disconnect and Error Reset Tests
// ----------------------------------------------------------------------------

void test_peer_disconnect_and_error_reset() {
    std::cout << "  [RUN] test_peer_disconnect_and_error_reset..." << std::endl;

    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Server started");
    const uint16_t port = server.bound_port();

    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon::Config dcfg;
    dcfg.expected_session_id = "err_session";
    dcfg.expected_generation_id = "gen-err";

    auto forwarder = std::make_shared<LoopbackTestForwarder>(port);

    SocketTransportConfig tcfg;
    tcfg.host = "127.0.0.1";
    tcfg.port = port;
    tcfg.connect_timeout_ms = 1000;
    auto transport = std::make_shared<SocketInputTransport>(tcfg);

    GuestInputBridgeClient::Config bcfg;
    bcfg.session_id = "err_session";
    bcfg.generation_id = "gen-err";
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, bcfg);

    auto mapping_engine = std::make_shared<InputMappingEngine>();
    mapping_engine->set_gameplay_mode_enabled(true);
    mapping_engine->set_viewport_focused(true);
    mapping_engine->set_display_dimensions({1080, 1920});

    StudioSessionController controller(
        nullptr, nullptr, nullptr, nullptr,
        mapping_engine, forwarder, bridge);

    SupervisorRuntimeSnapshot ready_snap;
    ready_snap.runtime.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_snap.runtime.guest_ready = true;
    ready_snap.runtime.adb_serial = "emulator-5554";
    ready_snap.runtime.generation_id = "gen-err";

    std::shared_ptr<IInputTransport> accepted_client;
    std::atomic<bool> run_daemon{true};
    std::thread daemon_thread([&]() {
        accepted_client = server.accept_client(2000);
        if (accepted_client) {
            GuestInputDaemon daemon(accepted_client, backend, dcfg);
            daemon.start();
            while (run_daemon.load()) {
                daemon.poll_and_process();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            daemon.shutdown();
        }
    });
    ThreadJoiner joiner(run_daemon, daemon_thread);

    controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});

    bool connected = poll_until([&]() {
        auto s = controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
        return s.bridge_state == StudioBridgeState::Connected;
    }, std::chrono::milliseconds(3000));
    require(connected, "Connected");

    // Simulate active contact in mapping engine
    mapping_engine->set_viewport_focused(true);
    HostPointerButtonEvent press_evt;
    press_evt.button = HostMouseButton::Left;
    press_evt.is_down = true;
    press_evt.client_x = 100;
    press_evt.client_y = 100;
    RECT rect{0, 0, 1080, 1920};
    mapping_engine->process_pointer_button(press_evt, rect);
    require(mapping_engine->active_contact_count() == 1, "1 active contact in mapping engine");

    // Abruptly sever the connection from daemon side
    run_daemon.store(false);
    if (accepted_client) {
        accepted_client->disconnect();
    }
    server.stop();
    if (daemon_thread.joinable()) {
        daemon_thread.join();
    }

    // Call update(): bridge must detect error/disconnect and reset active contacts in mapping engine
    bool detected_error = poll_until([&]() {
        auto s = controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
        return s.bridge_state == StudioBridgeState::Error || s.bridge_state == StudioBridgeState::Connecting;
    }, std::chrono::milliseconds(2000));

    require(detected_error, "Controller detected transport error on disconnect");
    require(mapping_engine->active_contact_count() == 0, "Mapping engine cleanly reset stuck contacts on disconnect");

    controller.notify_shutdown();
    std::cout << "  [PASS] test_peer_disconnect_and_error_reset" << std::endl;
}

// ----------------------------------------------------------------------------
// 6. Guest Artifact Readiness & Toolchain Missing Tests
// ----------------------------------------------------------------------------

void test_guest_artifact_readiness_toolchain_missing() {
    std::cout << "  [RUN] test_guest_artifact_readiness_toolchain_missing..." << std::endl;

    GuestDeploymentCoordinator coordinator;
    GuestDeploymentTarget target;
    target.abi = GuestTargetAbi::X86_64;
    target.staging_dir = "nonexistent_staging_directory_for_test";

    auto report = coordinator.assess_deployment_readiness(target);
    auto env = GuestToolchainDetector::inspect_environment();

    if (!env.ndk_found || !env.clang_found) {
        require(report.phase == GuestDeploymentPhase::BlockedMissingToolchain,
                "Reports BlockedMissingToolchain when NDK missing");
        require(report.diagnostic.find("Android NDK r25+") != std::string::npos,
                "Diagnostic provides actionable guidance on missing NDK");
        require(!report.is_ready_for_push, "Not ready for push");
        require(!report.is_deployed, "Not deployed");
    }

    std::cout << "  [PASS] test_guest_artifact_readiness_toolchain_missing" << std::endl;
}

// ----------------------------------------------------------------------------
// 7. Guest Artifact Readiness: PE vs ELF Architecture Mismatch Tests
// ----------------------------------------------------------------------------

void test_guest_artifact_readiness_pe_vs_elf_mismatch() {
    std::cout << "  [RUN] test_guest_artifact_readiness_pe_vs_elf_mismatch..." << std::endl;

    const auto temp_dir = std::filesystem::temp_directory_path() / "fang_test_staging_pe";
    std::filesystem::create_directories(temp_dir);

    const auto pe_file = temp_dir / "fang-input-daemon-android-x86_64";
    {
        std::ofstream out(pe_file, std::ios::binary);
        const char mz_header[4] = {'M', 'Z', 0x00, 0x00};
        out.write(mz_header, sizeof(mz_header));
        out << "PE Windows executable payload dummy data for testing architecture mismatch";
    }

    GuestDeploymentCoordinator coordinator;
    GuestDeploymentTarget target;
    target.abi = GuestTargetAbi::X86_64;
    target.staging_dir = temp_dir.string();

    auto report = coordinator.assess_deployment_readiness(target);
    require(report.phase == GuestDeploymentPhase::ArtifactArchitectureMismatch,
            "Rejects Windows PE binary for Android target as ArtifactArchitectureMismatch");
    require(report.diagnostic.find("PE-COFF") != std::string::npos,
            "Diagnostic identifies PE-COFF format");
    require(report.diagnostic.find("ELF executable") != std::string::npos,
            "Diagnostic explains ELF requirement");
    require(!report.is_ready_for_push, "Must not be ready for push");

    std::filesystem::remove_all(temp_dir);
    std::cout << "  [PASS] test_guest_artifact_readiness_pe_vs_elf_mismatch" << std::endl;
}

// ----------------------------------------------------------------------------
// 8. Guest Artifact Readiness: Valid ELF and Safe Offline Policy Tests
// ----------------------------------------------------------------------------

void write_synthetic_elf64(
    const std::filesystem::path& path,
    uint16_t machine = 0x3E,       // EM_X86_64
    uint8_t elf_class = 2,         // ELFCLASS64
    uint8_t data_encoding = 1,     // ELFDATA2LSB (little-endian)
    uint16_t object_type = 3,      // ET_DYN (PIE executable)
    uint16_t phnum = 1) {
    std::ofstream out(path, std::ios::binary);
    uint8_t header[64]{0};
    header[0] = 0x7F;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = elf_class;
    header[5] = data_encoding;
    header[6] = 1; // EV_CURRENT
    header[7] = 0; // ELFOSABI_NONE / System V
    header[16] = static_cast<uint8_t>(object_type & 0xFF);
    header[17] = static_cast<uint8_t>((object_type >> 8) & 0xFF);
    header[18] = static_cast<uint8_t>(machine & 0xFF);
    header[19] = static_cast<uint8_t>((machine >> 8) & 0xFF);
    header[20] = 1; // version
    header[24] = 0x00; // entry point
    header[25] = 0x10;
    header[32] = 64;   // phoff
    header[40] = 128;  // shoff
    header[52] = 64;   // ehsize
    header[54] = 56;   // phentsize
    header[56] = static_cast<uint8_t>(phnum & 0xFF);
    header[57] = static_cast<uint8_t>((phnum >> 8) & 0xFF);
    header[58] = 64;   // shentsize
    header[60] = 2;    // shnum

    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    const char dummy[128] = "Fang synthetic ELF binary payload for testing offline cross-compilation pipeline and artifact validation";
    out.write(dummy, sizeof(dummy));
}

// ----------------------------------------------------------------------------
// 8. Guest Artifact Readiness: Valid ELF and Safe Offline Policy Tests
// ----------------------------------------------------------------------------

void test_guest_artifact_readiness_valid_elf_and_offline_policy() {
    std::cout << "  [RUN] test_guest_artifact_readiness_valid_elf_and_offline_policy..." << std::endl;

    const auto temp_dir = std::filesystem::temp_directory_path() / "fang_test_staging_elf";
    std::filesystem::create_directories(temp_dir);

    const auto elf_file = temp_dir / "fang-input-daemon-android-x86_64";
    write_synthetic_elf64(elf_file, 0x3E, 2, 1, 3, 1);

    GuestDeploymentCoordinator coordinator;
    GuestDeploymentTarget target;
    target.abi = GuestTargetAbi::X86_64;
    target.staging_dir = temp_dir.string();

    auto report = coordinator.assess_deployment_readiness(target);
    require(report.phase == GuestDeploymentPhase::ArtifactReady, "ELF binary marked ArtifactReady");
    require(report.is_ready_for_push, "is_ready_for_push is true");
    require(!report.artifact_sha256.empty(), "SHA-256 computed");
    require(report.artifact_sha256.size() == 64, "SHA-256 is 64 hex characters");
    require(report.artifact_size_bytes > 0, "Artifact size non-zero");

    // Test deployment boundary: offline safety must block live ADB push
    GuestDeploymentReport deploy_report;
    bool ok = coordinator.deploy_to_guest("emulator-5554", target, deploy_report);
    require(!ok, "Live guest deployment returns false offline");
    require(deploy_report.phase == GuestDeploymentPhase::DeploymentBlockedOffline,
            "Phase is DeploymentBlockedOffline");
    require(!deploy_report.is_deployed, "is_deployed is false");
    require(deploy_report.diagnostic.find("unauthorized") != std::string::npos ||
            deploy_report.diagnostic.find("blocked offline") != std::string::npos,
            "Diagnostic states offline safety constraint");

    std::filesystem::remove_all(temp_dir);
    std::cout << "  [PASS] test_guest_artifact_readiness_valid_elf_and_offline_policy" << std::endl;
}

// ----------------------------------------------------------------------------
// 9. Deep ELF Header Inspection & Validation Tests
// ----------------------------------------------------------------------------

void test_elf_header_deep_inspection_and_validation() {
    std::cout << "  [RUN] test_elf_header_deep_inspection_and_validation..." << std::endl;

    const auto temp_dir = std::filesystem::temp_directory_path() / "fang_test_elf_validation";
    std::filesystem::create_directories(temp_dir);

    GuestDeploymentCoordinator coordinator;

    // A. Valid ELF64 x86_64
    {
        const auto valid_file = temp_dir / "valid_elf64";
        write_synthetic_elf64(valid_file, 0x3E, 2, 1, 3, 1);

        ElfHeaderInspection insp;
        std::string err;
        bool ok = coordinator.validate_elf_for_abi(valid_file.string(), GuestTargetAbi::X86_64, insp, &err);
        require(ok, "Valid ELF64 x86_64 succeeds validation");
        require(insp.is_valid_elf, "is_valid_elf is true");
        require(insp.elf_class == 2, "64-bit ELF class");
        require(insp.data_encoding == 1, "Little-endian encoding");
        require(insp.machine_arch == 0x3E, "Machine arch 0x3E");
        require(insp.object_type == 3, "ET_DYN object type");
        require(insp.program_header_count == 1, "phnum is 1");
    }

    // B. Truncated ELF (< 64 bytes)
    {
        const auto trunc_file = temp_dir / "trunc_elf";
        {
            std::ofstream out(trunc_file, std::ios::binary);
            uint8_t short_hdr[32]{0};
            short_hdr[0] = 0x7F; short_hdr[1] = 'E'; short_hdr[2] = 'L'; short_hdr[3] = 'F';
            short_hdr[4] = 2; // ELF64
            out.write(reinterpret_cast<const char*>(short_hdr), sizeof(short_hdr));
        }

        ElfHeaderInspection insp;
        std::string err;
        bool ok = coordinator.validate_elf_for_abi(trunc_file.string(), GuestTargetAbi::X86_64, insp, &err);
        require(!ok, "Truncated ELF header fails validation");
        require(insp.format == "Truncated", "Format identified as Truncated");
        require(err.find("truncated") != std::string::npos, "Error mentions truncated");
    }

    // C. ELF Class Mismatch (32-bit ELF for 64-bit ABI)
    {
        const auto elf32_file = temp_dir / "elf32_mismatch";
        write_synthetic_elf64(elf32_file, 0x03, 1, 1, 2, 1);

        ElfHeaderInspection insp;
        std::string err;
        bool ok = coordinator.validate_elf_for_abi(elf32_file.string(), GuestTargetAbi::X86_64, insp, &err);
        require(!ok, "32-bit ELF fails validation for x86_64 target ABI");
        require(err.find("ELF class mismatch") != std::string::npos, "Error mentions ELF class mismatch");
    }

    // D. Architecture Mismatch (AArch64 machine 0xB7 for x86_64 target)
    {
        const auto aarch64_file = temp_dir / "aarch64_mismatch";
        write_synthetic_elf64(aarch64_file, 0xB7, 2, 1, 3, 1);

        ElfHeaderInspection insp;
        std::string err;
        bool ok = coordinator.validate_elf_for_abi(aarch64_file.string(), GuestTargetAbi::X86_64, insp, &err);
        require(!ok, "AArch64 ELF fails validation for x86_64 target ABI");
        require(err.find("architecture mismatch") != std::string::npos, "Error identifies architecture mismatch");
    }

    // E. Big-Endian Data Encoding Mismatch
    {
        const auto be_file = temp_dir / "be_mismatch";
        write_synthetic_elf64(be_file, 0x3E, 2, 2, 3, 1); // data_encoding = 2 (MSB)

        ElfHeaderInspection insp;
        std::string err;
        bool ok = coordinator.validate_elf_for_abi(be_file.string(), GuestTargetAbi::X86_64, insp, &err);
        require(!ok, "Big-endian ELF fails validation for Android target ABI");
        require(err.find("data encoding mismatch") != std::string::npos, "Error mentions data encoding mismatch");
    }

    // F. Non-Executable Object Type (ET_REL = 1)
    {
        const auto rel_file = temp_dir / "rel_mismatch";
        write_synthetic_elf64(rel_file, 0x3E, 2, 1, 1, 1); // object_type = 1 (ET_REL)

        ElfHeaderInspection insp;
        std::string err;
        bool ok = coordinator.validate_elf_for_abi(rel_file.string(), GuestTargetAbi::X86_64, insp, &err);
        require(!ok, "Relocatable object fails validation (expected executable)");
        require(err.find("object type mismatch") != std::string::npos, "Error identifies object type mismatch");
    }

    // G. Zero Program Headers (Corrupt Executable)
    {
        const auto zero_ph_file = temp_dir / "zero_ph";
        write_synthetic_elf64(zero_ph_file, 0x3E, 2, 1, 3, 0); // phnum = 0

        ElfHeaderInspection insp;
        std::string err;
        bool ok = coordinator.validate_elf_for_abi(zero_ph_file.string(), GuestTargetAbi::X86_64, insp, &err);
        require(!ok, "ELF with 0 program headers fails validation");
        require(err.find("program headers") != std::string::npos, "Error mentions program headers count");
    }

    std::filesystem::remove_all(temp_dir);
    std::cout << "  [PASS] test_elf_header_deep_inspection_and_validation" << std::endl;
}

// ----------------------------------------------------------------------------
// 10. Companion Manifest Validation & Digest Consistency Tests
// ----------------------------------------------------------------------------

void test_artifact_manifest_parsing_validation_and_digest_consistency() {
    std::cout << "  [RUN] test_artifact_manifest_parsing_validation_and_digest_consistency..." << std::endl;

    const auto temp_dir = std::filesystem::temp_directory_path() / "fang_test_manifest_validation";
    std::filesystem::create_directories(temp_dir);

    const auto elf_file = temp_dir / "fang-input-daemon-android-x86_64";
    write_synthetic_elf64(elf_file, 0x3E, 2, 1, 3, 1);

    GuestDeploymentCoordinator coordinator;
    std::string actual_sha256 = coordinator.calculate_file_sha256(elf_file.string());
    require(actual_sha256.size() == 64, "Valid 64-char SHA-256 computed");

    const auto manifest_file = temp_dir / "guest_manifest.json";

    GuestDeploymentTarget target;
    target.abi = GuestTargetAbi::X86_64;
    target.staging_dir = temp_dir.string();

    // A. Valid matching manifest
    {
        GuestArtifactManifest mf;
        mf.artifact_name = "fang-input-daemon-android-x86_64";
        mf.artifact_format = "ELF";
        mf.target_os = "android";
        mf.target_abi = "x86_64";
        mf.is_android_elf_binary = true;
        mf.is_cross_compiled = true;
        mf.build_configuration = "Release";
        mf.artifact_sha256 = actual_sha256;
        mf.artifact_size_bytes = std::filesystem::file_size(elf_file);
        mf.ndk_version = "r26.1.10909125";
        mf.android_api_level = 26;
        mf.build_result = "Success";
        mf.elf_machine = "62";
        mf.elf_class = 2;
        mf.elf_data = 1;
        mf.toolchain_status = ToolchainStatus::ToolchainAvailable;

        std::ofstream out(manifest_file);
        out << mf.to_json();
        out.close();

        auto report = coordinator.assess_deployment_readiness(target);
        require(report.phase == GuestDeploymentPhase::ArtifactReady, "Valid manifest marked ArtifactReady");
        require(report.manifest.artifact_sha256 == actual_sha256, "Manifest SHA-256 preserved in report");
        require(report.manifest.ndk_version == "r26.1.10909125", "NDK version preserved");
    }

    // B. Target ABI Mismatch in Manifest
    {
        GuestArtifactManifest mf;
        mf.artifact_name = "fang-input-daemon-android-arm64-v8a";
        mf.artifact_format = "ELF";
        mf.target_os = "android";
        mf.target_abi = "arm64-v8a"; // Mismatch!
        mf.is_android_elf_binary = true;
        mf.build_configuration = "Release";
        mf.artifact_sha256 = actual_sha256;

        std::ofstream out(manifest_file);
        out << mf.to_json();
        out.close();

        auto report = coordinator.assess_deployment_readiness(target);
        require(report.phase == GuestDeploymentPhase::ArtifactManifestMismatch,
                "Report identifies ArtifactManifestMismatch for ABI mismatch");
        require(report.diagnostic.find("target ABI mismatch") != std::string::npos,
                "Diagnostic details ABI mismatch");
        require(!report.is_ready_for_push, "Not ready for push on manifest mismatch");
    }

    // C. Digest SHA-256 Mismatch in Manifest
    {
        GuestArtifactManifest mf;
        mf.artifact_name = "fang-input-daemon-android-x86_64";
        mf.artifact_format = "ELF";
        mf.target_os = "android";
        mf.target_abi = "x86_64";
        mf.is_android_elf_binary = true;
        mf.build_configuration = "Release";
        mf.artifact_sha256 = "0000000000000000000000000000000000000000000000000000000000000000"; // Fake digest!

        std::ofstream out(manifest_file);
        out << mf.to_json();
        out.close();

        auto report = coordinator.assess_deployment_readiness(target);
        require(report.phase == GuestDeploymentPhase::ArtifactManifestMismatch,
                "Report identifies ArtifactManifestMismatch for digest mismatch");
        require(report.diagnostic.find("digest") != std::string::npos,
                "Diagnostic details digest mismatch");
        require(!report.is_ready_for_push, "Not ready for push on digest mismatch");
    }

    // D. Corrupt JSON in Manifest
    {
        std::ofstream out(manifest_file);
        out << "{ corrupt json content ...";
        out.close();

        auto report = coordinator.assess_deployment_readiness(target);
        require(report.phase == GuestDeploymentPhase::ArtifactManifestMismatch,
                "Corrupt JSON marked as ArtifactManifestMismatch");
        require(report.diagnostic.find("parsing failed") != std::string::npos,
                "Diagnostic indicates JSON parsing failure");
    }

    std::filesystem::remove_all(temp_dir);
    std::cout << "  [PASS] test_artifact_manifest_parsing_validation_and_digest_consistency" << std::endl;
}

// ----------------------------------------------------------------------------
// 11. Synthetic NDK Toolchain Detector Inspection Tests
// ----------------------------------------------------------------------------

void test_guest_toolchain_detector_synthetic_ndk_inspection() {
    std::cout << "  [RUN] test_guest_toolchain_detector_synthetic_ndk_inspection..." << std::endl;

    const auto temp_dir = std::filesystem::temp_directory_path() / "fang_test_ndk_inspection";
    std::filesystem::create_directories(temp_dir);

    // A. Missing source.properties
    {
        auto env = GuestToolchainDetector::inspect_ndk_path(temp_dir.string(), 25);
        require(!env.ndk_found, "Missing source.properties results in ndk_found=false");
        require(env.diagnostic.find("source.properties") != std::string::npos, "Diagnostic mentions missing properties");
    }

    // B. Under-versioned NDK (r23 < minimum r25)
    {
        std::ofstream out(temp_dir / "source.properties");
        out << "Pkg.Desc = Android NDK\n";
        out << "Pkg.Revision = 23.1.7779620\n";
        out.close();

        auto env = GuestToolchainDetector::inspect_ndk_path(temp_dir.string(), 25);
        require(!env.ndk_found, "Under-versioned NDK results in ndk_found=false");
        require(env.diagnostic.find("below minimum") != std::string::npos, "Diagnostic identifies version below minimum");
    }

    // C. Valid NDK version r26, but missing Clang compiler
    {
        std::ofstream out(temp_dir / "source.properties");
        out << "Pkg.Desc = Android NDK\n";
        out << "Pkg.Revision = 26.1.10909125\n";
        out.close();

        auto env = GuestToolchainDetector::inspect_ndk_path(temp_dir.string(), 25);
        require(env.ndk_found, "Valid NDK version results in ndk_found=true");
        require(!env.clang_found, "Missing clang++ results in clang_found=false");
        require(env.ndk_version == "26.1.10909125", "NDK version parsed accurately");
    }

    // D. Valid NDK version r26 with Clang compiler present
    {
        const auto bin_dir = temp_dir / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin";
        std::filesystem::create_directories(bin_dir);
        std::ofstream clang_dummy(bin_dir / "clang++.exe");
        clang_dummy << "mock clang";
        clang_dummy.close();

        auto env = GuestToolchainDetector::inspect_ndk_path(temp_dir.string(), 25);
        require(env.ndk_found, "Valid NDK version results in ndk_found=true");
        require(env.clang_found, "Synthetic clang++ results in clang_found=true");
        require(!env.clang_path.empty(), "Clang path populated");
        require(env.diagnostic.find("ready") != std::string::npos, "Diagnostic marks toolchain ready");
    }

    std::filesystem::remove_all(temp_dir);
    std::cout << "  [PASS] test_guest_toolchain_detector_synthetic_ndk_inspection" << std::endl;
}

// ----------------------------------------------------------------------------
// 12. Studio Session Deployment Diagnostic Integration Tests
// ----------------------------------------------------------------------------

void test_studio_session_deployment_diagnostic_integration() {
    std::cout << "  [RUN] test_studio_session_deployment_diagnostic_integration..." << std::endl;

    auto mock_deployment = std::make_shared<MockGuestDeploymentCoordinator>(
        GuestDeploymentPhase::BlockedMissingToolchain,
        "Android NDK r25+ missing; cross-compilation blocked.");
    auto forwarder = std::make_shared<AdbPortForwarder>(static_cast<uint16_t>(24800));

    StudioSessionController controller(
        nullptr, nullptr, nullptr, nullptr,
        nullptr, forwarder, nullptr, mock_deployment);

    SupervisorRuntimeSnapshot starting_snap;
    starting_snap.runtime.state = fang::broker::RuntimeLifecycleState::Starting;
    starting_snap.runtime.generation_id = "gen-diag-1";
    starting_snap.runtime.process_id = 1234;
    starting_snap.runtime.adb_serial = "emulator-5554";

    auto snap = controller.update(starting_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
    require(snap.deployment_phase == GuestDeploymentPhase::BlockedMissingToolchain,
            "Deployment phase reflected in snapshot during Starting");
    require(snap.deployment_status_line().find(L"Missing Toolchain") != std::wstring::npos,
            "Deployment status line identifies missing toolchain");
    require(snap.user_status_line().find(L"[DEPLOYMENT BLOCKED: Missing Toolchain]") != std::wstring::npos,
            "User status line surfaces deployment blocker");

    // Supervisor transitions to Ready: bridge must be safely Blocked because deployment is not ready
    SupervisorRuntimeSnapshot ready_snap = starting_snap;
    ready_snap.runtime.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_snap.runtime.guest_ready = true;

    snap = controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
    require(snap.bridge_state == StudioBridgeState::Blocked,
            "Bridge state is Blocked when deployment is not ready");
    require(snap.bridge_diagnostic.find("guest daemon deployment is not ready") != std::string::npos,
            "Bridge diagnostic explains that deployment is unready");

    // Test ArtifactManifestMismatch diagnosis in Studio session
    mock_deployment->set_phase(GuestDeploymentPhase::ArtifactManifestMismatch);
    mock_deployment->set_diagnostic("SHA-256 digest mismatch between artifact and manifest.");

    snap = controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
    require(snap.deployment_phase == GuestDeploymentPhase::ArtifactManifestMismatch,
            "Snapshot reflects ArtifactManifestMismatch");
    require(snap.deployment_status_line().find(L"Manifest Mismatch") != std::wstring::npos,
            "Status line details Manifest Mismatch");
    require(snap.user_status_line().find(L"[DEPLOYMENT BLOCKED: Manifest Mismatch]") != std::wstring::npos,
            "User status line alerts Manifest Mismatch");

    // Test ArtifactCorrupt diagnosis in Studio session
    mock_deployment->set_phase(GuestDeploymentPhase::ArtifactCorrupt);
    mock_deployment->set_diagnostic("Artifact truncated or invalid header.");

    snap = controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
    require(snap.deployment_phase == GuestDeploymentPhase::ArtifactCorrupt,
            "Snapshot reflects ArtifactCorrupt");
    require(snap.deployment_status_line().find(L"Artifact Corrupt") != std::wstring::npos,
            "Status line details Artifact Corrupt");
    require(snap.user_status_line().find(L"[DEPLOYMENT BLOCKED: Artifact Corrupt]") != std::wstring::npos,
            "User status line alerts Artifact Corrupt");

    // Transition mock deployment to Deployed
    mock_deployment->set_phase(GuestDeploymentPhase::Deployed);
    mock_deployment->set_diagnostic("Daemon running in guest.");

    snap = controller.update(ready_snap, ViewportBackend::Direct3D11, 0, {"emulator-5554"});
    require(snap.deployment_phase == GuestDeploymentPhase::Deployed, "Deployment phase updated to Deployed");
    require(snap.deployment_status_line() == L"Guest Deployment: Deployed", "Status line reports Deployed");

    controller.notify_shutdown();
    snap = controller.snapshot();
    require(snap.deployment_phase == GuestDeploymentPhase::Unconfigured, "Deployment unconfigured on shutdown");

    std::cout << "  [PASS] test_studio_session_deployment_diagnostic_integration" << std::endl;
}

// ----------------------------------------------------------------------------
// 13. Provenance Assessment Tests
// ----------------------------------------------------------------------------

void test_provenance_assessment() {
    std::cout << "  [RUN] test_provenance_assessment..." << std::endl;

    GuestDeploymentCoordinator coordinator;
    GuestDeploymentTarget target;
    target.abi = GuestTargetAbi::X86_64;

    // Create a temp directory for testing
    const auto temp_dir = std::filesystem::temp_directory_path() / "fang_test_provenance";
    std::filesystem::create_directories(temp_dir);

    const auto elf_file = temp_dir / "fang-input-daemon-android-x86_64";
    write_synthetic_elf64(elf_file, 0x3E, 2, 1, 3, 1);

    target.staging_dir = temp_dir.string();

    // Test 1: Legacy manifest (stage9-v1) should return LegacyUnverified
    {
        GuestArtifactManifest mf;
        mf.artifact_name = "fang-input-daemon-android-x86_64";
        mf.artifact_format = "ELF";
        mf.target_os = "android";
        mf.target_abi = "x86_64";
        mf.is_android_elf_binary = true;
        mf.is_cross_compiled = true;
        mf.build_configuration = "Release";
        mf.manifest_schema_version = "stage9-v1"; // Legacy version
        mf.build_result = "Success";
        mf.artifact_sha256 = coordinator.calculate_file_sha256(elf_file.string());
        mf.elf_machine = "62";
        mf.elf_class = 2;
        mf.elf_data = 1;
        mf.toolchain_status = ToolchainStatus::ToolchainAvailable;

        const auto manifest_file = temp_dir / "guest_manifest.json";
        std::ofstream out(manifest_file);
        out << mf.to_json();
        out.close();

        auto report = coordinator.assess_deployment_readiness(target);
        require(report.provenance_assessment == ProvenanceAssessment::LegacyUnverified,
                "Legacy manifest should return LegacyUnverified");
    }

    // Test 2: Manifest with source_evidence but current evidence unavailable should return Unavailable
    // (This is hard to test without a git repo, so we skip the full test)

    // Test 3: Clean source fingerprint match should return QualifiedCurrent
    // Test 4: Dirty source fingerprint match should return QualifiedDirty
    // Test 5: Mismatched fingerprint should return Stale

    std::filesystem::remove_all(temp_dir);
    std::cout << "  [PASS] test_provenance_assessment" << std::endl;
}

void test_source_evidence_serialization() {
    std::cout << "  [RUN] test_source_evidence_serialization..." << std::endl;

    using namespace fang::guest;

    // Test SourceEvidence serialization
    SourceEvidence evidence;
    evidence.status = SourceEvidence::Status::Collected;
    evidence.algorithm = "sha256";
    evidence.fingerprint = "abc123";
    evidence.scope = "build-inputs-v1";
    evidence.captured_at_utc = "2026-10-18T12:00:00Z";
    evidence.unavailable_reasons = {"reason1", "reason2"};

    GuestArtifactManifest manifest;
    manifest.source_evidence = evidence;
    manifest.artifact_name = "test";
    manifest.artifact_format = "ELF";
    manifest.target_os = "android";
    manifest.target_abi = "x86_64";
    manifest.manifest_schema_version = "stage9-v2";

    std::string json = manifest.to_json();
    require(json.find("\"source_evidence\":") != std::string::npos, "JSON contains source_evidence");
    require(json.find("\"status\": \"collected\"") != std::string::npos, "JSON contains status");
    require(json.find("\"algorithm\": \"sha256\"") != std::string::npos, "JSON contains algorithm");
    require(json.find("\"fingerprint\": \"abc123\"") != std::string::npos, "JSON contains fingerprint");
    require(json.find("\"scope\": \"build-inputs-v1\"") != std::string::npos, "JSON contains scope");
    require(json.find("\"captured_at_utc\": \"2026-10-18T12:00:00Z\"") != std::string::npos, "JSON contains timestamp");
    require(json.find("\"reason1\"") != std::string::npos, "JSON contains unavailable_reasons");

    // Test round-trip
    GuestArtifactManifest parsed;
    std::string err;
    require(GuestArtifactManifest::from_json(json, parsed, &err), "from_json succeeds: " + err);
    require(parsed.source_evidence.is_collected(), "Parsed evidence is collected");
    require(parsed.source_evidence.algorithm == "sha256", "Parsed algorithm matches");
    require(parsed.source_evidence.fingerprint == "abc123", "Parsed fingerprint matches");
    require(parsed.source_evidence.scope == "build-inputs-v1", "Parsed scope matches");
    require(parsed.source_evidence.captured_at_utc == "2026-10-18T12:00:00Z", "Parsed timestamp matches");
    require(parsed.source_evidence.unavailable_reasons.size() == 2, "Parsed unavailable_reasons count matches");

    // Test unavailable status
    SourceEvidence unavailable_evidence;
    unavailable_evidence.status = SourceEvidence::Status::Unavailable;
    unavailable_evidence.unavailable_reasons = {"git failed"};

    GuestArtifactManifest manifest2;
    manifest2.source_evidence = unavailable_evidence;
    manifest2.artifact_name = "test";
    manifest2.artifact_format = "ELF";
    manifest2.target_os = "android";
    manifest2.target_abi = "x86_64";
    manifest2.manifest_schema_version = "stage9-v2";

    std::string json2 = manifest2.to_json();
    require(json2.find("\"status\": \"unavailable\"") != std::string::npos, "JSON contains unavailable status");

    GuestArtifactManifest parsed2;
    require(GuestArtifactManifest::from_json(json2, parsed2, &err), "from_json succeeds for unavailable");
    require(!parsed2.source_evidence.is_collected(), "Parsed evidence is not collected");

    std::cout << "  [PASS] test_source_evidence_serialization" << std::endl;
}

} // namespace

int main() {
#ifdef _WIN32
    // Suppress crash popups/WER in automated tests to prevent hangs
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif

    try {
        std::cout << "[FANG-STAGE9-TESTS] Running Stage 9 integration and deployment tests..." << std::endl;

        test_forwarder_abstractions();
        test_guest_manifest_and_toolchain_detector();
        test_studio_session_integrated_lifecycle();
        test_blocked_reachability_and_generation_reset();
        test_peer_disconnect_and_error_reset();
        test_guest_artifact_readiness_toolchain_missing();
        test_guest_artifact_readiness_pe_vs_elf_mismatch();
        test_guest_artifact_readiness_valid_elf_and_offline_policy();
        test_elf_header_deep_inspection_and_validation();
        test_artifact_manifest_parsing_validation_and_digest_consistency();
        test_guest_toolchain_detector_synthetic_ndk_inspection();
        test_studio_session_deployment_diagnostic_integration();
        test_provenance_assessment();
        test_source_evidence_serialization();

        std::cout << "[FANG-STAGE9-TESTS] All Stage 9 tests passed successfully." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FANG-STAGE9-TESTS] FAILED: " << ex.what() << std::endl;
        return 1;
    }
}
