#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace fang::studio {

class IInputTransport {
public:
    virtual ~IInputTransport() = default;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const noexcept = 0;
    virtual bool send_bytes(std::span<const std::byte> bytes) = 0;
    virtual std::vector<std::byte> receive_bytes(std::size_t max_bytes) = 0;
};

// Thread-safe in-memory bi-directional loopback channel for offline integration testing
class LoopbackInputTransport final : public IInputTransport {
public:
    explicit LoopbackInputTransport(std::size_t max_buffer_bytes = 65536);

    // Host endpoint interface (implements IInputTransport)
    bool connect() override;
    void disconnect() override;
    bool is_connected() const noexcept override;
    bool send_bytes(std::span<const std::byte> bytes) override;
    std::vector<std::byte> receive_bytes(std::size_t max_bytes) override;

    // Guest endpoint interface
    bool guest_send_bytes(std::span<const std::byte> bytes);
    std::vector<std::byte> guest_receive_bytes(std::size_t max_bytes);
    bool is_guest_connected() const noexcept;
    void guest_disconnect();

    // Fault injection for tests
    void set_fault_drop_sends(bool drop) noexcept;
    void set_fault_sever_connection() noexcept;
    std::size_t host_to_guest_depth() const noexcept;
    std::size_t guest_to_host_depth() const noexcept;
    void clear_buffers();

private:
    const std::size_t max_buffer_bytes_;
    mutable std::mutex mutex_;
    bool connected_{false};
    bool fault_drop_sends_{false};

    std::deque<std::byte> host_to_guest_queue_;
    std::deque<std::byte> guest_to_host_queue_;
};

} // namespace fang::studio
