#include "guest_input_backend.hpp"
#include "guest_input_daemon.hpp"
#include "guest_input_bridge.hpp"
#include "fake_guest_input_endpoint.hpp"
#include "input_ipc_protocol.hpp"
#include "input_transport.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace fang::guest;
using namespace fang::studio;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_daemon_initialization_and_backend_selection() {
    // 1. Recording backend
    auto recording = std::make_shared<RecordingGuestInputBackend>();
    require(!recording->is_ready(), "Recording backend initially not ready.");
    require(recording->initialize(1080, 1920, 10), "Recording initialize should succeed.");
    require(recording->is_ready(), "Recording backend is ready after init.");
    require(recording->backend_name() == "recording", "Backend name is recording.");
    require(recording->supported_capabilities() == 0x07, "Capabilities are Touch|Key|PointerAim.");
    recording->shutdown();
    require(!recording->is_ready(), "Not ready after shutdown.");

    // 2. Linux uinput backend
    auto uinput = std::make_shared<LinuxUinputGuestInputBackend>();
    require(uinput->backend_name() == "linux-uinput", "Backend name is linux-uinput.");
#if defined(__linux__) || defined(__ANDROID__)
    // On real Linux without root, /dev/uinput open may fail safely
    uinput->initialize(1080, 1920, 10);
#else
    // On Windows MSVC host, it reports unavailable cleanly
    require(!uinput->initialize(1080, 1920, 10), "Uinput must report false on host OS.");
    require(!uinput->is_ready(), "Uinput not ready on non-Linux.");
    require(uinput->status_diagnostic().find("Linux /dev/uinput backend is not available") != std::string::npos,
        "Diagnostic message must explain lack of Linux kernel.");
#endif

    // 3. Unavailable backend
    auto unavail = std::make_shared<UnavailableGuestInputBackend>("Test unavailable reason");
    require(!unavail->initialize(1080, 1920, 10), "Unavailable returns false.");
    require(!unavail->is_ready(), "Unavailable is not ready.");
    require(unavail->backend_name() == "unavailable", "Backend name is unavailable.");
    require(unavail->status_diagnostic() == "Test unavailable reason", "Diagnostic matches.");
}

void test_daemon_hello_negotiation_and_capabilities() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();

    GuestInputDaemon::Config cfg;
    cfg.expected_session_id = "sess-alpha";
    cfg.expected_generation_id = "gen-1";
    cfg.max_contacts = 10;
    GuestInputDaemon daemon(guest_endpoint, backend, cfg);

    require(daemon.start(), "Daemon start should succeed.");
    require(daemon.state() == GuestInputDaemon::DaemonState::AwaitingHello,
        "Daemon state should be AwaitingHello.");

    // Client sends valid Hello
    HelloPayload hello{input_protocol_version, 1080, 1920, "sess-alpha", "gen-1"};
    auto hello_pkt = encode_hello_message(1, 100, hello);
    transport->send_bytes(hello_pkt);

    std::size_t processed = daemon.poll_and_process();
    require(processed == 1, "Daemon should process 1 message.");
    require(daemon.state() == GuestInputDaemon::DaemonState::Active,
        "Daemon should transition to Active after Hello.");
    require(backend->is_ready(), "Backend should be initialized.");
    require(daemon.negotiated_max_contacts() == 10, "Negotiated contacts should be 10.");

    // Verify HelloAck sent back
    auto guest_bytes = transport->receive_bytes(4096);
    require(guest_bytes.size() >= input_wire_header_size, "Should receive HelloAck.");
    WireHeader wh;
    auto hdr_err = decode_wire_header(guest_bytes, wh);
    require(hdr_err == InputProtocolError::None && wh.type == InputMessageType::HelloAck,
        "Must be valid HelloAck.");

    std::span<const std::byte> payload(guest_bytes.data() + input_wire_header_size, wh.payload_size);
    InputProtocolError parse_err = InputProtocolError::None;
    auto ack = decode_hello_ack_message(payload, parse_err);
    require(ack.has_value() && ack->status == InputProtocolError::None, "HelloAck status None.");
    require(ack->max_contacts == 10, "Max contacts 10.");
    require(ack->session_id == "sess-alpha", "Session ID matches.");
    require(ack->generation_id == "gen-1", "Generation ID matches.");
}

void test_daemon_session_and_generation_mismatch() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();

    GuestInputDaemon::Config cfg;
    cfg.expected_session_id = "sess-main";
    cfg.expected_generation_id = "gen-target";
    GuestInputDaemon daemon(guest_endpoint, backend, cfg);
    daemon.start();

    // 1. Generation mismatch
    HelloPayload bad_gen{input_protocol_version, 1080, 1920, "sess-main", "gen-stale"};
    auto pkt1 = encode_hello_message(1, 100, bad_gen);
    transport->send_bytes(pkt1);
    daemon.poll_and_process();

    require(daemon.state() == GuestInputDaemon::DaemonState::Error, "Should enter Error state.");
    require(daemon.last_error() == InputProtocolError::StaleGeneration, "Error must be StaleGeneration.");

    auto resp1 = transport->receive_bytes(4096);
    require(resp1.size() >= input_wire_header_size, "Response received.");
    WireHeader wh1;
    decode_wire_header(resp1, wh1);
    std::span<const std::byte> payload1(resp1.data() + input_wire_header_size, wh1.payload_size);
    InputProtocolError err = InputProtocolError::None;
    auto ack1 = decode_hello_ack_message(payload1, err);
    require(ack1.has_value() && ack1->status == InputProtocolError::StaleGeneration,
        "Ack reports StaleGeneration.");

    // 2. Session mismatch
    daemon.shutdown();
    transport->clear_buffers();
    daemon.start();

    HelloPayload bad_sess{input_protocol_version, 1080, 1920, "sess-foreign", "gen-target"};
    auto pkt2 = encode_hello_message(2, 101, bad_sess);
    transport->send_bytes(pkt2);
    daemon.poll_and_process();

    require(daemon.state() == GuestInputDaemon::DaemonState::Error, "Should enter Error state.");
    require(daemon.last_error() == InputProtocolError::SessionMismatch, "Error must be SessionMismatch.");
}

void test_daemon_partial_reads_and_writes() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();

    GuestInputDaemon::Config cfg;
    cfg.expected_session_id = "sess-partial";
    cfg.expected_generation_id = "gen-partial";
    GuestInputDaemon daemon(guest_endpoint, backend, cfg);
    daemon.start();

    HelloPayload hello{input_protocol_version, 1080, 1920, "sess-partial", "gen-partial"};
    auto full_pkt = encode_hello_message(1, 100, hello);

    // Feed in tiny chunks of 7 bytes
    std::size_t offset = 0;
    while (offset < full_pkt.size()) {
        std::size_t chunk_size = std::min<std::size_t>(7, full_pkt.size() - offset);
        std::span<const std::byte> chunk(full_pkt.data() + offset, chunk_size);
        transport->send_bytes(chunk);
        offset += chunk_size;

        daemon.poll_and_process();
        if (offset < full_pkt.size()) {
            require(daemon.state() == GuestInputDaemon::DaemonState::AwaitingHello,
                "Must wait until complete packet arrives.");
        }
    }

    require(daemon.state() == GuestInputDaemon::DaemonState::Active,
        "Daemon should activate once full packet is received.");
}

void test_daemon_truncated_oversized_malformed_packets() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon daemon(guest_endpoint, backend);
    daemon.start();

    // Oversized payload header (> 4096 bytes)
    WireHeader wh;
    wh.magic = input_protocol_magic;
    wh.version = input_protocol_version;
    wh.type = InputMessageType::TouchFrame;
    wh.payload_size = 9999;
    auto bad_hdr = encode_wire_header(wh);
    transport->send_bytes(bad_hdr);

    daemon.poll_and_process();
    // Daemon should reject with OversizedPayload error
    auto resp = transport->receive_bytes(4096);
    require(resp.size() >= input_wire_header_size, "Error response sent.");
    WireHeader err_wh;
    decode_wire_header(resp, err_wh);
    require(err_wh.type == InputMessageType::Error, "Must send Error packet.");
    std::span<const std::byte> p_span(resp.data() + input_wire_header_size, err_wh.payload_size);
    InputProtocolError err = InputProtocolError::None;
    auto err_msg = decode_error_message(p_span, err);
    require(err_msg.has_value() && err_msg->error_code == InputProtocolError::OversizedPayload,
        "Error must be OversizedPayload.");

    // Unsupported version header
    transport->clear_buffers();
    wh.version = 99;
    wh.payload_size = 0;
    auto ver_hdr = encode_wire_header(wh);
    transport->send_bytes(ver_hdr);
    daemon.poll_and_process();

    require(daemon.last_error() == InputProtocolError::UnsupportedVersion,
        "Error must be UnsupportedVersion.");
}

void test_daemon_invalid_message_types_and_coordinates() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon daemon(guest_endpoint, backend);
    daemon.start();

    // Handshake first
    HelloPayload hello{input_protocol_version, 1080, 1920, "default_session", "gen-1"};
    transport->send_bytes(encode_hello_message(1, 100, hello));
    daemon.poll_and_process();
    transport->clear_buffers();

    // 1. TouchFrame with invalid coordinates (norm_u = 2.5f)
    TouchFramePayload tf1;
    WireTouchContact c1{};
    c1.contact_id = 0;
    c1.transition = 0;
    c1.norm_u = 2.5f; // invalid out of bounds
    c1.norm_v = 0.5f;
    tf1.contacts.push_back(c1);
    transport->send_bytes(encode_touch_frame_message(2, 101, tf1));
    daemon.poll_and_process();

    require(daemon.last_error() == InputProtocolError::InvalidCoordinates,
        "Error must be InvalidCoordinates.");

    // 2. TouchFrame with invalid contact ID (15)
    transport->clear_buffers();
    TouchFramePayload tf2;
    WireTouchContact c2{};
    c2.contact_id = 15; // invalid! max 9
    c2.transition = 0;
    c2.norm_u = 0.5f;
    c2.norm_v = 0.5f;
    tf2.contacts.push_back(c2);
    transport->send_bytes(encode_touch_frame_message(3, 102, tf2));
    daemon.poll_and_process();

    require(daemon.last_error() == InputProtocolError::InvalidContactId,
        "Error must be InvalidContactId.");
}

void test_daemon_sequence_numbers_duplicates_gaps() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon daemon(guest_endpoint, backend);
    daemon.start();

    // Handshake
    HelloPayload hello{input_protocol_version, 1080, 1920, "default_session", "gen-1"};
    transport->send_bytes(encode_hello_message(1, 100, hello));
    daemon.poll_and_process();
    transport->clear_buffers();

    TouchFramePayload tf;
    WireTouchContact c{};
    c.contact_id = 0;
    c.transition = 0;
    c.norm_u = 0.5f;
    c.norm_v = 0.5f;
    tf.contacts.push_back(c);

    // Frame with sequence 2
    transport->send_bytes(encode_touch_frame_message(2, 110, tf));
    daemon.poll_and_process();
    require(daemon.last_received_sequence() == 2, "Seq 2 received.");
    require(daemon.gap_seq_count() == 0, "No gap yet.");

    // Frame with sequence 10 (gap of 8)
    transport->send_bytes(encode_touch_frame_message(10, 120, tf));
    daemon.poll_and_process();
    require(daemon.last_received_sequence() == 10, "Seq 10 received.");
    require(daemon.gap_seq_count() == 1, "Gap detected.");

    // Duplicate frame with sequence 5 (older than last received 10)
    transport->send_bytes(encode_touch_frame_message(5, 130, tf));
    daemon.poll_and_process();
    require(daemon.duplicate_seq_count() == 1, "Duplicate detected.");
}

void test_daemon_multi_touch_transitions_and_active_tracking() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon daemon(guest_endpoint, backend);
    daemon.start();

    HelloPayload hello{input_protocol_version, 1080, 1920, "default_session", "gen-1"};
    transport->send_bytes(encode_hello_message(1, 100, hello));
    daemon.poll_and_process();
    transport->clear_buffers();

    // 1. Send 2 simultaneous contacts Down (slots 0 and 1)
    TouchFramePayload tf1;
    WireTouchContact c0{0, 0, {0,0,0}, 0.2f, 0.3f, 216, 576};
    WireTouchContact c1{1, 0, {0,0,0}, 0.7f, 0.8f, 756, 1536};
    tf1.contacts = {c0, c1};
    transport->send_bytes(encode_touch_frame_message(2, 101, tf1));
    daemon.poll_and_process();

    require(daemon.active_contact_count() == 2, "2 contacts active.");
    require(daemon.active_contact_mask() == 0x03, "Mask is 0x03.");
    require(backend->active_contact_count() == 2, "Backend tracks 2 contacts.");
    require(backend->sync_count() == 1, "Backend received 1 sync.");

    // 2. Move contact 0, Up contact 1
    TouchFramePayload tf2;
    c0.transition = 1; // Move
    c0.norm_u = 0.25f;
    c1.transition = 2; // Up
    tf2.contacts = {c0, c1};
    transport->send_bytes(encode_touch_frame_message(3, 102, tf2));
    daemon.poll_and_process();

    require(daemon.active_contact_count() == 1, "1 contact active after Up.");
    require(daemon.active_contact_mask() == 0x01, "Mask is 0x01.");
    require(daemon.is_contact_active(0), "Contact 0 active.");
    require(!daemon.is_contact_active(1), "Contact 1 inactive.");
    require(backend->sync_count() == 2, "Backend received 2 syncs.");
}

void test_daemon_key_events_and_explicit_release() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon daemon(guest_endpoint, backend);
    daemon.start();

    HelloPayload hello{input_protocol_version, 1080, 1920, "default_session", "gen-1"};
    transport->send_bytes(encode_hello_message(1, 100, hello));
    daemon.poll_and_process();

    // Key Down: ENTER (66)
    transport->send_bytes(encode_key_event_message(2, 101, KeyEventPayload{66, true}));
    daemon.poll_and_process();
    require(daemon.held_key_count() == 1, "1 key held.");
    require(backend->is_key_held(66), "Backend tracks ENTER held.");

    // Key Down: SPACE (62)
    transport->send_bytes(encode_key_event_message(3, 102, KeyEventPayload{62, true}));
    daemon.poll_and_process();
    require(daemon.held_key_count() == 2, "2 keys held.");

    // Key Up: ENTER (66)
    transport->send_bytes(encode_key_event_message(4, 103, KeyEventPayload{66, false}));
    daemon.poll_and_process();
    require(daemon.held_key_count() == 1, "1 key held.");
    require(!backend->is_key_held(66), "ENTER released.");
    require(backend->is_key_held(62), "SPACE still held.");

    // Teardown with active held keys must explicitly clear them
    daemon.shutdown();
    require(daemon.held_key_count() == 0, "All held keys cleared on shutdown.");
    require(!backend->is_key_held(62), "SPACE released by backend on shutdown.");
}

void test_daemon_cancel_all_held_contacts_and_keys() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon daemon(guest_endpoint, backend);
    daemon.start();

    HelloPayload hello{input_protocol_version, 1080, 1920, "default_session", "gen-1"};
    transport->send_bytes(encode_hello_message(1, 100, hello));
    daemon.poll_and_process();

    // Hold contacts 0, 1, 2
    TouchFramePayload tf;
    tf.contacts = {
        WireTouchContact{0, 0, {0,0,0}, 0.1f, 0.1f, 100, 100},
        WireTouchContact{1, 0, {0,0,0}, 0.2f, 0.2f, 200, 200},
        WireTouchContact{2, 0, {0,0,0}, 0.3f, 0.3f, 300, 300}
    };
    transport->send_bytes(encode_touch_frame_message(2, 101, tf));
    daemon.poll_and_process();
    require(daemon.active_contact_count() == 3, "3 contacts active.");

    // Hold key 19 (DPAD_UP)
    transport->send_bytes(encode_key_event_message(3, 102, KeyEventPayload{19, true}));
    daemon.poll_and_process();
    require(daemon.held_key_count() == 1, "1 key held.");

    // Send CancelAll
    transport->send_bytes(encode_cancel_all_message(4, 103, CancelAllPayload{1 /*Loss of focus*/}));
    daemon.poll_and_process();

    require(daemon.active_contact_count() == 0, "Active contacts cleared.");
    require(daemon.active_contact_mask() == 0, "Contact mask 0.");
    require(daemon.held_key_count() == 0, "Held keys cleared.");
    require(backend->active_contact_count() == 0, "Backend slots cleared.");
    require(!backend->is_key_held(19), "Backend key cleared.");
    require(daemon.total_cancels_processed() == 1, "Total cancels 1.");
}

void test_daemon_heartbeat_timeout_and_peer_disconnect() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();

    GuestInputDaemon::Config cfg;
    cfg.heartbeat_timeout_ms = 5000;
    GuestInputDaemon daemon(guest_endpoint, backend, cfg);
    daemon.start();

    HelloPayload hello{input_protocol_version, 1080, 1920, "default_session", "gen-1"};
    transport->send_bytes(encode_hello_message(1, 100, hello));
    daemon.poll_and_process(100);
    require(daemon.is_active(), "Daemon is active.");

    // Add active touch
    TouchFramePayload tf;
    tf.contacts.push_back(WireTouchContact{0, 0, {0,0,0}, 0.5f, 0.5f, 500, 500});
    transport->send_bytes(encode_touch_frame_message(2, 200, tf));
    daemon.poll_and_process(200);
    require(daemon.active_contact_count() == 1, "1 contact active.");

    // Simulate 6000 ms elapsed with no peer packets
    daemon.poll_and_process(200 + 6000);
    require(!daemon.is_active(), "Daemon timed out and is no longer active.");
    require(daemon.state() == GuestInputDaemon::DaemonState::Disconnected, "State Disconnected.");
    require(daemon.active_contact_count() == 0, "Contacts canceled on timeout.");
    require(backend->active_contact_count() == 0, "Backend cleared on timeout.");
}

void test_daemon_backend_failure_handling() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon daemon(guest_endpoint, backend);
    daemon.start();

    HelloPayload hello{input_protocol_version, 1080, 1920, "default_session", "gen-1"};
    transport->send_bytes(encode_hello_message(1, 100, hello));
    daemon.poll_and_process();
    transport->clear_buffers();

    // Trigger backend injection failure
    backend->set_fail_on_sync(true);

    TouchFramePayload tf;
    tf.contacts.push_back(WireTouchContact{0, 0, {0,0,0}, 0.5f, 0.5f, 500, 500});
    transport->send_bytes(encode_touch_frame_message(2, 101, tf));
    daemon.poll_and_process();

    // Daemon does not crash; it emits TouchAck with QueueOverflow/failure status
    auto ack_bytes = transport->receive_bytes(4096);
    require(ack_bytes.size() >= input_wire_header_size, "Ack received.");
    WireHeader wh;
    decode_wire_header(ack_bytes, wh);
    std::span<const std::byte> p_span(ack_bytes.data() + input_wire_header_size, wh.payload_size);
    InputProtocolError err = InputProtocolError::None;
    auto tack = decode_touch_ack_message(p_span, err);
    require(tack.has_value(), "TouchAck decoded.");
    require(tack->status == InputProtocolError::QueueOverflow,
        "TouchAck reports QueueOverflow when backend fails.");
}

void test_daemon_queue_capacity_and_overflow_recovery() {
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();

    GuestInputDaemon::Config cfg;
    cfg.max_buffer_bytes = 128; // very small capacity
    GuestInputDaemon daemon(guest_endpoint, backend, cfg);
    daemon.start();

    // Flood transport with 200 bytes of dummy data
    std::vector<std::byte> flood(200, std::byte{0xFF});
    transport->send_bytes(flood);
    daemon.poll_and_process();

    require(daemon.last_error() == InputProtocolError::QueueOverflow,
        "Buffer overflow detected.");

    // Send valid Ping afterwards: daemon must recover cleanly
    transport->send_bytes(encode_ping_message(1, 100, PingPayload{0x12345678}));
    daemon.poll_and_process();

    auto resp = transport->receive_bytes(4096);
    require(resp.size() >= input_wire_header_size, "Pong response received after recovery.");
    WireHeader wh;
    decode_wire_header(resp, wh);
    require(wh.type == InputMessageType::Pong, "Must be Pong message.");
}

void test_host_bridge_conformance_with_daemon() {
    // Connect real GuestInputBridgeClient to real GuestInputDaemon over LoopbackInputTransport!
    auto transport = std::make_shared<LoopbackInputTransport>();
    auto guest_endpoint = std::make_shared<LoopbackGuestEndpoint>(transport);
    auto backend = std::make_shared<RecordingGuestInputBackend>();

    GuestInputDaemon::Config d_cfg;
    d_cfg.expected_session_id = "conformance_sess";
    d_cfg.expected_generation_id = "gen-conf";
    d_cfg.max_contacts = 10;
    auto daemon = std::make_shared<GuestInputDaemon>(guest_endpoint, backend, d_cfg);
    daemon->start();

    GuestInputBridgeClient::Config b_cfg;
    b_cfg.session_id = "conformance_sess";
    b_cfg.generation_id = "gen-conf";
    b_cfg.display_width = 1080;
    b_cfg.display_height = 1920;
    auto bridge = std::make_shared<GuestInputBridgeClient>(transport, b_cfg);

    // 1. Handshake & Capability Negotiation
    require(bridge->start(), "Bridge start.");
    daemon->poll_and_process();
    bridge->poll_incoming_acks();

    require(bridge->is_connected(), "Bridge is connected.");
    require(daemon->is_active(), "Daemon is active.");
    require(bridge->capability() == MultiTouchBackendCapability::Supported, "Capability Supported.");
    require(bridge->negotiated_max_contacts() == 10, "10 contacts negotiated.");

    // 2. Multi-touch delivery: Down contacts 0 and 1
    TouchContactState c0;
    c0.contact_id = 0;
    c0.is_active = true;
    c0.start_time_ms = 1000;
    c0.last_move_time_ms = 1000;
    c0.norm_pos = {0.2f, 0.3f};
    c0.guest_x = 216;
    c0.guest_y = 576;

    TouchContactState c1;
    c1.contact_id = 1;
    c1.is_active = true;
    c1.start_time_ms = 1000;
    c1.last_move_time_ms = 1000;
    c1.norm_pos = {0.7f, 0.8f};
    c1.guest_x = 756;
    c1.guest_y = 1536;

    require(bridge->send_touch_frame({c0, c1}), "Send touch frame.");
    daemon->poll_and_process();
    bridge->poll_incoming_acks();

    require(bridge->total_sent_count() == 1, "1 sent.");
    require(bridge->total_acked_count() == 1, "1 acked.");
    require(daemon->active_contact_count() == 2, "Daemon tracks 2 contacts.");
    require(backend->active_contact_count() == 2, "Backend has 2 contacts.");

    // 3. Key event delivery
    require(bridge->send_key_event(66 /*ENTER*/, true), "Send key ENTER.");
    daemon->poll_and_process();
    bridge->poll_incoming_acks();

    require(bridge->total_sent_count() == 2, "2 sent.");
    require(bridge->total_acked_count() == 2, "2 acked.");
    require(backend->is_key_held(66), "Backend has ENTER held.");

    // 4. CancelAll delivery
    require(bridge->cancel_all_contacts(1), "Send CancelAll.");
    daemon->poll_and_process();
    bridge->poll_incoming_acks();

    require(daemon->active_contact_count() == 0, "Daemon contacts cleared.");
    require(daemon->held_key_count() == 0, "Daemon keys cleared.");
    require(backend->active_contact_count() == 0, "Backend contacts cleared.");
    require(!backend->is_key_held(66), "Backend key cleared.");

    // 5. Symmetric protocol behavior check: compare with FakeGuestInputEndpoint
    auto mock_transport = std::make_shared<LoopbackInputTransport>();
    FakeGuestInputEndpoint mock_endpoint(mock_transport, "conformance_sess", "gen-conf", 1080, 1920);
    auto mock_bridge = std::make_shared<GuestInputBridgeClient>(mock_transport, b_cfg);
    mock_bridge->start();
    mock_endpoint.poll_and_process();
    mock_bridge->poll_incoming_acks();

    require(mock_bridge->is_connected() == bridge->is_connected(),
        "Bridge connection state conforms between daemon and mock.");
    require(mock_bridge->negotiated_max_contacts() == bridge->negotiated_max_contacts(),
        "Negotiated contacts conform between daemon and mock.");
    require(mock_bridge->capability() == bridge->capability(),
        "Capability conforms between daemon and mock.");
}

} // namespace

int main() {
    try {
        std::cout << "Starting guest daemon tests...\n" << std::flush;

        std::cout << "1. test_daemon_initialization_and_backend_selection\n" << std::flush;
        test_daemon_initialization_and_backend_selection();

        std::cout << "2. test_daemon_hello_negotiation_and_capabilities\n" << std::flush;
        test_daemon_hello_negotiation_and_capabilities();

        std::cout << "3. test_daemon_session_and_generation_mismatch\n" << std::flush;
        test_daemon_session_and_generation_mismatch();

        std::cout << "4. test_daemon_partial_reads_and_writes\n" << std::flush;
        test_daemon_partial_reads_and_writes();

        std::cout << "5. test_daemon_truncated_oversized_malformed_packets\n" << std::flush;
        test_daemon_truncated_oversized_malformed_packets();

        std::cout << "6. test_daemon_invalid_message_types_and_coordinates\n" << std::flush;
        test_daemon_invalid_message_types_and_coordinates();

        std::cout << "7. test_daemon_sequence_numbers_duplicates_gaps\n" << std::flush;
        test_daemon_sequence_numbers_duplicates_gaps();

        std::cout << "8. test_daemon_multi_touch_transitions_and_active_tracking\n" << std::flush;
        test_daemon_multi_touch_transitions_and_active_tracking();

        std::cout << "9. test_daemon_key_events_and_explicit_release\n" << std::flush;
        test_daemon_key_events_and_explicit_release();

        std::cout << "10. test_daemon_cancel_all_held_contacts_and_keys\n" << std::flush;
        test_daemon_cancel_all_held_contacts_and_keys();

        std::cout << "11. test_daemon_heartbeat_timeout_and_peer_disconnect\n" << std::flush;
        test_daemon_heartbeat_timeout_and_peer_disconnect();

        std::cout << "12. test_daemon_backend_failure_handling\n" << std::flush;
        test_daemon_backend_failure_handling();

        std::cout << "13. test_daemon_queue_capacity_and_overflow_recovery\n" << std::flush;
        test_daemon_queue_capacity_and_overflow_recovery();

        std::cout << "14. test_host_bridge_conformance_with_daemon\n" << std::flush;
        test_host_bridge_conformance_with_daemon();

        std::cout << "All guest input daemon core, input backend, protocol conformance, and offline integration tests passed successfully.\n" << std::flush;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Guest daemon test failed: " << e.what() << "\n";
        return 1;
    }
}
