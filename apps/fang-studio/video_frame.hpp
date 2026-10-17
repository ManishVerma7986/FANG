#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace fang::studio {

enum class VideoPixelFormat : std::uint32_t {
    Unknown = 0,
    RGBA8888 = 1,
    BGRA8888 = 2,
    RGB888 = 3
};

constexpr std::string_view pixel_format_name(VideoPixelFormat format) noexcept {
    switch (format) {
    case VideoPixelFormat::RGBA8888: return "RGBA8888";
    case VideoPixelFormat::BGRA8888: return "BGRA8888";
    case VideoPixelFormat::RGB888:   return "RGB888";
    case VideoPixelFormat::Unknown:
    default:
        return "Unknown";
    }
}

constexpr std::uint32_t bytes_per_pixel(VideoPixelFormat format) noexcept {
    switch (format) {
    case VideoPixelFormat::RGBA8888:
    case VideoPixelFormat::BGRA8888:
        return 4;
    case VideoPixelFormat::RGB888:
        return 3;
    case VideoPixelFormat::Unknown:
    default:
        return 0;
    }
}

#pragma pack(push, 1)
struct SharedMemoryVideoHeader {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t fps{};
    std::uint32_t frame_counter{};
    std::uint32_t reserved[2]{};
};
#pragma pack(pop)

static_assert(sizeof(SharedMemoryVideoHeader) == 24,
    "SharedMemoryVideoHeader must match the Android Emulator 24-byte layout.");

struct VideoFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    VideoPixelFormat format{VideoPixelFormat::BGRA8888};
    std::uint32_t sequence{};
    std::uint64_t timestamp_us{};
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;

    [[nodiscard]] constexpr std::size_t required_buffer_size() const noexcept {
        if (width == 0 || height == 0) return 0;
        const auto bpp = bytes_per_pixel(format);
        if (bpp == 0) return 0;
        constexpr std::uint64_t max_buffer_size = 0x40000000ULL; // 1 GiB sanity ceiling
        if (width > max_buffer_size / bpp) return 0;
        const std::uint64_t min_stride = static_cast<std::uint64_t>(width) * bpp;
        const std::uint64_t effective_stride = stride == 0 ? min_stride : static_cast<std::uint64_t>(stride);
        if (effective_stride < min_stride || effective_stride > max_buffer_size) return 0;
        if (effective_stride > max_buffer_size / static_cast<std::uint64_t>(height)) return 0;
        const std::uint64_t total = effective_stride * static_cast<std::uint64_t>(height);
        if (total > max_buffer_size) return 0;
        return static_cast<std::size_t>(total);
    }

    [[nodiscard]] bool is_valid() const noexcept {
        if (width == 0 || height == 0) return false;
        if (width > 8192 || height > 8192) return false;
        if (format == VideoPixelFormat::Unknown) return false;
        const auto bpp = bytes_per_pixel(format);
        if (bpp == 0) return false;
        const std::uint32_t min_stride = width * bpp;
        if (stride != 0) {
            if (stride < min_stride) return false;
            if (stride > 8192 * 16) return false; // Max reasonable padding limit
        }
        if (!pixels) return false;
        const auto required_size = required_buffer_size();
        return required_size > 0 && pixels->size() >= required_size;
    }
};

}
