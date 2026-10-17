#pragma once

#include "video_frame.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fang::studio {

enum class VideoTransportState : std::uint32_t {
    Disconnected = 0,
    Connecting = 1,
    Streaming = 2,
    Error = 3,
    Closed = 4
};

constexpr std::string_view transport_state_name(VideoTransportState state) noexcept {
    switch (state) {
    case VideoTransportState::Disconnected: return "Disconnected";
    case VideoTransportState::Connecting:   return "Connecting";
    case VideoTransportState::Streaming:    return "Streaming";
    case VideoTransportState::Error:        return "Error";
    case VideoTransportState::Closed:       return "Closed";
    default:                                return "Unknown";
    }
}

[[nodiscard]] std::string shared_memory_name_for_port(std::uint16_t port);
[[nodiscard]] std::optional<std::string> shared_memory_name_for_serial(std::string_view serial);

struct VideoTransportMetrics {
    std::size_t frames_received{};
    std::size_t frames_dropped{};
    std::size_t frames_consumed{};
    std::size_t frames_torn{};
    std::size_t counter_discontinuities{};
    std::uint32_t latest_sequence{};
    std::uint64_t latest_frame_age_us{};
    std::uint32_t current_width{};
    std::uint32_t current_height{};
};

class IVideoConsumer {
public:
    virtual ~IVideoConsumer() = default;
    virtual bool publish_frame(VideoFrame frame) = 0;
    virtual void set_transport_state(VideoTransportState state, std::string message) = 0;
    virtual void record_frame_dropped() = 0;
    virtual void record_frame_torn() = 0;
};

class StudioVideoConsumer final : public IVideoConsumer {
public:
    explicit StudioVideoConsumer(std::size_t max_queue_depth = 1);
    ~StudioVideoConsumer() override = default;

    bool publish_frame(VideoFrame frame) override;
    void set_transport_state(VideoTransportState state, std::string message) override;
    void record_frame_dropped() override;
    void record_frame_torn() override;

    std::shared_ptr<const VideoFrame> current_frame() const;
    std::optional<VideoFrame> consume_frame();
    bool has_frame() const; // True if current presentation frame is valid
    bool has_queued_frames() const; // True if unconsumed queue is non-empty
    std::size_t queue_size() const;

    void set_max_frame_age_us(std::uint64_t max_age_us) noexcept;
    std::uint64_t max_frame_age_us() const noexcept;

    VideoTransportMetrics metrics() const;
    VideoTransportState transport_state() const noexcept;
    std::string transport_message() const;

    void clear();

private:
    const std::size_t max_queue_depth_;
    mutable std::mutex mutex_;
    std::deque<VideoFrame> queue_;
    std::shared_ptr<const VideoFrame> current_frame_;
    VideoTransportMetrics metrics_;
    VideoTransportState state_{VideoTransportState::Disconnected};
    std::string message_;
    std::uint64_t max_frame_age_us_{0};
};

class IVideoTransport {
public:
    virtual ~IVideoTransport() = default;
    virtual bool start(std::string_view endpoint) = 0;
    virtual void stop() = 0;
    virtual VideoTransportState state() const noexcept = 0;
    virtual std::string last_error() const = 0;
};

class SharedMemoryVideoTransport final : public IVideoTransport {
public:
    explicit SharedMemoryVideoTransport(std::shared_ptr<IVideoConsumer> consumer);
    ~SharedMemoryVideoTransport() override;

    bool start(std::string_view endpoint) override;
    void stop() override;
    VideoTransportState state() const noexcept override;
    std::string last_error() const override;

    // Direct synchronous frame extraction for deterministic testing
    bool poll_once_from_memory(const std::uint8_t* memory_data, std::size_t memory_size);

    // Test seam for injecting a raw synthetic buffer without opening a Win32 file mapping
    void set_in_memory_source(const std::uint8_t* data, std::size_t size);

private:
    void worker_loop();
    bool attach_mapping(const std::wstring& mapping_name);
    void detach_mapping();
    bool poll_once_from_memory_locked(const std::uint8_t* memory_data, std::size_t memory_size);

    std::shared_ptr<IVideoConsumer> consumer_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<VideoTransportState> state_{VideoTransportState::Disconnected};
    std::string last_error_;
    std::string endpoint_;

    void* file_mapping_handle_{};
    void* mapped_view_{};
    std::size_t mapped_size_{};

    const std::uint8_t* test_source_data_{};
    std::size_t test_source_size_{};

    std::optional<std::uint32_t> last_observed_counter_;
    std::atomic<bool> worker_running_{false};
    std::thread worker_thread_;
};

}
