#include "guest_input_backend.hpp"
#include "guest_input_daemon.hpp"
#include "guest_input_bridge.hpp"
#include "input_ipc_protocol.hpp"
#include "socket_input_transport.hpp"

#include <chrono>
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

// Helper to poll until condition or timeout
template <typename F>
bool poll_until(F&& condition, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000), std::chrono::milliseconds step = std::chrono::milliseconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(step);
    }
    return condition();
}

void test_socket_transport_server_client_lifecycle() {
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Server must bind ephemeral port.");
    require(server.is_listening(), "Server must be listening.");
    const uint16_t port = server.bound_port();
    require(port > 0, "Server must have a valid bound port.");

    SocketTransportConfig client_cfg;
    client_cfg.host = "127.0.0.1";
    client_cfg.port = port;
    client_cfg.connect_timeout_ms = 1000;

    auto client = std::make_shared<SocketInputTransport>(client_cfg);
    require(!client->is_connected(), "Client not connected before connect().");
    require(client->state() == SocketTransportState::Disconnected, "State is Disconnected.");

    require(client->connect(), "Client connect() must succeed.");
    require(client->is_connected(), "Client is connected after connect().");
    require(client->state() == SocketTransportState::Connected, "State is Connected.");

    auto accepted = server.accept_client(1000);
    require(accepted != nullptr, "Server must accept client socket.");
    require(accepted->is_connected(), "Accepted server socket must be connected.");
    require(accepted->local_port() == port, "Server socket local port matches listening port.");
    require(client->remote_port() == port, "Client remote port matches server port.");

    // Orderly disconnect from client
    client->disconnect();
    require(!client->is_connected(), "Client is disconnected.");
    require(client->state() == SocketTransportState::Disconnected, "Client state Disconnected.");

    // Server detects peer orderly shutdown
    auto incoming = accepted->receive_bytes(1024);
    require(incoming.empty(), "Receive returns empty on peer disconnect.");
    require(!accepted->is_connected(), "Accepted socket detects orderly peer disconnect.");
    require(accepted->state() == SocketTransportState::Disconnected, "Accepted state Disconnected.");

    server.stop();
    require(!server.is_listening(), "Server stops listening.");
}

void test_socket_transport_bidirectional_framing_and_conformance() {
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Start server.");
    const uint16_t port = server.bound_port();

    SocketTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    auto client = std::make_shared<SocketInputTransport>(cfg);
    require(client->connect(), "Client connect.");
    auto guest = server.accept_client(1000);
    require(guest != nullptr, "Guest accepted.");

    // 1. Client -> Guest: Hello message
    HelloPayload hello{input_protocol_version, 1080, 1920, "sess_socket", "gen_1"};
    auto hello_bytes = encode_hello_message(1, 1000, hello);
    require(client->send_bytes(hello_bytes), "Client send hello.");

    // Poll until guest receives header + payload
    std::vector<std::byte> guest_buf;
    bool recvd = poll_until([&]() {
        auto b = guest->receive_bytes(4096);
        if (!b.empty()) guest_buf.insert(guest_buf.end(), b.begin(), b.end());
        return guest_buf.size() >= hello_bytes.size();
    });
    require(recvd, "Guest must receive full Hello message.");

    WireHeader in_hdr;
    require(decode_wire_header(guest_buf, in_hdr) == InputProtocolError::None, "Decode header.");
    require(in_hdr.type == InputMessageType::Hello, "Type is Hello.");
    std::span<const std::byte> p_span(guest_buf.data() + input_wire_header_size, in_hdr.payload_size);
    InputProtocolError p_err = InputProtocolError::None;
    auto in_hello = decode_hello_message(p_span, p_err);
    require(in_hello.has_value(), "Decode hello payload.");
    require(in_hello->session_id == "sess_socket", "Session ID matches.");
    require(in_hello->generation_id == "gen_1", "Generation ID matches.");

    // 2. Guest -> Client: HelloAck message
    HelloAckPayload ack{input_protocol_version, InputProtocolError::None, 10, 0x07, "sess_socket", "gen_1"};
    auto ack_bytes = encode_hello_ack_message(1, 1001, ack);
    require(guest->send_bytes(ack_bytes), "Guest send hello ack.");

    std::vector<std::byte> client_buf;
    recvd = poll_until([&]() {
        auto b = client->receive_bytes(4096);
        if (!b.empty()) client_buf.insert(client_buf.end(), b.begin(), b.end());
        return client_buf.size() >= ack_bytes.size();
    });
    require(recvd, "Client must receive HelloAck.");

    WireHeader out_hdr;
    require(decode_wire_header(client_buf, out_hdr) == InputProtocolError::None, "Decode client header.");
    require(out_hdr.type == InputMessageType::HelloAck, "Type is HelloAck.");
    std::span<const std::byte> ack_span(client_buf.data() + input_wire_header_size, out_hdr.payload_size);
    auto in_ack = decode_hello_ack_message(ack_span, p_err);
    require(in_ack.has_value(), "Decode hello ack payload.");
    require(in_ack->max_contacts == 10, "Max contacts 10.");

    client->disconnect();
    guest->disconnect();
    server.stop();
}

void test_socket_transport_non_blocking_send_and_backpressure() {
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Start server.");
    const uint16_t port = server.bound_port();

    SocketTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.max_send_queue_bytes = 4096; // small queue to trigger backpressure
    auto client = std::make_shared<SocketInputTransport>(cfg);
    require(client->connect(), "Client connect.");
    auto guest = server.accept_client(1000);
    require(guest != nullptr, "Guest accepted.");

    // DO NOT read on guest side. Flood client with data chunks
    std::vector<std::byte> chunk(1024, std::byte{0xAA});
    bool backpressure_triggered = false;

    const auto t_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 500; ++i) {
        if (!client->send_bytes(chunk)) {
            backpressure_triggered = true;
            require(client->last_error() == SocketTransportError::QueueOverflow,
                "Error must be QueueOverflow on backpressure.");
            break;
        }
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();

    require(backpressure_triggered, "Backpressure must trigger when OS and queue buffers saturate.");
    require(elapsed_ms < 500, "Send must remain non-blocking without stalling the caller thread.");

    // Drain from guest side
    std::size_t total_drained = 0;
    while (true) {
        auto b = guest->receive_bytes(16384);
        if (b.empty()) break;
        total_drained += b.size();
    }
    require(total_drained > 0, "Guest drained saturated buffer.");

    client->disconnect();
    server.stop();
}

void test_socket_transport_fragmented_and_coalesced_frames() {
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Start server.");
    const uint16_t port = server.bound_port();

    SocketTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    auto client = std::make_shared<SocketInputTransport>(cfg);
    client->connect();
    auto guest = server.accept_client(1000);

    // 1. Coalesce 3 packets into single buffer
    HelloPayload p1{input_protocol_version, 1080, 1920, "sess_frags", "gen_1"};
    auto pkt1 = encode_hello_message(1, 100, p1);

    PingPayload p2{0xCAFEF00D};
    auto pkt2 = encode_ping_message(2, 101, p2);

    CancelAllPayload p3{0};
    auto pkt3 = encode_cancel_all_message(3, 102, p3);

    std::vector<std::byte> combined;
    combined.insert(combined.end(), pkt1.begin(), pkt1.end());
    combined.insert(combined.end(), pkt2.begin(), pkt2.end());
    combined.insert(combined.end(), pkt3.begin(), pkt3.end());

    // 2. Transmit over socket in fragmented chunks of 7 bytes
    std::size_t offset = 0;
    while (offset < combined.size()) {
        std::size_t n = std::min<std::size_t>(7, combined.size() - offset);
        client->send_bytes(std::span<const std::byte>(combined.data() + offset, n));
        offset += n;
    }

    // 3. Receive and reassemble on guest endpoint
    std::vector<std::byte> rx_stream;
    bool all_rx = poll_until([&]() {
        auto b = guest->receive_bytes(4096);
        if (!b.empty()) rx_stream.insert(rx_stream.end(), b.begin(), b.end());
        return rx_stream.size() >= combined.size();
    });
    require(all_rx, "All fragmented bytes arrived.");

    // Frame 1
    WireHeader h1;
    require(decode_wire_header(rx_stream, h1) == InputProtocolError::None && h1.type == InputMessageType::Hello, "Pkt 1 Hello.");
    rx_stream.erase(rx_stream.begin(), rx_stream.begin() + input_wire_header_size + h1.payload_size);

    // Frame 2
    WireHeader h2;
    require(decode_wire_header(rx_stream, h2) == InputProtocolError::None && h2.type == InputMessageType::Ping, "Pkt 2 Ping.");
    rx_stream.erase(rx_stream.begin(), rx_stream.begin() + input_wire_header_size + h2.payload_size);

    // Frame 3
    WireHeader h3;
    require(decode_wire_header(rx_stream, h3) == InputProtocolError::None && h3.type == InputMessageType::CancelAll, "Pkt 3 CancelAll.");
    rx_stream.erase(rx_stream.begin(), rx_stream.begin() + input_wire_header_size + h3.payload_size);
    require(rx_stream.empty(), "All coalesced frames parsed cleanly.");

    client->disconnect();
    server.stop();
}

void test_socket_transport_timeout_and_unreachable() {
    SocketTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 24899; // no server listening
    cfg.connect_timeout_ms = 100; // short timeout

    auto client = std::make_shared<SocketInputTransport>(cfg);
    const auto t_start = std::chrono::steady_clock::now();
    bool connected = client->connect();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();

    require(!connected, "Connect must fail when port is unopened.");
    require(!client->is_connected(), "is_connected must be false.");
    require(elapsed < 1000, "Connect failure must return within bounded timeout.");
    require(client->last_error() == SocketTransportError::ConnectionRefused ||
            client->last_error() == SocketTransportError::TimedOut,
            "Error must be ConnectionRefused or TimedOut.");
}

void test_socket_transport_reconnection_and_stale_session_rejection() {
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Start server.");
    const uint16_t port = server.bound_port();

    SocketTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    auto client_transport = std::make_shared<SocketInputTransport>(cfg);

    GuestInputBridgeClient::Config b_cfg;
    b_cfg.session_id = "sess_reconnect";
    b_cfg.generation_id = "gen-A";
    auto bridge = std::make_shared<GuestInputBridgeClient>(client_transport, b_cfg);

    require(bridge->start(), "Bridge start.");
    auto guest_transport = server.accept_client(1000);
    require(guest_transport != nullptr, "Server accepted.");

    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon::Config d_cfg;
    d_cfg.expected_session_id = "sess_reconnect";
    d_cfg.expected_generation_id = "gen-A";
    GuestInputDaemon daemon(guest_transport, backend, d_cfg);
    require(daemon.start(), "Daemon start.");

    // Handshake
    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return bridge->is_connected() && daemon.is_active();
    });
    require(bridge->is_connected(), "Bridge connected gen-A.");
    require(daemon.is_active(), "Daemon active gen-A.");

    // 2. Change generation on bridge to gen-B -> should detect mismatch
    bridge->stop();
    daemon.shutdown();

    // Reconnect bridge with mismatched gen-B while daemon expects gen-A
    b_cfg.generation_id = "gen-B";
    auto client_transport2 = std::make_shared<SocketInputTransport>(cfg);
    auto bridge2 = std::make_shared<GuestInputBridgeClient>(client_transport2, b_cfg);
    require(bridge2->start(), "Bridge restart gen-B.");

    auto guest_transport2 = server.accept_client(1000);
    require(guest_transport2 != nullptr, "Server accept connection for gen-B.");

    GuestInputDaemon daemon2(guest_transport2, backend, d_cfg); // d_cfg still expects gen-A
    require(daemon2.start(), "Daemon 2 start.");

    poll_until([&]() {
        daemon2.poll_and_process();
        bridge2->poll_incoming_acks();
        return daemon2.state() == GuestInputDaemon::DaemonState::Error;
    });
    require(daemon2.state() == GuestInputDaemon::DaemonState::Error, "Daemon rejects mismatched generation.");
    require(daemon2.last_error() == InputProtocolError::StaleGeneration, "Error must be StaleGeneration.");

    bridge2->stop();
    daemon2.shutdown();
    server.stop();
}

void test_socket_transport_daemon_restart_active_contacts() {
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Start server.");
    const uint16_t port = server.bound_port();

    SocketTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    auto client_transport = std::make_shared<SocketInputTransport>(cfg);

    GuestInputBridgeClient::Config b_cfg;
    b_cfg.session_id = "sess_crash";
    b_cfg.generation_id = "gen-1";
    auto bridge = std::make_shared<GuestInputBridgeClient>(client_transport, b_cfg);
    bridge->start();

    auto guest_transport = server.accept_client(1000);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon::Config d_cfg;
    d_cfg.expected_session_id = "sess_crash";
    d_cfg.expected_generation_id = "gen-1";
    GuestInputDaemon daemon(guest_transport, backend, d_cfg);
    daemon.start();

    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return bridge->is_connected();
    });

    // Send 2 active touches
    TouchContactState c0; c0.contact_id = 0; c0.is_active = true; c0.start_time_ms = 100; c0.last_move_time_ms = 100; c0.norm_pos = {0.2f, 0.2f};
    TouchContactState c1; c1.contact_id = 1; c1.is_active = true; c1.start_time_ms = 100; c1.last_move_time_ms = 100; c1.norm_pos = {0.8f, 0.8f};
    require(bridge->send_touch_frame({c0, c1}), "Send touches.");

    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return daemon.active_contact_count() == 2;
    });
    require(backend->active_contact_count() == 2, "Backend tracks 2 contacts.");

    // Daemon process terminates abruptly: shutdown() clears hardware slots
    daemon.shutdown();
    require(backend->active_contact_count() == 0, "Backend cleared on daemon shutdown.");
    require(daemon.active_contact_count() == 0, "Daemon active contacts cleared.");

    bridge->stop();
    server.stop();
}

void test_socket_transport_cancellation_during_shutdown() {
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Start server.");
    const uint16_t port = server.bound_port();

    SocketTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    auto client_transport = std::make_shared<SocketInputTransport>(cfg);

    GuestInputBridgeClient::Config b_cfg;
    b_cfg.session_id = "sess_cancel";
    b_cfg.generation_id = "gen-1";
    auto bridge = std::make_shared<GuestInputBridgeClient>(client_transport, b_cfg);
    bridge->start();

    auto guest_transport = server.accept_client(1000);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon::Config d_cfg;
    d_cfg.expected_session_id = "sess_cancel";
    d_cfg.expected_generation_id = "gen-1";
    GuestInputDaemon daemon(guest_transport, backend, d_cfg);
    daemon.start();

    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return bridge->is_connected();
    });

    // Hold key and contacts
    bridge->send_key_event(66 /*ENTER*/, true);
    TouchContactState c0; c0.contact_id = 0; c0.is_active = true; c0.start_time_ms = 10; c0.last_move_time_ms = 10; c0.norm_pos = {0.5f, 0.5f};
    bridge->send_touch_frame({c0});

    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return backend->active_contact_count() == 1 && backend->is_key_held(66);
    });

    // Cancel all contacts via bridge
    require(bridge->cancel_all_contacts(1), "Send cancel all.");
    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return backend->active_contact_count() == 0 && !backend->is_key_held(66);
    });

    require(backend->active_contact_count() == 0, "Contacts canceled without leak.");
    require(!backend->is_key_held(66), "Key released without leak.");

    bridge->stop();
    daemon.shutdown();
    server.stop();
}

void test_socket_transport_four_tier_acknowledgement_distinction() {
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Start server.");
    const uint16_t port = server.bound_port();

    SocketTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    auto client_transport = std::make_shared<SocketInputTransport>(cfg);

    GuestInputBridgeClient::Config b_cfg;
    b_cfg.session_id = "sess_tiers";
    b_cfg.generation_id = "gen-1";
    auto bridge = std::make_shared<GuestInputBridgeClient>(client_transport, b_cfg);
    bridge->start();

    auto guest_transport = server.accept_client(1000);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon::Config d_cfg;
    d_cfg.expected_session_id = "sess_tiers";
    d_cfg.expected_generation_id = "gen-1";
    GuestInputDaemon daemon(guest_transport, backend, d_cfg);
    daemon.start();

    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return bridge->is_connected();
    });

    // 1. Normal delivery (Tier 1 -> Tier 2 -> Tier 3 -> Tier 4 all pass)
    TouchContactState c0; c0.contact_id = 0; c0.is_active = true; c0.start_time_ms = 1; c0.last_move_time_ms = 1; c0.norm_pos = {0.1f, 0.1f};
    require(bridge->send_touch_frame({c0}), "Tier 1: Host queue acceptance passed.");
    require(client_transport->total_bytes_sent() > 0, "Tier 2: Transport delivery passed.");

    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return bridge->total_acked_count() == 1;
    });
    require(bridge->total_acked_count() == 1, "Tier 3: Daemon acknowledgement passed.");
    require(bridge->last_error() == InputProtocolError::None, "Tier 4: Guest backend delivery passed.");

    // 2. Fault injection: simulate guest backend failure at Tier 4
    backend->set_fail_on_sync(true);
    require(bridge->send_touch_frame({c0}), "Host queues frame.");
    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return bridge->total_acked_count() == 2;
    });
    require(bridge->total_acked_count() == 2, "Tier 3: Daemon still acknowledged packet arrival.");
    require(bridge->last_error() == InputProtocolError::QueueOverflow,
        "Tier 4: Bridge correctly identified guest backend injection failure.");

    bridge->stop();
    daemon.shutdown();
    server.stop();
}

void test_socket_transport_end_to_end_bridge_daemon_interop() {
    SocketInputServer server;
    require(server.start("127.0.0.1", 0), "Start server.");
    const uint16_t port = server.bound_port();

    SocketTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    auto client_transport = std::make_shared<SocketInputTransport>(cfg);

    GuestInputBridgeClient::Config b_cfg;
    b_cfg.session_id = "sess_e2e";
    b_cfg.generation_id = "gen-1";
    b_cfg.display_width = 1080;
    b_cfg.display_height = 1920;
    auto bridge = std::make_shared<GuestInputBridgeClient>(client_transport, b_cfg);
    bridge->start();

    auto guest_transport = server.accept_client(1000);
    auto backend = std::make_shared<RecordingGuestInputBackend>();
    GuestInputDaemon::Config d_cfg;
    d_cfg.expected_session_id = "sess_e2e";
    d_cfg.expected_generation_id = "gen-1";
    GuestInputDaemon daemon(guest_transport, backend, d_cfg);
    daemon.start();

    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return bridge->is_connected() && daemon.is_active();
    });

    require(bridge->is_connected(), "Bridge connected.");
    require(daemon.is_active(), "Daemon active.");
    require(bridge->capability() == MultiTouchBackendCapability::Supported, "Multi-touch Supported.");
    require(bridge->negotiated_max_contacts() == 10, "10 contacts negotiated.");

    // Multi-touch stream: 2 fingers down
    TouchContactState f0; f0.contact_id = 0; f0.is_active = true; f0.start_time_ms = 10; f0.last_move_time_ms = 10; f0.norm_pos = {0.2f, 0.2f};
    TouchContactState f1; f1.contact_id = 1; f1.is_active = true; f1.start_time_ms = 10; f1.last_move_time_ms = 10; f1.norm_pos = {0.8f, 0.8f};
    require(bridge->send_touch_frame({f0, f1}), "Send 2 fingers down.");

    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return daemon.active_contact_count() == 2;
    });
    require(backend->active_contact_count() == 2, "Backend has 2 contacts active.");

    // Move finger 0, Release finger 1
    f0.norm_pos = {0.25f, 0.25f}; f0.last_move_time_ms = 20;
    f1.is_active = false;
    require(bridge->send_touch_frame({f0, f1}), "Send move and up.");

    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return daemon.active_contact_count() == 1;
    });
    require(daemon.is_contact_active(0), "Finger 0 active.");
    require(!daemon.is_contact_active(1), "Finger 1 released.");

    // Send key event
    require(bridge->send_key_event(62 /*SPACE*/, true), "Send key SPACE down.");
    poll_until([&]() {
        daemon.poll_and_process();
        bridge->poll_incoming_acks();
        return backend->is_key_held(62);
    });
    require(backend->is_key_held(62), "Backend has SPACE held.");

    bridge->stop();
    daemon.shutdown();
    server.stop();
}

} // namespace

int main() {
    try {
        std::cout << "Starting production socket transport tests...\n" << std::flush;

        std::cout << "1. test_socket_transport_server_client_lifecycle\n" << std::flush;
        test_socket_transport_server_client_lifecycle();

        std::cout << "2. test_socket_transport_bidirectional_framing_and_conformance\n" << std::flush;
        test_socket_transport_bidirectional_framing_and_conformance();

        std::cout << "3. test_socket_transport_non_blocking_send_and_backpressure\n" << std::flush;
        test_socket_transport_non_blocking_send_and_backpressure();

        std::cout << "4. test_socket_transport_fragmented_and_coalesced_frames\n" << std::flush;
        test_socket_transport_fragmented_and_coalesced_frames();

        std::cout << "5. test_socket_transport_timeout_and_unreachable\n" << std::flush;
        test_socket_transport_timeout_and_unreachable();

        std::cout << "6. test_socket_transport_reconnection_and_stale_session_rejection\n" << std::flush;
        test_socket_transport_reconnection_and_stale_session_rejection();

        std::cout << "7. test_socket_transport_daemon_restart_active_contacts\n" << std::flush;
        test_socket_transport_daemon_restart_active_contacts();

        std::cout << "8. test_socket_transport_cancellation_during_shutdown\n" << std::flush;
        test_socket_transport_cancellation_during_shutdown();

        std::cout << "9. test_socket_transport_four_tier_acknowledgement_distinction\n" << std::flush;
        test_socket_transport_four_tier_acknowledgement_distinction();

        std::cout << "10. test_socket_transport_end_to_end_bridge_daemon_interop\n" << std::flush;
        test_socket_transport_end_to_end_bridge_daemon_interop();

        std::cout << "All production socket input transport tests passed successfully.\n" << std::flush;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Socket transport test failed: " << e.what() << "\n";
        return 1;
    }
}
