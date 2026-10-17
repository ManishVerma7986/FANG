#include "video_transport.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <chrono>

namespace fang::studio {

namespace {

std::uint64_t current_time_microseconds() noexcept {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (QueryPerformanceCounter(&counter) != 0 && QueryPerformanceFrequency(&frequency) != 0 && frequency.QuadPart > 0) {
        return static_cast<std::uint64_t>((counter.QuadPart * 1000000ULL) / frequency.QuadPart);
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::wstring to_wide_string(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), result.data(), count);
    return result;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), result.data(), count, nullptr, nullptr);
    return result;
}

}

std::string shared_memory_name_for_port(std::uint16_t port) {
    return "videmulator" + std::to_string(port);
}

std::optional<std::string> shared_memory_name_for_serial(std::string_view serial) {
    constexpr std::string_view prefix = "emulator-";
    if (!serial.starts_with(prefix)) return std::nullopt;
    const auto port_str = serial.substr(prefix.size());
    if (port_str.empty()) return std::nullopt;
    for (char c : port_str) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    try {
        const auto port_val = std::stoul(std::string(port_str));
        if (port_val < 1024 || port_val > 65534 || (port_val % 2 != 0)) return std::nullopt;
        return shared_memory_name_for_port(static_cast<std::uint16_t>(port_val));
    } catch (...) {
        return std::nullopt;
    }
}

StudioVideoConsumer::StudioVideoConsumer(std::size_t max_queue_depth)
    : max_queue_depth_(std::max<std::size_t>(1, max_queue_depth)) {}

bool StudioVideoConsumer::publish_frame(VideoFrame frame) {
    if (!frame.is_valid()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++metrics_.frames_dropped;
        return false;
    }

    const auto now_us = current_time_microseconds();
    std::lock_guard<std::mutex> lock(mutex_);

    // Drop frame if already older than configured stale threshold
    if (max_frame_age_us_ > 0 && frame.timestamp_us > 0) {
        if (now_us > frame.timestamp_us + max_frame_age_us_) {
            ++metrics_.frames_dropped;
            return false;
        }
    }

    ++metrics_.frames_received;
    if (metrics_.latest_sequence > 0 && frame.sequence < metrics_.latest_sequence) {
        ++metrics_.counter_discontinuities;
    }
    metrics_.latest_sequence = frame.sequence;
    if (frame.timestamp_us > 0 && now_us >= frame.timestamp_us) {
        metrics_.latest_frame_age_us = now_us - frame.timestamp_us;
    } else {
        metrics_.latest_frame_age_us = 0;
    }
    metrics_.current_width = frame.width;
    metrics_.current_height = frame.height;

    // Drop oldest frame if queue capacity reached
    while (queue_.size() >= max_queue_depth_) {
        queue_.pop_front();
        ++metrics_.frames_dropped;
    }

    queue_.push_back(frame);
    current_frame_ = std::make_shared<const VideoFrame>(std::move(frame));
    state_ = VideoTransportState::Streaming;
    return true;
}

void StudioVideoConsumer::set_transport_state(VideoTransportState state, std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    message_ = std::move(message);
    if (state == VideoTransportState::Disconnected || state == VideoTransportState::Closed ||
        state == VideoTransportState::Error) {
        queue_.clear();
    }
}

void StudioVideoConsumer::record_frame_dropped() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.frames_dropped;
}

void StudioVideoConsumer::record_frame_torn() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.frames_torn;
    ++metrics_.frames_dropped;
}

std::shared_ptr<const VideoFrame> StudioVideoConsumer::current_frame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_frame_;
}

std::optional<VideoFrame> StudioVideoConsumer::consume_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return std::nullopt;
    VideoFrame frame = std::move(queue_.front());
    queue_.pop_front();
    ++metrics_.frames_consumed;
    return frame;
}

bool StudioVideoConsumer::has_frame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_frame_ != nullptr && current_frame_->is_valid();
}

bool StudioVideoConsumer::has_queued_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !queue_.empty();
}

std::size_t StudioVideoConsumer::queue_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void StudioVideoConsumer::set_max_frame_age_us(std::uint64_t max_age_us) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    max_frame_age_us_ = max_age_us;
}

std::uint64_t StudioVideoConsumer::max_frame_age_us() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_frame_age_us_;
}

VideoTransportMetrics StudioVideoConsumer::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

VideoTransportState StudioVideoConsumer::transport_state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string StudioVideoConsumer::transport_message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

void StudioVideoConsumer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    current_frame_.reset();
    metrics_.current_width = 0;
    metrics_.current_height = 0;
}

SharedMemoryVideoTransport::SharedMemoryVideoTransport(std::shared_ptr<IVideoConsumer> consumer)
    : consumer_(std::move(consumer)) {}

SharedMemoryVideoTransport::~SharedMemoryVideoTransport() {
    stop();
}

bool SharedMemoryVideoTransport::start(std::string_view endpoint) {
    stop();

    std::lock_guard<std::mutex> lock(mutex_);
    endpoint_ = std::string(endpoint);
    last_error_.clear();
    last_observed_counter_.reset();

    if (consumer_) {
        consumer_->set_transport_state(VideoTransportState::Connecting, "Connecting to shared memory: " + endpoint_);
    }

    if (test_source_data_ != nullptr && test_source_size_ > 0) {
        state_ = VideoTransportState::Streaming;
        if (consumer_) {
            consumer_->set_transport_state(VideoTransportState::Streaming, "Attached to synthetic memory source");
        }
        worker_running_ = true;
        worker_thread_ = std::thread(&SharedMemoryVideoTransport::worker_loop, this);
        return true;
    }

    const std::wstring wide_name = to_wide_string(endpoint_);
    if (!attach_mapping(wide_name)) {
        state_ = VideoTransportState::Error;
        if (consumer_) {
            consumer_->set_transport_state(VideoTransportState::Error, last_error_);
        }
        return false;
    }

    state_ = VideoTransportState::Streaming;
    if (consumer_) {
        consumer_->set_transport_state(VideoTransportState::Streaming, "Attached to Win32 section: " + endpoint_);
    }

    worker_running_ = true;
    worker_thread_ = std::thread(&SharedMemoryVideoTransport::worker_loop, this);
    return true;
}

void SharedMemoryVideoTransport::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        cv_.notify_all();
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    detach_mapping();
    last_observed_counter_.reset();
    state_ = VideoTransportState::Closed;
    if (consumer_) {
        consumer_->set_transport_state(VideoTransportState::Closed, "Transport stopped");
    }
}

VideoTransportState SharedMemoryVideoTransport::state() const noexcept {
    return state_.load();
}

std::string SharedMemoryVideoTransport::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void SharedMemoryVideoTransport::set_in_memory_source(const std::uint8_t* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    test_source_data_ = data;
    test_source_size_ = size;
}

bool SharedMemoryVideoTransport::poll_once_from_memory(const std::uint8_t* memory_data, std::size_t memory_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    return poll_once_from_memory_locked(memory_data, memory_size);
}

bool SharedMemoryVideoTransport::poll_once_from_memory_locked(const std::uint8_t* memory_data, std::size_t memory_size) {
    if (memory_data == nullptr || memory_size < sizeof(SharedMemoryVideoHeader)) {
        last_error_ = "Memory region is smaller than the required 24-byte header.";
        return false;
    }

    SharedMemoryVideoHeader header_before{};
    std::memcpy(&header_before, memory_data, sizeof(SharedMemoryVideoHeader));

    if (header_before.width == 0 || header_before.height == 0 ||
        header_before.width > 8192 || header_before.height > 8192) {
        last_error_ = "Invalid frame dimensions in shared memory header: " +
            std::to_string(header_before.width) + "x" + std::to_string(header_before.height);
        return false;
    }

    const std::size_t bpp = 4; // Standard 32-bit BGRA
    const std::size_t payload_bytes = static_cast<std::size_t>(header_before.width) * header_before.height * bpp;
    if (memory_size < sizeof(SharedMemoryVideoHeader) + payload_bytes) {
        last_error_ = "Memory region is truncated: size is " + std::to_string(memory_size) +
            ", required " + std::to_string(sizeof(SharedMemoryVideoHeader) + payload_bytes);
        return false;
    }

    if (last_observed_counter_.has_value() && *last_observed_counter_ == header_before.frame_counter) {
        // No new frame published yet
        return true;
    }

    auto pixel_buffer = std::make_shared<std::vector<std::uint8_t>>(
        memory_data + sizeof(SharedMemoryVideoHeader),
        memory_data + sizeof(SharedMemoryVideoHeader) + payload_bytes);

    // Tear detection: verify that producer did not update header/counter during buffer copy
    SharedMemoryVideoHeader header_after{};
    std::memcpy(&header_after, memory_data, sizeof(SharedMemoryVideoHeader));
    if (header_after.frame_counter != header_before.frame_counter ||
        header_after.width != header_before.width ||
        header_after.height != header_before.height) {
        last_error_ = "Torn frame detected: header modified during copy (counter changed from " +
            std::to_string(header_before.frame_counter) + " to " +
            std::to_string(header_after.frame_counter) + ").";
        if (consumer_) {
            consumer_->record_frame_torn();
        }
        return false;
    }

    last_observed_counter_ = header_before.frame_counter;

    VideoFrame frame;
    frame.width = header_before.width;
    frame.height = header_before.height;
    frame.stride = header_before.width * static_cast<std::uint32_t>(bpp);
    frame.format = VideoPixelFormat::BGRA8888;
    frame.sequence = header_before.frame_counter;
    frame.timestamp_us = current_time_microseconds();
    frame.pixels = std::move(pixel_buffer);

    if (consumer_) {
        return consumer_->publish_frame(std::move(frame));
    }
    return true;
}

bool SharedMemoryVideoTransport::attach_mapping(const std::wstring& mapping_name) {
    if (mapping_name.empty()) {
        last_error_ = "Shared memory handle name is empty.";
        return false;
    }

    // Attempt direct name first, then Local\ prefix fallback
    HANDLE handle = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name.c_str());
    if (handle == nullptr && !mapping_name.starts_with(L"Local\\")) {
        const std::wstring local_name = L"Local\\" + mapping_name;
        handle = OpenFileMappingW(FILE_MAP_READ, FALSE, local_name.c_str());
    }

    if (handle == nullptr) {
        const DWORD err = GetLastError();
        last_error_ = "OpenFileMapping failed for " + wide_to_utf8(mapping_name) +
            "; Win32 error " + std::to_string(err);
        return false;
    }

    void* view = MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        const DWORD err = GetLastError();
        CloseHandle(handle);
        last_error_ = "MapViewOfFile failed; Win32 error " + std::to_string(err);
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < sizeof(SharedMemoryVideoHeader)) {
        UnmapViewOfFile(view);
        CloseHandle(handle);
        last_error_ = "VirtualQuery failed or shared memory mapping size is smaller than required 24-byte header.";
        return false;
    }

    mapped_size_ = mbi.RegionSize;
    file_mapping_handle_ = handle;
    mapped_view_ = view;
    return true;
}

void SharedMemoryVideoTransport::detach_mapping() {
    if (mapped_view_ != nullptr) {
        UnmapViewOfFile(mapped_view_);
        mapped_view_ = nullptr;
    }
    if (file_mapping_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_mapping_handle_));
        file_mapping_handle_ = nullptr;
    }
    mapped_size_ = 0;
}

void SharedMemoryVideoTransport::worker_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (worker_running_) {
        if (mapped_view_ != nullptr && mapped_size_ > 0) {
            poll_once_from_memory_locked(static_cast<const std::uint8_t*>(mapped_view_), mapped_size_);
        } else if (test_source_data_ != nullptr && test_source_size_ > 0) {
            poll_once_from_memory_locked(test_source_data_, test_source_size_);
        }
        cv_.wait_for(lock, std::chrono::milliseconds(16), [this]() {
            return !worker_running_;
        });
    }
}

}
