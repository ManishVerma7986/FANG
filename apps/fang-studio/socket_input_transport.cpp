#include "socket_input_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

using sock_handle_t = SOCKET;
constexpr sock_handle_t k_invalid_sock = INVALID_SOCKET;
constexpr int k_sock_err = SOCKET_ERROR;

namespace {
struct WinsockGlobalScope {
    WinsockGlobalScope() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockGlobalScope() {
        WSACleanup();
    }
};

void ensure_network_subsystem() {
    static WinsockGlobalScope s_init;
}

int get_last_net_error() {
    return WSAGetLastError();
}

bool is_would_block_error(int err) {
    return err == WSAEWOULDBLOCK;
}

bool is_conn_reset_error(int err) {
    return err == WSAECONNRESET || err == WSAECONNABORTED || err == WSAESHUTDOWN;
}

void close_sock_handle(sock_handle_t s) {
    if (s != k_invalid_sock) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}

bool set_sock_nonblocking(sock_handle_t s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

} // namespace

#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

using sock_handle_t = int;
constexpr sock_handle_t k_invalid_sock = -1;
constexpr int k_sock_err = -1;

namespace {
void ensure_network_subsystem() {}

int get_last_net_error() {
    return errno;
}

bool is_would_block_error(int err) {
    return err == EWOULDBLOCK || err == EAGAIN;
}

bool is_conn_reset_error(int err) {
    return err == ECONNRESET || err == EPIPE || err == ENOTCONN;
}

void close_sock_handle(sock_handle_t s) {
    if (s != k_invalid_sock) {
        shutdown(s, SHUT_RDWR);
        close(s);
    }
}

bool set_sock_nonblocking(sock_handle_t s) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace
#endif

namespace fang::studio {

// ============================================================================
// SocketInputTransport
// ============================================================================

SocketInputTransport::SocketInputTransport(SocketTransportConfig config)
    : config_(std::move(config))
    , socket_(static_cast<uintptr_t>(k_invalid_sock)) {
    ensure_network_subsystem();
}

SocketInputTransport::SocketInputTransport(
    uintptr_t native_socket, SocketTransportConfig config, uint16_t local_port, uint16_t remote_port)
    : config_(std::move(config))
    , socket_(native_socket)
    , state_(native_socket != static_cast<uintptr_t>(k_invalid_sock) ? SocketTransportState::Connected : SocketTransportState::Disconnected)
    , local_port_(local_port)
    , remote_port_(remote_port) {
    ensure_network_subsystem();
    if (socket_ != static_cast<uintptr_t>(k_invalid_sock)) {
        configure_socket(socket_);
    }
}

SocketInputTransport::~SocketInputTransport() {
    disconnect();
}

bool SocketInputTransport::configure_socket(uintptr_t sock) {
    auto s = static_cast<sock_handle_t>(sock);
    if (s == k_invalid_sock) {
        return false;
    }

    if (!set_sock_nonblocking(s)) {
        return false;
    }

    if (config_.tcp_nodelay) {
        int flag = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
    }

    return true;
}

void SocketInputTransport::close_socket_internal() {
    auto s = static_cast<sock_handle_t>(socket_);
    if (s != k_invalid_sock) {
        close_sock_handle(s);
        socket_ = static_cast<uintptr_t>(k_invalid_sock);
    }
    send_queue_.clear();
}

bool SocketInputTransport::connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == SocketTransportState::Connected) {
        return true;
    }

    close_socket_internal();
    state_ = SocketTransportState::Connecting;
    last_error_ = SocketTransportError::None;
    last_error_msg_.clear();

    sock_handle_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == k_invalid_sock) {
        state_ = SocketTransportState::Faulted;
        last_error_ = SocketTransportError::OsError;
        last_error_msg_ = "Failed to create socket: " + std::to_string(get_last_net_error());
        return false;
    }

    if (!configure_socket(static_cast<uintptr_t>(s))) {
        close_sock_handle(s);
        state_ = SocketTransportState::Faulted;
        last_error_ = SocketTransportError::OsError;
        last_error_msg_ = "Failed to set socket non-blocking.";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) <= 0) {
        close_sock_handle(s);
        state_ = SocketTransportState::Faulted;
        last_error_ = SocketTransportError::InvalidConfig;
        last_error_msg_ = "Invalid host IP address: " + config_.host;
        return false;
    }

    int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc == k_sock_err) {
        int err = get_last_net_error();
        if (is_would_block_error(err)
#if !defined(_WIN32)
            || err == EINPROGRESS
#endif
        ) {
            // Await connection completion using non-blocking select
            fd_set write_fds, err_fds;
            FD_ZERO(&write_fds);
            FD_ZERO(&err_fds);
            FD_SET(s, &write_fds);
            FD_SET(s, &err_fds);

            timeval tv{};
            tv.tv_sec = static_cast<long>(config_.connect_timeout_ms / 1000);
            tv.tv_usec = static_cast<long>((config_.connect_timeout_ms % 1000) * 1000);

            int sel_res = select(static_cast<int>(s + 1), nullptr, &write_fds, &err_fds, &tv);
            if (sel_res <= 0) {
                close_sock_handle(s);
                state_ = SocketTransportState::Faulted;
                last_error_ = (sel_res == 0) ? SocketTransportError::TimedOut : SocketTransportError::OsError;
                last_error_msg_ = (sel_res == 0) ? "Connection timed out." : "Select error during connect.";
                return false;
            }

            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
            if (so_error != 0) {
                close_sock_handle(s);
                state_ = SocketTransportState::Faulted;
                last_error_ = SocketTransportError::ConnectionRefused;
                last_error_msg_ = "Connection failed: error " + std::to_string(so_error);
                return false;
            }
        } else {
            close_sock_handle(s);
            state_ = SocketTransportState::Faulted;
            last_error_ = SocketTransportError::ConnectionRefused;
            last_error_msg_ = "Connect failed immediately: error " + std::to_string(err);
            return false;
        }
    }

    socket_ = static_cast<uintptr_t>(s);
    state_ = SocketTransportState::Connected;

    // Cache local port
    sockaddr_in local_addr{};
    socklen_t l_len = sizeof(local_addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local_addr), &l_len) == 0) {
        local_port_ = ntohs(local_addr.sin_port);
    }
    remote_port_ = config_.port;

    return true;
}

void SocketInputTransport::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_socket_internal();
    state_ = SocketTransportState::Disconnected;
}

bool SocketInputTransport::is_connected() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == SocketTransportState::Connected &&
           socket_ != static_cast<uintptr_t>(k_invalid_sock);
}

bool SocketInputTransport::drain_send_queue_locked() {
      auto s = static_cast<sock_handle_t>(socket_);
      std::cerr << "[DEBUG drain] entry state=" << static_cast<int>(state_) << " socket=" << socket_ << " queue_size=" << send_queue_.size() << std::endl;
      if (s == k_invalid_sock || send_queue_.empty()) {
          std::cerr << "[DEBUG drain] empty queue or invalid sock, returning true" << std::endl;
          return true;
      }

    // Flatten contiguous chunk from deque
    std::vector<std::byte> chunk;
    chunk.reserve(std::min<std::size_t>(send_queue_.size(), 16384));
    for (std::size_t i = 0; i < chunk.capacity() && !send_queue_.empty(); ++i) {
        chunk.push_back(send_queue_.front());
        send_queue_.pop_front();
    }

int sent = ::send(s, reinterpret_cast<const char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
      std::cerr << "[DEBUG drain] send returned " << sent << " errno=" << (sent == k_sock_err ? get_last_net_error() : 0) << std::endl;
      if (sent > 0) {
        total_bytes_sent_ += static_cast<uint64_t>(sent);
        if (static_cast<std::size_t>(sent) < chunk.size()) {
            // Push unsent portion back to the front of send_queue_
            for (std::size_t i = chunk.size(); i > static_cast<std::size_t>(sent); --i) {
                send_queue_.push_front(chunk[i - 1]);
            }
        }
        return true;
    }

if (sent == k_sock_err) {
          int err = get_last_net_error();
          std::cerr << "[DEBUG drain] send error err=" << err << std::endl;
          if (is_would_block_error(err)) {
              // Socket buffer full; restore popped chunk
              for (auto it = chunk.rbegin(); it != chunk.rend(); ++it) {
                  send_queue_.push_front(*it);
              }
              std::cerr << "[DEBUG drain] would block, restored queue, size=" << send_queue_.size() << std::endl;
              return true;
          }

          if (is_conn_reset_error(err)) {
              std::cerr << "[DEBUG drain] connection reset" << std::endl;
              last_error_ = SocketTransportError::ConnectionReset;
              last_error_msg_ = "Connection reset by peer during drain.";
          } else {
              std::cerr << "[DEBUG drain] OS error" << std::endl;
              last_error_ = SocketTransportError::OsError;
              last_error_msg_ = "Send error: " + std::to_string(err);
          }
          close_socket_internal();
          state_ = SocketTransportState::Faulted;
          return false;
      }

    return true;
}

bool SocketInputTransport::send_bytes(std::span<const std::byte> bytes) {
      std::lock_guard<std::mutex> lock(mutex_);
      std::cerr << "[DEBUG send_bytes] entry state=" << static_cast<int>(state_) << " socket=" << socket_ << " bytes=" << bytes.size() << std::endl;
      if (state_ != SocketTransportState::Connected || socket_ == static_cast<uintptr_t>(k_invalid_sock)) {
          std::cerr << "[DEBUG send_bytes] Not connected! state=" << static_cast<int>(state_) << std::endl;
          last_error_ = SocketTransportError::NotConnected;
          return false;
      }

      // Drain pending bytes first
      std::cerr << "[DEBUG send_bytes] Calling drain_send_queue_locked" << std::endl;
      if (!drain_send_queue_locked()) {
          std::cerr << "[DEBUG send_bytes] drain_send_queue_locked failed" << std::endl;
          return false;
      }
      std::cerr << "[DEBUG send_bytes] drain_send_queue_locked returned true" << std::endl;

    // Bounded queue backpressure protection
    if (send_queue_.size() + bytes.size() > config_.max_send_queue_bytes) {
        last_error_ = SocketTransportError::QueueOverflow;
        last_error_msg_ = "Socket send queue capacity exceeded (backpressure triggered).";
        return false;
    }

    if (fault_drop_sends_) {
        // Drop simulation for fault injection tests
        return true;
    }

    auto s = static_cast<sock_handle_t>(socket_);
    if (send_queue_.empty()) {
        // Attempt immediate non-blocking send
        int sent = ::send(s, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
        if (sent > 0) {
            total_bytes_sent_ += static_cast<uint64_t>(sent);
            if (static_cast<std::size_t>(sent) < bytes.size()) {
                // Buffer the unsent tail
                send_queue_.insert(send_queue_.end(), bytes.begin() + sent, bytes.end());
            }
            return true;
        }

        if (sent == k_sock_err) {
            int err = get_last_net_error();
            if (is_would_block_error(err)) {
                // Buffer the full packet in queue
                send_queue_.insert(send_queue_.end(), bytes.begin(), bytes.end());
                return true;
            }

            if (is_conn_reset_error(err)) {
                last_error_ = SocketTransportError::ConnectionReset;
                last_error_msg_ = "Peer reset connection.";
            } else {
                last_error_ = SocketTransportError::OsError;
                last_error_msg_ = "Send socket error: " + std::to_string(err);
            }
            close_socket_internal();
            state_ = SocketTransportState::Faulted;
            return false;
        }
    } else {
        // Send queue has prior data; preserve strict FIFO sequence ordering
        send_queue_.insert(send_queue_.end(), bytes.begin(), bytes.end());
    }

    return true;
}

std::vector<std::byte> SocketInputTransport::receive_bytes(std::size_t max_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cerr << "[DEBUG receive_bytes] entry state=" << static_cast<int>(state_) << " socket=" << socket_ << std::endl;
    if (state_ != SocketTransportState::Connected || socket_ == static_cast<uintptr_t>(k_invalid_sock)) {
        std::cerr << "[DEBUG receive_bytes] not connected, returning empty" << std::endl;
        return {};
    }

    // Drain pending send queue opportunistically
    drain_send_queue_locked();

    auto s = static_cast<sock_handle_t>(socket_);
    const std::size_t read_limit = std::min({max_bytes, config_.max_receive_buffer_bytes, std::size_t(65536)});
    std::vector<std::byte> buf(read_limit);

    int received = ::recv(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);

    // DEBUG
    static uint64_t recv_call_count = 0;
    recv_call_count++;
    if (recv_call_count % 100000 == 0) {
        int err = get_last_net_error();
        std::cerr << "[DEBUG SocketInputTransport::receive_bytes] call=" << recv_call_count 
                  << " received=" << received << " err=" << err 
                  << " state=" << static_cast<int>(state_) << std::endl;
    }

    if (received > 0) {
        buf.resize(static_cast<std::size_t>(received));
        total_bytes_received_ += static_cast<uint64_t>(received);
        return buf;
    }

    if (received == 0) {
        // Orderly disconnect by peer
        close_socket_internal();
        state_ = SocketTransportState::Disconnected;
        last_error_ = SocketTransportError::None;
        last_error_msg_ = "Peer disconnected cleanly.";
        return {};
    }

    int err = get_last_net_error();
    if (is_would_block_error(err)) {
        return {}; // No data available right now
    }

    if (is_conn_reset_error(err)) {
        last_error_ = SocketTransportError::ConnectionReset;
        last_error_msg_ = "Connection reset by peer.";
    } else {
        last_error_ = SocketTransportError::OsError;
        last_error_msg_ = "Recv error: " + std::to_string(err);
    }
    close_socket_internal();
    state_ = SocketTransportState::Faulted;
    return {};
}

SocketTransportState SocketInputTransport::state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

SocketTransportError SocketInputTransport::last_error() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::string SocketInputTransport::error_diagnostic() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream ss;
    ss << "State=" << socket_transport_state_name(state_)
       << ", Error=" << socket_transport_error_name(last_error_);
    if (!last_error_msg_.empty()) {
        ss << " (" << last_error_msg_ << ")";
    }
    ss << ", Sent=" << total_bytes_sent_ << "B, Recv=" << total_bytes_received_ << "B"
       << ", Queue=" << send_queue_.size() << "B"
       << ", LocalPort=" << local_port_ << ", RemotePort=" << remote_port_;
    return ss.str();
}

uint64_t SocketInputTransport::total_bytes_sent() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_bytes_sent_;
}

uint64_t SocketInputTransport::total_bytes_received() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_bytes_received_;
}

std::size_t SocketInputTransport::pending_send_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return send_queue_.size();
}

uint16_t SocketInputTransport::local_port() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_port_;
}

uint16_t SocketInputTransport::remote_port() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return remote_port_;
}

void SocketInputTransport::set_fault_drop_sends(bool drop) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fault_drop_sends_ = drop;
}

void SocketInputTransport::flush_send_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    drain_send_queue_locked();
}

// ============================================================================
// SocketInputServer
// ============================================================================

SocketInputServer::~SocketInputServer() {
    stop();
}

bool SocketInputServer::start(std::string bind_address, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_listening_) {
        return true;
    }

    ensure_network_subsystem();
    bind_address_ = std::move(bind_address);

    sock_handle_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == k_invalid_sock) {
        last_error_ = SocketTransportError::OsError;
        last_error_msg_ = "Failed to create server socket: " + std::to_string(get_last_net_error());
        return false;
    }

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address_.c_str(), &addr.sin_addr) <= 0) {
        close_sock_handle(s);
        last_error_ = SocketTransportError::InvalidConfig;
        last_error_msg_ = "Invalid bind address: " + bind_address_;
        return false;
    }

    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == k_sock_err) {
        int err = get_last_net_error();
        close_sock_handle(s);
        last_error_ = SocketTransportError::OsError;
        last_error_msg_ = "Failed to bind server socket to port " + std::to_string(port) + ": " + std::to_string(err);
        return false;
    }

    if (listen(s, SOMAXCONN) == k_sock_err) {
        int err = get_last_net_error();
        close_sock_handle(s);
        last_error_ = SocketTransportError::OsError;
        last_error_msg_ = "Failed to listen: " + std::to_string(err);
        return false;
    }

    if (!set_sock_nonblocking(s)) {
        close_sock_handle(s);
        last_error_ = SocketTransportError::OsError;
        last_error_msg_ = "Failed to set server non-blocking.";
        return false;
    }

    sockaddr_in actual_addr{};
    socklen_t a_len = sizeof(actual_addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&actual_addr), &a_len) == 0) {
        bound_port_ = ntohs(actual_addr.sin_port);
    } else {
        bound_port_ = port;
    }

    listen_socket_ = static_cast<uintptr_t>(s);
    is_listening_ = true;
    last_error_ = SocketTransportError::None;
    last_error_msg_.clear();
    return true;
}

void SocketInputServer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto s = static_cast<sock_handle_t>(listen_socket_);
    if (s != k_invalid_sock) {
        close_sock_handle(s);
        listen_socket_ = static_cast<uintptr_t>(k_invalid_sock);
    }
    is_listening_ = false;
    bound_port_ = 0;
}

bool SocketInputServer::is_listening() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_listening_;
}

uint16_t SocketInputServer::bound_port() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return bound_port_;
}

std::shared_ptr<SocketInputTransport> SocketInputServer::accept_client(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_listening_ || listen_socket_ == static_cast<uintptr_t>(k_invalid_sock)) {
        return nullptr;
    }

    auto s = static_cast<sock_handle_t>(listen_socket_);

    if (timeout_ms > 0) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(s, &read_fds);

        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout_ms / 1000);
        tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);

        int sel = select(static_cast<int>(s + 1), &read_fds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            return nullptr;
        }
    }

    sockaddr_in client_addr{};
    socklen_t c_len = sizeof(client_addr);
    sock_handle_t client_sock = accept(s, reinterpret_cast<sockaddr*>(&client_addr), &c_len);

    if (client_sock == k_invalid_sock) {
        int err = get_last_net_error();
        if (!is_would_block_error(err)) {
            last_error_ = SocketTransportError::OsError;
            last_error_msg_ = "Accept failed: " + std::to_string(err);
        }
        return nullptr;
    }

    // DEBUG
    std::cerr << "[DEBUG accept] client_sock=" << client_sock << " remote_port=" << ntohs(client_addr.sin_port) << std::endl;

    // Configure accepted client socket
    set_sock_nonblocking(client_sock);
    int flag = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    uint16_t remote_p = ntohs(client_addr.sin_port);
    SocketTransportConfig cfg;
    cfg.host = bind_address_;
    cfg.port = bound_port_;

    auto transport = std::make_shared<SocketInputTransport>(
        static_cast<uintptr_t>(client_sock), cfg, bound_port_, remote_p);
    
    // DEBUG
    std::cerr << "[DEBUG accept] transport_created state=" << static_cast<int>(transport->state()) 
              << " connected=" << transport->is_connected() << std::endl;

    return transport;
}

SocketTransportError SocketInputServer::last_error() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::string SocketInputServer::error_diagnostic() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream ss;
    ss << "Listening=" << (is_listening_ ? "true" : "false")
       << ", Address=" << bind_address_ << ":" << bound_port_
       << ", LastError=" << socket_transport_error_name(last_error_);
    if (!last_error_msg_.empty()) {
        ss << " (" << last_error_msg_ << ")";
    }
    return ss.str();
}

} // namespace fang::studio
