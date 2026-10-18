#pragma once

#include "input_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fang::studio {

enum class SocketTransportState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Faulted
};

constexpr std::string_view socket_transport_state_name(SocketTransportState state) noexcept {
    switch (state) {
    case SocketTransportState::Disconnected: return "Disconnected";
    case SocketTransportState::Connecting:   return "Connecting";
    case SocketTransportState::Connected:    return "Connected";
    case SocketTransportState::Faulted:      return "Faulted";
    default:                                 return "Unknown";
    }
}

enum class SocketTransportError : uint8_t {
    None,
    NotConnected,
    ConnectionRefused,
    ConnectionReset,
    TimedOut,
    HostUnreachable,
    QueueOverflow,
    InvalidConfig,
    OsError
};

constexpr std::string_view socket_transport_error_name(SocketTransportError error) noexcept {
    switch (error) {
    case SocketTransportError::None:              return "None";
    case SocketTransportError::NotConnected:      return "NotConnected";
    case SocketTransportError::ConnectionRefused:  return "ConnectionRefused";
    case SocketTransportError::ConnectionReset:    return "ConnectionReset";
    case SocketTransportError::TimedOut:           return "TimedOut";
    case SocketTransportError::HostUnreachable:    return "HostUnreachable";
    case SocketTransportError::QueueOverflow:      return "QueueOverflow";
    case SocketTransportError::InvalidConfig:      return "InvalidConfig";
    case SocketTransportError::OsError:            return "OsError";
    default:                                       return "Unknown";
    }
}

struct SocketTransportConfig {
    std::string host{"127.0.0.1"};
    uint16_t port{24800};
    uint32_t connect_timeout_ms{2000};
    uint32_t send_timeout_ms{500};
    std::size_t max_send_queue_bytes{65536};
    std::size_t max_receive_buffer_bytes{65536};
    bool tcp_nodelay{true};
};

// High-performance non-blocking stream socket implementation of IInputTransport.
// Suitable for host-to-guest port-forwarded sockets (QEMU hostfwd, ADB forward)
// and deterministic offline loopback integration testing.
class SocketInputTransport final : public IInputTransport {
public:
    explicit SocketInputTransport(SocketTransportConfig config = {});
    // Server-accepted client constructor (internal socket adoption)
    SocketInputTransport(uintptr_t native_socket, SocketTransportConfig config, uint16_t local_port, uint16_t remote_port);
    ~SocketInputTransport() override;

    SocketInputTransport(const SocketInputTransport&) = delete;
    SocketInputTransport& operator=(const SocketInputTransport&) = delete;

    // IInputTransport interface
    bool connect() override;
    void disconnect() override;
    bool is_connected() const noexcept override;
    bool send_bytes(std::span<const std::byte> bytes) override;
    std::vector<std::byte> receive_bytes(std::size_t max_bytes) override;

    // Diagnostics & Metrics
    SocketTransportState state() const noexcept;
    SocketTransportError last_error() const noexcept;
    std::string error_diagnostic() const;
    const SocketTransportConfig& config() const noexcept { return config_; }

    uint64_t total_bytes_sent() const noexcept;
    uint64_t total_bytes_received() const noexcept;
    std::size_t pending_send_bytes() const noexcept;
    uint16_t local_port() const noexcept;
    uint16_t remote_port() const noexcept;

    // Test hooks & fault injection
    void set_fault_drop_sends(bool drop) noexcept;
    void flush_send_queue();

private:
    bool configure_socket(uintptr_t sock);
    void close_socket_internal();
    bool drain_send_queue_locked();

    SocketTransportConfig config_;
    mutable std::mutex mutex_;

    uintptr_t socket_{~uintptr_t(0)}; // INVALID_SOCKET / -1
    SocketTransportState state_{SocketTransportState::Disconnected};
    SocketTransportError last_error_{SocketTransportError::None};
    std::string last_error_msg_;

    uint16_t local_port_{0};
    uint16_t remote_port_{0};

    uint64_t total_bytes_sent_{0};
    uint64_t total_bytes_received_{0};
    std::deque<std::byte> send_queue_;

    bool fault_drop_sends_{false};
};

// Non-blocking TCP server listener for guest-side daemon or offline integration tests.
class SocketInputServer final {
public:
    SocketInputServer() = default;
    ~SocketInputServer();

    SocketInputServer(const SocketInputServer&) = delete;
    SocketInputServer& operator=(const SocketInputServer&) = delete;

    // Bind and listen on address:port (port = 0 selects an ephemeral port)
    bool start(std::string bind_address = "127.0.0.1", uint16_t port = 0);
    void stop();

    bool is_listening() const noexcept;
    uint16_t bound_port() const noexcept;
    const std::string& bound_address() const noexcept { return bind_address_; }

    // Poll for an incoming client connection with optional timeout in milliseconds
    std::shared_ptr<SocketInputTransport> accept_client(uint32_t timeout_ms = 0);

    SocketTransportError last_error() const noexcept;
    std::string error_diagnostic() const;

private:
    mutable std::mutex mutex_;
    uintptr_t listen_socket_{~uintptr_t(0)};
    std::string bind_address_{"127.0.0.1"};
    uint16_t bound_port_{0};
    bool is_listening_{false};

    SocketTransportError last_error_{SocketTransportError::None};
    std::string last_error_msg_;
};

} // namespace fang::studio
