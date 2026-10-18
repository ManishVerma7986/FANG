#include "input_transport.hpp"

#include <algorithm>

namespace fang::studio {

LoopbackInputTransport::LoopbackInputTransport(std::size_t max_buffer_bytes)
    : max_buffer_bytes_(max_buffer_bytes) {}

bool LoopbackInputTransport::connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    return true;
}

void LoopbackInputTransport::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    host_to_guest_queue_.clear();
    guest_to_host_queue_.clear();
}

bool LoopbackInputTransport::is_connected() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool LoopbackInputTransport::send_bytes(std::span<const std::byte> bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || fault_drop_sends_) {
        return false;
    }

    if (host_to_guest_queue_.size() + bytes.size() > max_buffer_bytes_) {
        return false; // Queue overflow
    }

    host_to_guest_queue_.insert(host_to_guest_queue_.end(), bytes.begin(), bytes.end());
    return true;
}

std::vector<std::byte> LoopbackInputTransport::receive_bytes(std::size_t max_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) {
        return {};
    }

    const std::size_t count = std::min(max_bytes, guest_to_host_queue_.size());
    std::vector<std::byte> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(guest_to_host_queue_.front());
        guest_to_host_queue_.pop_front();
    }
    return out;
}

bool LoopbackInputTransport::guest_send_bytes(std::span<const std::byte> bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || fault_drop_sends_) {
        return false;
    }

    if (guest_to_host_queue_.size() + bytes.size() > max_buffer_bytes_) {
        return false; // Queue overflow
    }

    guest_to_host_queue_.insert(guest_to_host_queue_.end(), bytes.begin(), bytes.end());
    return true;
}

std::vector<std::byte> LoopbackInputTransport::guest_receive_bytes(std::size_t max_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) {
        return {};
    }

    const std::size_t count = std::min(max_bytes, host_to_guest_queue_.size());
    std::vector<std::byte> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(host_to_guest_queue_.front());
        host_to_guest_queue_.pop_front();
    }
    return out;
}

bool LoopbackInputTransport::is_guest_connected() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

void LoopbackInputTransport::guest_disconnect() {
    disconnect();
}

void LoopbackInputTransport::set_fault_drop_sends(bool drop) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    fault_drop_sends_ = drop;
}

void LoopbackInputTransport::set_fault_sever_connection() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
}

std::size_t LoopbackInputTransport::host_to_guest_depth() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return host_to_guest_queue_.size();
}

std::size_t LoopbackInputTransport::guest_to_host_depth() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return guest_to_host_queue_.size();
}

void LoopbackInputTransport::clear_buffers() {
    std::lock_guard<std::mutex> lock(mutex_);
    host_to_guest_queue_.clear();
    guest_to_host_queue_.clear();
}

} // namespace fang::studio
