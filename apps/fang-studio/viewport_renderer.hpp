#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "video_frame.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace fang::studio {

enum class ViewportBackend : std::uint32_t {
    Direct3D11 = 0,
    GdiFallback = 1,
    None = 2
};

constexpr std::string_view viewport_backend_name(ViewportBackend backend) noexcept {
    switch (backend) {
    case ViewportBackend::Direct3D11:  return "Direct3D 11 (Hardware Accelerated)";
    case ViewportBackend::GdiFallback: return "GDI (Software Fallback)";
    case ViewportBackend::None:
    default:                           return "None";
    }
}

struct ViewportPresentationMetrics {
    std::size_t frames_submitted{0};
    std::size_t frames_presented{0};
    std::size_t frames_dropped{0};
    std::size_t uploads_completed{0};
    std::uint64_t last_arrival_timestamp_us{0};
    std::uint64_t last_upload_duration_us{0};
    std::uint64_t last_present_duration_us{0};
    std::uint32_t current_width{0};
    std::uint32_t current_height{0};
    VideoPixelFormat current_format{VideoPixelFormat::Unknown};
    std::uint32_t dxgi_format{0};
    bool hardware_accelerated{false};
    std::string diagnostic;
};

struct ViewportRenderOptions {
    bool force_gdi_fallback{false};
    bool vsync_enabled{true};
    bool allow_warp_fallback{true};
    std::size_t max_buffer_dimension{8192};
};

using OverlayPainter = std::function<void(HDC)>;

class IViewportRenderer {
public:
    virtual ~IViewportRenderer() = default;

    virtual bool initialize(HWND target_window, const ViewportRenderOptions& options = {}) = 0;
    virtual void shutdown() = 0;
    virtual bool is_initialized() const noexcept = 0;

    virtual bool resize(int width, int height) = 0;
    virtual bool present_frame(const VideoFrame& frame, RECT target_rect,
        OverlayPainter overlay_painter = nullptr, HDC fallback_dc = nullptr) = 0;

    virtual ViewportBackend active_backend() const noexcept = 0;
    virtual std::string_view backend_name() const noexcept = 0;
    virtual std::string diagnostic() const = 0;
    virtual ViewportPresentationMetrics metrics() const = 0;
    virtual void clear_metrics() = 0;
};

class ViewportD3D11Renderer final : public IViewportRenderer {
public:
    ViewportD3D11Renderer();
    ~ViewportD3D11Renderer() override;

    ViewportD3D11Renderer(const ViewportD3D11Renderer&) = delete;
    ViewportD3D11Renderer& operator=(const ViewportD3D11Renderer&) = delete;
    ViewportD3D11Renderer(ViewportD3D11Renderer&&) = delete;
    ViewportD3D11Renderer& operator=(ViewportD3D11Renderer&&) = delete;

    bool initialize(HWND target_window, const ViewportRenderOptions& options = {}) override;
    void shutdown() override;
    bool is_initialized() const noexcept override;

    bool resize(int width, int height) override;
    bool present_frame(const VideoFrame& frame, RECT target_rect,
        OverlayPainter overlay_painter = nullptr, HDC fallback_dc = nullptr) override;

    ViewportBackend active_backend() const noexcept override;
    std::string_view backend_name() const noexcept override;
    std::string diagnostic() const override;
    ViewportPresentationMetrics metrics() const override;
    void clear_metrics() override;

    bool is_gdi_compatible() const noexcept { return gdi_compatible_; }

    static std::uint32_t translate_pixel_format_to_dxgi(VideoPixelFormat format) noexcept;

private:
    bool initialize_d3d11();
    bool create_swapchain_render_target();
    bool create_pipeline_shaders();
    bool upload_texture(const VideoFrame& frame);
    void render_gdi_fallback(const VideoFrame& frame, RECT target_rect, HDC target_dc);
    void release_swapchain_resources();
    void release_all_resources();
    void handle_device_lost();

    HWND window_{};
    ViewportRenderOptions options_;
    ViewportBackend active_backend_{ViewportBackend::None};
    bool initialized_{false};
    std::string diagnostic_;
    ViewportPresentationMetrics metrics_;
    bool gdi_compatible_{false};

    int client_width_{0};
    int client_height_{0};

    // D3D11 COM Interfaces
    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    IDXGISwapChain* swapchain_{nullptr};
    ID3D11RenderTargetView* render_target_view_{nullptr};
    ID3D11Texture2D* frame_texture_{nullptr};
    ID3D11ShaderResourceView* frame_srv_{nullptr};
    ID3D11VertexShader* vertex_shader_{nullptr};
    ID3D11PixelShader* pixel_shader_{nullptr};
    ID3D11SamplerState* sampler_state_{nullptr};

    std::uint32_t texture_width_{0};
    std::uint32_t texture_height_{0};
    std::uint32_t texture_dxgi_format_{0};
    VideoPixelFormat texture_format_{VideoPixelFormat::Unknown};
};

} // namespace fang::studio
