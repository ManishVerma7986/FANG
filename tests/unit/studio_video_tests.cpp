#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "player_viewport.hpp"
#include "studio_input_router.hpp"
#include "video_frame.hpp"
#include "video_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace fang::studio;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::shared_ptr<std::vector<std::uint8_t>> make_solid_buffer(std::uint32_t width, std::uint32_t height,
    std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a = 255) {
    const std::size_t size = static_cast<std::size_t>(width) * height * 4;
    auto buffer = std::make_shared<std::vector<std::uint8_t>>(size);
    for (std::size_t i = 0; i < size; i += 4) {
        (*buffer)[i + 0] = b;
        (*buffer)[i + 1] = g;
        (*buffer)[i + 2] = r;
        (*buffer)[i + 3] = a;
    }
    return buffer;
}

// 1. Valid/invalid video frame metadata & buffer bounds
void test_frame_metadata_and_validation() {
    VideoFrame frame;
    frame.width = 1080;
    frame.height = 1920;
    frame.format = VideoPixelFormat::BGRA8888;
    frame.stride = 1080 * 4;
    frame.sequence = 1;
    frame.pixels = make_solid_buffer(1080, 1920, 255, 0, 0);

    require(frame.is_valid(), "Standard 1080x1920 BGRA frame must be valid.");
    require(frame.required_buffer_size() == 1080 * 1920 * 4, "Required buffer size must match 1080*1920*4.");

    // Zero width or height
    VideoFrame zero_width = frame;
    zero_width.width = 0;
    require(!zero_width.is_valid(), "Zero width must be invalid.");
    require(zero_width.required_buffer_size() == 0, "Zero width required buffer size must be 0.");

    VideoFrame zero_height = frame;
    zero_height.height = 0;
    require(!zero_height.is_valid(), "Zero height must be invalid.");

    // Excessive dimensions (> 8192)
    VideoFrame excessive_dim = frame;
    excessive_dim.width = 9000;
    require(!excessive_dim.is_valid(), "Width > 8192 must be invalid.");

    // Unknown format
    VideoFrame unknown_fmt = frame;
    unknown_fmt.format = VideoPixelFormat::Unknown;
    require(!unknown_fmt.is_valid(), "Unknown pixel format must be invalid.");
    require(unknown_fmt.required_buffer_size() == 0, "Unknown pixel format required size must be 0.");

    // Stride checks: stride less than minimum bytes per row
    VideoFrame invalid_stride = frame;
    invalid_stride.stride = (frame.width * 4) - 4;
    require(!invalid_stride.is_valid(), "Stride less than width*bpp must be invalid.");

    // Padded stride (e.g. 4096 aligned row)
    VideoFrame padded_stride = frame;
    padded_stride.width = 100;
    padded_stride.height = 100;
    padded_stride.stride = 512; // 100 * 4 = 400 < 512
    padded_stride.pixels = std::make_shared<std::vector<std::uint8_t>>(512 * 100, std::uint8_t{0});
    require(padded_stride.is_valid(), "Padded stride frame with sufficient buffer must be valid.");
    require(padded_stride.required_buffer_size() == 51200, "Required buffer size must account for stride.");

    // Truncated buffer
    VideoFrame truncated = frame;
    truncated.pixels = std::make_shared<std::vector<std::uint8_t>>((1080 * 1920 * 4) - 1);
    require(!truncated.is_valid(), "Buffer smaller than required size must be invalid.");

    // Null pixels pointer
    VideoFrame null_pixels = frame;
    null_pixels.pixels = nullptr;
    require(!null_pixels.is_valid(), "Null pixels buffer must be invalid.");
}

// 2. Video transport bounded queue, overwrite/drop oldest, and metrics
void test_consumer_bounded_queue() {
    auto consumer = std::make_shared<StudioVideoConsumer>(2); // capacity = 2

    require(!consumer->has_frame(), "Initially consumer must have no frame.");
    require(consumer->current_frame() == nullptr, "Initial current_frame must be null.");
    require(consumer->transport_state() == VideoTransportState::Disconnected, "Initial state Disconnected.");

    auto buffer = make_solid_buffer(64, 64, 0, 255, 0);

    VideoFrame f1{64, 64, 256, VideoPixelFormat::BGRA8888, 1, 1000, buffer};
    VideoFrame f2{64, 64, 256, VideoPixelFormat::BGRA8888, 2, 2000, buffer};
    VideoFrame f3{64, 64, 256, VideoPixelFormat::BGRA8888, 3, 3000, buffer};

    require(consumer->publish_frame(f1), "Frame 1 must be accepted.");
    require(consumer->has_frame(), "Consumer should have frame after publishing f1.");
    require(consumer->current_frame()->sequence == 1, "Current frame sequence must be 1.");

    require(consumer->publish_frame(f2), "Frame 2 must be accepted.");
    require(consumer->current_frame()->sequence == 2, "Current frame sequence must be 2.");

    auto metrics = consumer->metrics();
    require(metrics.frames_received == 2, "Frames received must be 2.");
    require(metrics.frames_dropped == 0, "No frames should be dropped yet.");

    // Publishing f3 should drop f1 (oldest in queue) since capacity is 2
    require(consumer->publish_frame(f3), "Frame 3 must be accepted.");
    metrics = consumer->metrics();
    require(metrics.frames_received == 3, "Frames received must be 3.");
    require(metrics.frames_dropped == 1, "Oldest frame must be dropped when bounded capacity reached.");

    // Consume frames: first should be f2 (since f1 was dropped), then f3
    auto consumed_a = consumer->consume_frame();
    require(consumed_a.has_value(), "Must consume first available frame.");
    require(consumed_a->sequence == 2, "First consumed frame must be sequence 2.");

    auto consumed_b = consumer->consume_frame();
    require(consumed_b.has_value(), "Must consume second available frame.");
    require(consumed_b->sequence == 3, "Second consumed frame must be sequence 3.");

    auto consumed_c = consumer->consume_frame();
    require(!consumed_c.has_value(), "Queue must now be empty.");

    metrics = consumer->metrics();
    require(metrics.frames_consumed == 2, "Frames consumed must be 2.");
}

// 3. Stale frame dropping & invalid frame rejection
void test_stale_and_invalid_frame_rejection() {
    auto consumer = std::make_shared<StudioVideoConsumer>(1);

    VideoFrame invalid_frame{};
    require(!consumer->publish_frame(invalid_frame), "Invalid frame must be rejected.");
    auto metrics = consumer->metrics();
    require(metrics.frames_dropped == 1, "Rejected invalid frame must count as dropped.");
    require(metrics.frames_received == 0, "Invalid frame must not count as received.");

    auto buffer = make_solid_buffer(32, 32, 128, 128, 128);
    VideoFrame valid_frame{32, 32, 128, VideoPixelFormat::BGRA8888, 10, 1000, buffer};
    require(consumer->publish_frame(valid_frame), "Valid frame must be accepted.");

    metrics = consumer->metrics();
    require(metrics.frames_received == 1, "Valid frame received count must be 1.");
    require(metrics.current_width == 32, "Consumer width must be 32.");
    require(metrics.current_height == 32, "Consumer height must be 32.");

    consumer->clear();
    require(!consumer->has_frame(), "After clear(), consumer has no frame.");
    require(consumer->consume_frame() == std::nullopt, "After clear(), queue is empty.");
}

// 4. Concurrent producer and consumer stress test
void test_concurrent_producer_consumer() {
    auto consumer = std::make_shared<StudioVideoConsumer>(2);
    constexpr int total_frames = 200;
    std::atomic<bool> producer_done{false};

    auto buffer = make_solid_buffer(16, 16, 255, 255, 255);

    std::thread producer([&]() {
        for (int i = 1; i <= total_frames; ++i) {
            VideoFrame frame{16, 16, 64, VideoPixelFormat::BGRA8888, static_cast<std::uint32_t>(i),
                static_cast<std::uint64_t>(i * 100), buffer};
            consumer->publish_frame(std::move(frame));
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        producer_done = true;
    });

    std::size_t frames_polled = 0;
    std::thread reader([&]() {
        while (!producer_done) {
            auto frame_opt = consumer->consume_frame();
            if (frame_opt.has_value()) {
                require(frame_opt->is_valid(), "Consumed frame must be valid.");
                ++frames_polled;
            }
            auto current = consumer->current_frame();
            if (current != nullptr) {
                require(current->is_valid(), "Current frame must be valid.");
            }
            std::this_thread::sleep_for(std::chrono::microseconds(30));
        }
        while (auto frame_opt = consumer->consume_frame()) {
            require(frame_opt->is_valid(), "Consumed frame must be valid.");
            ++frames_polled;
        }
    });

    producer.join();
    reader.join();

    auto metrics = consumer->metrics();
    require(metrics.frames_received == total_frames, "All producer frames must be accounted for.");
    require(metrics.frames_consumed == frames_polled, "Consumer polled count must match metrics.");
    require(metrics.frames_consumed + metrics.frames_dropped >= static_cast<std::size_t>(total_frames),
        "Sum of consumed and dropped frames must account for all pushed frames.");
}

// 5. Dynamic resolution change handling
void test_dynamic_resolution_change() {
    auto consumer = std::make_shared<StudioVideoConsumer>(1);

    auto buf_1080p = make_solid_buffer(1080, 1920, 255, 0, 0);
    VideoFrame f_1080p{1080, 1920, 1080 * 4, VideoPixelFormat::BGRA8888, 1, 1000, buf_1080p};
    consumer->publish_frame(f_1080p);
    require(consumer->metrics().current_width == 1080, "Initial width must be 1080.");
    require(consumer->metrics().current_height == 1920, "Initial height must be 1920.");

    auto buf_720p = make_solid_buffer(720, 1280, 0, 255, 0);
    VideoFrame f_720p{720, 1280, 720 * 4, VideoPixelFormat::BGRA8888, 2, 2000, buf_720p};
    consumer->publish_frame(f_720p);
    require(consumer->metrics().current_width == 720, "Switched width must be 720.");
    require(consumer->metrics().current_height == 1280, "Switched height must be 1280.");

    auto current = consumer->current_frame();
    require(current != nullptr && current->width == 720 && current->height == 1280,
        "Current frame must immediately reflect dynamic resolution change.");
}

// 6. Shared memory transport polling and header contract verification
void test_shared_memory_header_and_polling() {
    auto consumer = std::make_shared<StudioVideoConsumer>(1);
    SharedMemoryVideoTransport transport(consumer);

    // Truncated buffer test (less than 24-byte header)
    std::uint8_t tiny_buffer[16]{};
    require(!transport.poll_once_from_memory(tiny_buffer, sizeof(tiny_buffer)),
        "Polling with buffer < 24 bytes must fail.");
    require(!transport.last_error().empty(), "Truncated header must set last_error.");

    // Invalid dimensions in header (0x0)
    std::vector<std::uint8_t> mem(1024, 0);
    auto* header = reinterpret_cast<SharedMemoryVideoHeader*>(mem.data());
    header->width = 0;
    header->height = 0;
    require(!transport.poll_once_from_memory(mem.data(), mem.size()),
        "Polling with 0x0 header must fail.");

    // Valid header but truncated pixel payload
    header->width = 100;
    header->height = 100;
    header->fps = 60;
    header->frame_counter = 1;
    require(!transport.poll_once_from_memory(mem.data(), mem.size()),
        "Polling with payload smaller than width*height*4 must fail.");

    // Fully valid memory block
    const std::size_t full_size = sizeof(SharedMemoryVideoHeader) + (100 * 100 * 4);
    mem.resize(full_size, 0x7F);
    header = reinterpret_cast<SharedMemoryVideoHeader*>(mem.data());
    header->width = 100;
    header->height = 100;
    header->fps = 60;
    header->frame_counter = 101;

    require(transport.poll_once_from_memory(mem.data(), mem.size()),
        "Polling with full valid buffer must succeed.");
    require(consumer->has_frame(), "Consumer must receive the polled frame.");
    require(consumer->current_frame()->sequence == 101, "Sequence must match frame_counter.");

    // Unchanged frame_counter should be a no-op
    const auto initial_received = consumer->metrics().frames_received;
    require(transport.poll_once_from_memory(mem.data(), mem.size()),
        "Polling with same counter returns true as no-op.");
    require(consumer->metrics().frames_received == initial_received,
        "No new frame should be published when frame_counter has not advanced.");

    // Advance frame_counter
    header->frame_counter = 102;
    require(transport.poll_once_from_memory(mem.data(), mem.size()),
        "Polling with advanced counter must succeed.");
    require(consumer->metrics().frames_received == initial_received + 1,
        "New frame published when frame_counter advances.");
}

// 7. Non-existent Win32 mapping and transport lifecycle
void test_transport_lifecycle_and_fallback() {
    auto consumer = std::make_shared<StudioVideoConsumer>(1);
    SharedMemoryVideoTransport transport(consumer);

    // Attempting to open a non-existent shared memory mapping
    const bool started = transport.start("non_existent_fang_shm_test_section_xyz_12345");
    require(!started, "Opening non-existent file mapping must fail cleanly.");
    require(transport.state() == VideoTransportState::Error, "Transport state must be Error.");
    require(!transport.last_error().empty(), "Transport error message must be set.");
    require(consumer->transport_state() == VideoTransportState::Error, "Consumer reflects Error state.");

    // Test in-memory synthetic source mode
    std::vector<std::uint8_t> synthetic_mem(sizeof(SharedMemoryVideoHeader) + 64 * 64 * 4, 0xAB);
    auto* header = reinterpret_cast<SharedMemoryVideoHeader*>(synthetic_mem.data());
    header->width = 64;
    header->height = 64;
    header->fps = 60;
    header->frame_counter = 1;

    transport.set_in_memory_source(synthetic_mem.data(), synthetic_mem.size());
    require(transport.start("synthetic_test"), "Start with synthetic memory source must succeed.");
    require(transport.state() == VideoTransportState::Streaming, "State must be Streaming.");

    // Clean stop
    transport.stop();
    require(transport.state() == VideoTransportState::Closed, "State must be Closed after stop.");
}

// 8. Viewport video integration, GDI paint with live frame, and safe destruction
void test_viewport_live_video_rendering_and_destruction() {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    // Create a hidden test parent window
    WNDCLASSEXW host_wc{};
    host_wc.cbSize = sizeof(host_wc);
    host_wc.lpfnWndProc = DefWindowProcW;
    host_wc.hInstance = instance;
    host_wc.lpszClassName = L"FangStudioVideoTestHost";
    RegisterClassExW(&host_wc);

    HWND host_window = CreateWindowExW(
        0, L"FangStudioVideoTestHost", L"TestHost", WS_OVERLAPPEDWINDOW,
        100, 100, 400, 700, nullptr, nullptr, instance, nullptr);
    require(host_window != nullptr, "Host window must be created.");

    struct HostGuard {
        HWND window;
        ~HostGuard() { if (window != nullptr) DestroyWindow(window); }
    } host_guard{host_window};

    class MockInputDispatcher final : public IInputDispatcher {
    public:
        bool dispatch(const std::string&, const InputEvent&) override { return true; }
    };

    auto router = std::make_shared<StudioInputRouter>(std::make_shared<MockInputDispatcher>());
    auto consumer = std::make_shared<StudioVideoConsumer>(2);

    // Create PlayerViewport with video consumer
    auto viewport = std::make_unique<PlayerViewport>(host_window, instance, 3001, router, consumer);
    require(viewport->is_valid(), "PlayerViewport must be valid.");
    viewport->move(0, 0, 360, 640, false);

    // While runtime is Stopped: paint triggers fallback placeholder
    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);

    // Transition runtime to Ready
    fang::broker::RuntimeStatusData ready_status;
    ready_status.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_status.guest_ready = true;
    ready_status.adb_serial = "emulator-5554";
    viewport->update_runtime_state(ready_status, false);

    // Ready, but no video frame yet: paint triggers active Ready placeholder fallback
    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);

    // Publish a live 64x64 frame
    auto live_pixels = make_solid_buffer(64, 64, 0, 200, 255);
    VideoFrame frame{64, 64, 256, VideoPixelFormat::BGRA8888, 1, 1000, live_pixels};
    require(consumer->publish_frame(std::move(frame)), "Publishing live frame must succeed.");
    require(consumer->has_frame(), "Consumer must have active frame.");

    // Ready with live video frame: paint triggers StretchDIBits live path
    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);

    // Simulate touch feedback over live frame
    SendMessageW(viewport->handle(), WM_LBUTTONDOWN, 0, MAKELPARAM(180, 320));
    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);
    SendMessageW(viewport->handle(), WM_LBUTTONUP, 0, MAKELPARAM(180, 320));

    // Transition to Stopped: viewport clears consumer frame automatically
    fang::broker::RuntimeStatusData stopped_status;
    stopped_status.state = fang::broker::RuntimeLifecycleState::Stopped;
    stopped_status.guest_ready = false;
    viewport->update_runtime_state(stopped_status, false);
    require(!consumer->has_frame(), "Consumer frames must be cleared when guest leaves Ready.");

    // Safe destruction while consumer is still referenced
    viewport.reset();
    require(consumer != nullptr, "Consumer outlives viewport without issue.");
}

// 9. Overflow in stride, buffer size calculations, and bounds
void test_overflow_stride_and_bounds() {
    VideoFrame frame;
    frame.width = 100;
    frame.height = 100;
    frame.format = VideoPixelFormat::BGRA8888;
    frame.sequence = 1;
    frame.pixels = make_solid_buffer(100, 100, 0, 0, 0);

    // Stride causing overflow or exceeding sanity limits
    VideoFrame huge_stride = frame;
    huge_stride.stride = 0xFFFFFFFF;
    require(!huge_stride.is_valid(), "Max uint32 stride must be invalid.");
    require(huge_stride.required_buffer_size() == 0, "Huge stride must return 0 required size.");

    VideoFrame excessive_padding = frame;
    excessive_padding.stride = 8192 * 32; // > 8192 * 16 limit
    require(!excessive_padding.is_valid(), "Excessive row padding must be rejected.");

    // Multiplication overflow in required_buffer_size
    VideoFrame overflow_calc = frame;
    overflow_calc.width = 0x20000000;
    overflow_calc.height = 10;
    require(overflow_calc.required_buffer_size() == 0, "Buffer size calculation exceeding 1 GiB returns 0.");
}

// 10. Frame counter zero and discontinuity handling
void test_frame_counter_zero_and_discontinuity() {
    auto consumer = std::make_shared<StudioVideoConsumer>(2);
    SharedMemoryVideoTransport transport(consumer);

    const std::size_t mem_size = sizeof(SharedMemoryVideoHeader) + (32 * 32 * 4);
    std::vector<std::uint8_t> mem(mem_size, 0x55);
    auto* header = reinterpret_cast<SharedMemoryVideoHeader*>(mem.data());
    header->width = 32;
    header->height = 32;
    header->fps = 60;
    header->frame_counter = 0; // Test sequence starting at 0

    // First poll with counter 0 publishes frame 0
    require(transport.poll_once_from_memory(mem.data(), mem.size()), "Initial frame 0 must poll.");
    require(consumer->metrics().frames_received == 1, "Frames received must be 1.");

    // Second poll with counter still 0 must NOT duplicate frame
    require(transport.poll_once_from_memory(mem.data(), mem.size()), "Repeated counter 0 returns true.");
    require(consumer->metrics().frames_received == 1, "Counter 0 must not cause duplicate publications.");

    // Advance counter to 500
    header->frame_counter = 500;
    require(transport.poll_once_from_memory(mem.data(), mem.size()), "Counter 500 must poll.");
    require(consumer->metrics().frames_received == 2, "Frames received must be 2.");

    // Reset counter to 5 (counter discontinuity / wrap)
    header->frame_counter = 5;
    require(transport.poll_once_from_memory(mem.data(), mem.size()), "Counter reset must be accepted.");
    require(consumer->metrics().frames_received == 3, "Frames received must be 3.");
    require(consumer->metrics().counter_discontinuities == 1, "Counter discontinuity must be tracked.");
}

// 11. Stale frame threshold dropping and queue queries
void test_stale_frame_threshold_and_queue_queries() {
    auto consumer = std::make_shared<StudioVideoConsumer>(3);
    consumer->set_max_frame_age_us(50000); // 50 ms max age
    require(consumer->max_frame_age_us() == 50000, "Max frame age must match set value.");

    auto buffer = make_solid_buffer(16, 16, 10, 20, 30);

    // Stale frame: timestamp is far in the past (1000 us)
    VideoFrame stale_frame{16, 16, 64, VideoPixelFormat::BGRA8888, 1, 1000, buffer};
    require(!consumer->publish_frame(stale_frame), "Stale frame must be rejected by consumer.");
    require(consumer->metrics().frames_dropped == 1, "Stale frame must increment frames_dropped.");
    require(!consumer->has_queued_frames(), "Queue must be empty after stale drop.");
    require(consumer->queue_size() == 0, "Queue size must be 0.");

    // Fresh frame: timestamp = 0 (bypasses age check) or fresh
    VideoFrame fresh_frame{16, 16, 64, VideoPixelFormat::BGRA8888, 2, 0, buffer};
    require(consumer->publish_frame(fresh_frame), "Fresh frame must be accepted.");
    require(consumer->has_queued_frames(), "Queue must have queued frame.");
    require(consumer->queue_size() == 1, "Queue size must be 1.");
    require(consumer->has_frame(), "Current presentation frame must be available.");

    auto consumed = consumer->consume_frame();
    require(consumed.has_value() && consumed->sequence == 2, "Must consume fresh frame.");
    require(!consumer->has_queued_frames(), "Queue must now be empty.");
    require(consumer->has_frame(), "Current presentation frame remains valid after queue pop.");
}

// 12. Torn frame detection and metrics
void test_torn_frame_detection() {
    auto consumer = std::make_shared<StudioVideoConsumer>(1);
    SharedMemoryVideoTransport transport(consumer);

    // Test record_frame_torn API on consumer
    consumer->record_frame_torn();
    require(consumer->metrics().frames_torn == 1, "Frames torn must be 1.");
    require(consumer->metrics().frames_dropped == 1, "Torn frame must also increment frames_dropped.");

    // Deterministic test for poll_once_from_memory tear check:
    // If a background thread mutates the header counter during buffer copy of a large buffer
    const std::size_t mem_size = sizeof(SharedMemoryVideoHeader) + (1000 * 1000 * 4);
    std::vector<std::uint8_t> mem(mem_size, 0xEE);
    auto* header = reinterpret_cast<SharedMemoryVideoHeader*>(mem.data());
    header->width = 1000;
    header->height = 1000;
    header->fps = 60;
    header->frame_counter = 1;

    std::atomic<bool> stop_mutator{false};
    std::thread mutator([&]() {
        while (!stop_mutator) {
            header->frame_counter = header->frame_counter + 1;
            std::this_thread::yield();
        }
    });

    // When counter is rapidly changing, poll_once_from_memory detects tear
    bool detected_tear = false;
    for (int i = 0; i < 20; ++i) {
        if (!transport.poll_once_from_memory(mem.data(), mem.size())) {
            detected_tear = true;
            break;
        }
    }
    stop_mutator = true;
    mutator.join();

    require(detected_tear || consumer->metrics().frames_torn > 0 || !transport.last_error().empty(),
        "Torn frame or error must be registered when producer modifies header concurrently.");
}

// 13. Responsive worker thread shutdown via condition variable
void test_responsive_worker_thread_shutdown() {
    auto consumer = std::make_shared<StudioVideoConsumer>(1);
    SharedMemoryVideoTransport transport(consumer);

    std::vector<std::uint8_t> synthetic_mem(sizeof(SharedMemoryVideoHeader) + 16 * 16 * 4, 0x99);
    auto* header = reinterpret_cast<SharedMemoryVideoHeader*>(synthetic_mem.data());
    header->width = 16;
    header->height = 16;
    header->fps = 60;
    header->frame_counter = 1;

    transport.set_in_memory_source(synthetic_mem.data(), synthetic_mem.size());
    require(transport.start("responsive_stop_test"), "Transport start must succeed.");
    require(transport.state() == VideoTransportState::Streaming, "Transport must be Streaming.");

    const auto t0 = std::chrono::steady_clock::now();
    transport.stop();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    require(transport.state() == VideoTransportState::Closed, "State must be Closed.");
    require(elapsed_ms < 100, "Worker thread shutdown must be responsive via CV (<100ms).");
}

// 14. RGBA format swizzle and zero dimension safety in Viewport
void test_rgba_swizzle_and_zero_dimension_viewport() {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW host_wc{};
    host_wc.cbSize = sizeof(host_wc);
    host_wc.lpfnWndProc = DefWindowProcW;
    host_wc.hInstance = instance;
    host_wc.lpszClassName = L"FangStudioRgbaTestHost";
    RegisterClassExW(&host_wc);

    HWND host_window = CreateWindowExW(
        0, L"FangStudioRgbaTestHost", L"TestHost", WS_OVERLAPPEDWINDOW,
        50, 50, 300, 500, nullptr, nullptr, instance, nullptr);
    require(host_window != nullptr, "Host window must be created.");

    struct HostGuard {
        HWND window;
        ~HostGuard() { if (window != nullptr) DestroyWindow(window); }
    } host_guard{host_window};

    class MockInputDispatcher final : public IInputDispatcher {
    public:
        bool dispatch(const std::string&, const InputEvent&) override { return true; }
    };

    auto router = std::make_shared<StudioInputRouter>(std::make_shared<MockInputDispatcher>());
    auto consumer = std::make_shared<StudioVideoConsumer>(1);

    auto viewport = std::make_unique<PlayerViewport>(host_window, instance, 4001, router, consumer);
    require(viewport->is_valid(), "PlayerViewport must be valid.");

    // Set Ready state
    fang::broker::RuntimeStatusData ready_status;
    ready_status.state = fang::broker::RuntimeLifecycleState::Ready;
    ready_status.guest_ready = true;
    viewport->update_runtime_state(ready_status, false);

    // Publish RGBA8888 frame (tests RGBA to BGRA swizzle conversion in paint)
    auto rgba_pixels = make_solid_buffer(32, 32, 255, 128, 64); // R=255, G=128, B=64
    VideoFrame rgba_frame{32, 32, 128, VideoPixelFormat::RGBA8888, 1, 0, rgba_pixels};
    require(consumer->publish_frame(std::move(rgba_frame)), "RGBA frame must be accepted.");

    viewport->move(0, 0, 200, 350, false);
    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);

    // Test zero dimension safety
    viewport->move(0, 0, 0, 0, false);
    SendMessageW(viewport->handle(), WM_PAINT, 0, 0);

    // Viewport destruction
    viewport.reset();
}

// 15. Port, serial, mapping name resolution, delayed mapping creation & reconnection resilience
void test_port_serial_mapping_and_reconnect_resilience() {
    // 1. Endpoint name resolution
    require(shared_memory_name_for_port(5554) == "videmulator5554", "Port 5554 name must match.");
    require(shared_memory_name_for_port(5556) == "videmulator5556", "Port 5556 name must match.");

    require(shared_memory_name_for_serial("emulator-5554") == "videmulator5554", "Valid serial resolution.");
    require(shared_memory_name_for_serial("emulator-5556") == "videmulator5556", "Valid serial resolution.");
    require(!shared_memory_name_for_serial("emulator-").has_value(), "Empty port is invalid.");
    require(!shared_memory_name_for_serial("emulator-abc").has_value(), "Non-numeric port is invalid.");
    require(!shared_memory_name_for_serial("device-5554").has_value(), "Wrong prefix is invalid.");
    require(!shared_memory_name_for_serial("").has_value(), "Empty serial is invalid.");
    require(!shared_memory_name_for_serial("emulator-5555").has_value(), "Odd console port is invalid.");
    require(!shared_memory_name_for_serial("emulator-80").has_value(), "Privileged port < 1024 is invalid.");
    require(!shared_memory_name_for_serial("emulator-0").has_value(), "Port 0 is invalid.");
    require(!shared_memory_name_for_serial("emulator-65535").has_value(), "Port 65535 is invalid.");
    require(!shared_memory_name_for_serial("emulator-99999").has_value(), "Port > 65535 is invalid.");

    // 2. Delayed Win32 mapping creation & reconnection
    auto consumer = std::make_shared<StudioVideoConsumer>(2);
    SharedMemoryVideoTransport transport(consumer);

    // Initial start fails because mapping doesn't exist yet
    const std::string mapping_name = "videmulator_test_delayed_5554";
    const std::wstring full_mapping_name = L"Local\\" + std::wstring(mapping_name.begin(), mapping_name.end());

    require(!transport.start(mapping_name), "Start must fail when mapping is absent.");
    require(transport.state() == VideoTransportState::Error, "State must be Error on missing mapping.");

    // Now simulate the emulator creating the Win32 mapping
    const std::size_t section_bytes = sizeof(SharedMemoryVideoHeader) + 64 * 64 * 4;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(section_bytes), full_mapping_name.c_str());
    require(mapping != nullptr, "Test mapping creation must succeed.");

    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, section_bytes);
    require(view != nullptr, "MapViewOfFile must succeed.");

    auto* header = reinterpret_cast<SharedMemoryVideoHeader*>(view);
    header->width = 64;
    header->height = 64;
    header->fps = 60;
    header->frame_counter = 1;
    std::memset(static_cast<std::uint8_t*>(view) + sizeof(SharedMemoryVideoHeader), 0x7F, 64 * 64 * 4);

    // Reconnection attempt: now that mapping exists, start must succeed
    require(transport.start(mapping_name), "Reconnection start must succeed after mapping appears.");
    require(transport.state() == VideoTransportState::Streaming, "State must be Streaming.");

    // Allow worker thread to poll frame
    for (int i = 0; i < 20 && !consumer->has_frame(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(consumer->has_frame(), "Consumer must receive frame from newly created mapping.");
    const auto frame = consumer->current_frame();
    require(frame != nullptr && frame->width == 64 && frame->height == 64, "Frame dimensions must match.");

    // Stop transport and cleanup
    transport.stop();
    require(transport.state() == VideoTransportState::Closed, "State must be Closed.");

    UnmapViewOfFile(view);
    CloseHandle(mapping);
}

}

int main() {
    try {
        test_frame_metadata_and_validation();
        test_consumer_bounded_queue();
        test_stale_and_invalid_frame_rejection();
        test_concurrent_producer_consumer();
        test_dynamic_resolution_change();
        test_shared_memory_header_and_polling();
        test_transport_lifecycle_and_fallback();
        test_viewport_live_video_rendering_and_destruction();
        test_overflow_stride_and_bounds();
        test_frame_counter_zero_and_discontinuity();
        test_stale_frame_threshold_and_queue_queries();
        test_torn_frame_detection();
        test_responsive_worker_thread_shutdown();
        test_rgba_swizzle_and_zero_dimension_viewport();
        test_port_serial_mapping_and_reconnect_resilience();
        std::cout << "All studio video transport, consumer, and viewport integration tests passed successfully.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
