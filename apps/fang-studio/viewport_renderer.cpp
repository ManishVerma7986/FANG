#include "viewport_renderer.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <sstream>

namespace fang::studio {

namespace {

std::uint64_t qpc_now_us() noexcept {
    LARGE_INTEGER freq{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    if (freq.QuadPart == 0) return 0;
    return static_cast<std::uint64_t>((counter.QuadPart * 1000000ULL) / freq.QuadPart);
}

template <typename T>
void safe_release(T*& ptr) noexcept {
    if (ptr != nullptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

const char g_shader_hlsl[] = R"(
struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT VSMain(uint id : SV_VertexID) {
    VS_OUTPUT output;
    float2 coords = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? -1.0 : 1.0);
    output.pos = float4(coords, 0.0, 1.0);
    output.uv = float2((coords.x + 1.0) * 0.5, (1.0 - coords.y) * 0.5);
    return output;
}

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

float4 PSMain(VS_OUTPUT input) : SV_Target {
    return g_texture.Sample(g_sampler, input.uv);
}
)";

} // namespace

std::uint32_t ViewportD3D11Renderer::translate_pixel_format_to_dxgi(VideoPixelFormat format) noexcept {
    switch (format) {
    case VideoPixelFormat::RGBA8888: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VideoPixelFormat::BGRA8888: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VideoPixelFormat::RGB888:
    case VideoPixelFormat::Unknown:
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

ViewportD3D11Renderer::ViewportD3D11Renderer() = default;

ViewportD3D11Renderer::~ViewportD3D11Renderer() {
    shutdown();
}

bool ViewportD3D11Renderer::initialize(HWND target_window, const ViewportRenderOptions& options) {
    shutdown();
    window_ = target_window;
    options_ = options;
    metrics_ = {};

    if (options_.force_gdi_fallback) {
        active_backend_ = ViewportBackend::GdiFallback;
        initialized_ = true;
        diagnostic_ = "GDI fallback active: forced by configuration.";
        metrics_.diagnostic = diagnostic_;
        return true;
    }

    if (window_ == nullptr || IsWindow(window_) == FALSE) {
        active_backend_ = ViewportBackend::GdiFallback;
        initialized_ = true;
        diagnostic_ = "Target window handle is null or invalid; initialized in GDI fallback mode.";
        metrics_.diagnostic = diagnostic_;
        return false;
    }

    RECT rc{};
    GetClientRect(window_, &rc);
    client_width_ = std::max(1, static_cast<int>(rc.right - rc.left));
    client_height_ = std::max(1, static_cast<int>(rc.bottom - rc.top));

    if (initialize_d3d11()) {
        active_backend_ = ViewportBackend::Direct3D11;
        initialized_ = true;
        diagnostic_ = "Direct3D 11 hardware presentation pipeline active.";
        metrics_.hardware_accelerated = true;
        metrics_.diagnostic = diagnostic_;
        return true;
    }

    active_backend_ = ViewportBackend::GdiFallback;
    initialized_ = true;
    metrics_.hardware_accelerated = false;
    metrics_.diagnostic = diagnostic_;
    return false;
}

void ViewportD3D11Renderer::shutdown() {
    release_all_resources();
    initialized_ = false;
    active_backend_ = ViewportBackend::None;
}

bool ViewportD3D11Renderer::is_initialized() const noexcept {
    return initialized_;
}

bool ViewportD3D11Renderer::resize(int width, int height) {
    if (!initialized_) {
        return false;
    }

    if (width <= 0 || height <= 0) {
        client_width_ = width;
        client_height_ = height;
        return true; // Minimized; swapchain resize deferred
    }

    if (width == client_width_ && height == client_height_) {
        return true;
    }

    client_width_ = width;
    client_height_ = height;

    if (active_backend_ != ViewportBackend::Direct3D11 || swapchain_ == nullptr) {
        return true;
    }

    release_swapchain_resources();

    const HRESULT hr = swapchain_->ResizeBuffers(0,
        static_cast<UINT>(client_width_),
        static_cast<UINT>(client_height_),
        DXGI_FORMAT_UNKNOWN, 0);

    if (FAILED(hr)) {
        diagnostic_ = "DXGI ResizeBuffers failed: hr=" + std::to_string(hr);
        handle_device_lost();
        return false;
    }

    return create_swapchain_render_target();
}

bool ViewportD3D11Renderer::upload_texture(const VideoFrame& frame) {
    const std::uint32_t target_dxgi = translate_pixel_format_to_dxgi(frame.format);
    if (target_dxgi == DXGI_FORMAT_UNKNOWN) {
        diagnostic_ = "Unsupported video pixel format for D3D11 upload: " +
            std::string(pixel_format_name(frame.format));
        return false;
    }

    if (frame.width > options_.max_buffer_dimension || frame.height > options_.max_buffer_dimension) {
        diagnostic_ = "Frame dimensions exceed maximum allowed buffer dimension.";
        return false;
    }

    if (frame_texture_ == nullptr || texture_width_ != frame.width ||
        texture_height_ != frame.height || texture_dxgi_format_ != target_dxgi) {
        safe_release(frame_srv_);
        safe_release(frame_texture_);

        D3D11_TEXTURE2D_DESC td{};
        td.Width = frame.width;
        td.Height = frame.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = static_cast<DXGI_FORMAT>(target_dxgi);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&td, nullptr, &frame_texture_);
        if (FAILED(hr)) {
            diagnostic_ = "Failed to create D3D11 texture2d: hr=" + std::to_string(hr);
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = td.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;

        hr = device_->CreateShaderResourceView(frame_texture_, &srv_desc, &frame_srv_);
        if (FAILED(hr)) {
            safe_release(frame_texture_);
            diagnostic_ = "Failed to create D3D11 shader resource view: hr=" + std::to_string(hr);
            return false;
        }

        texture_width_ = frame.width;
        texture_height_ = frame.height;
        texture_dxgi_format_ = target_dxgi;
        texture_format_ = frame.format;
    }

    const std::uint64_t start_us = qpc_now_us();
    const std::uint32_t bpp = bytes_per_pixel(frame.format);
    const std::uint32_t row_pitch = frame.stride == 0 ? (frame.width * bpp) : frame.stride;

    context_->UpdateSubresource(frame_texture_, 0, nullptr, frame.pixels->data(), row_pitch, 0);

    const std::uint64_t end_us = qpc_now_us();
    metrics_.last_upload_duration_us = (end_us >= start_us) ? (end_us - start_us) : 0;
    ++metrics_.uploads_completed;

    return true;
}

void ViewportD3D11Renderer::render_gdi_fallback(const VideoFrame& frame, RECT target_rect, HDC target_dc) {
    if (target_dc == nullptr || frame.pixels == nullptr) {
        return;
    }

    const int target_width = target_rect.right - target_rect.left;
    const int target_height = target_rect.bottom - target_rect.top;
    if (target_width <= 0 || target_height <= 0) {
        return;
    }

    const std::uint64_t start_us = qpc_now_us();

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(frame.width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(frame.height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const void* pixel_data = frame.pixels->data();
    std::vector<std::uint8_t> converted_pixels;
    if (frame.format == VideoPixelFormat::RGBA8888) {
        converted_pixels.resize(frame.pixels->size());
        const auto* src = frame.pixels->data();
        auto* dst = converted_pixels.data();
        const std::size_t total_pixels = static_cast<std::size_t>(frame.width) * frame.height;
        for (std::size_t i = 0; i < total_pixels; ++i) {
            dst[i * 4 + 0] = src[i * 4 + 2]; // B
            dst[i * 4 + 1] = src[i * 4 + 1]; // G
            dst[i * 4 + 2] = src[i * 4 + 0]; // R
            dst[i * 4 + 3] = src[i * 4 + 3]; // A
        }
        pixel_data = converted_pixels.data();
    }

    SetStretchBltMode(target_dc, COLORONCOLOR);
    StretchDIBits(target_dc,
        target_rect.left, target_rect.top,
        target_width, target_height,
        0, 0, frame.width, frame.height,
        pixel_data, &bmi, DIB_RGB_COLORS, SRCCOPY);

    const std::uint64_t end_us = qpc_now_us();
    metrics_.last_upload_duration_us = (end_us >= start_us) ? (end_us - start_us) : 0;
    metrics_.last_present_duration_us = 0;
}

bool ViewportD3D11Renderer::present_frame(const VideoFrame& frame, RECT target_rect,
    OverlayPainter overlay_painter, HDC fallback_dc) {
    if (!initialized_) {
        return false;
    }

    if (!frame.is_valid()) {
        ++metrics_.frames_dropped;
        diagnostic_ = "Frame rejected: invalid metadata or buffer size.";
        return false;
    }

    ++metrics_.frames_submitted;
    metrics_.last_arrival_timestamp_us = frame.timestamp_us;
    metrics_.current_width = frame.width;
    metrics_.current_height = frame.height;
    metrics_.current_format = frame.format;

    if (active_backend_ == ViewportBackend::GdiFallback) {
        if (fallback_dc != nullptr) {
            render_gdi_fallback(frame, target_rect, fallback_dc);
            if (overlay_painter != nullptr) {
                overlay_painter(fallback_dc);
            }
            ++metrics_.frames_presented;
            return true;
        }
        diagnostic_ = "GDI fallback active but target DC was null.";
        return false;
    }

    // Direct3D 11 Presentation
    if (client_width_ <= 0 || client_height_ <= 0) {
        return true; // Viewport minimized
    }

    if (!upload_texture(frame)) {
        ++metrics_.frames_dropped;
        // Fall back to GDI if DC provided
        if (fallback_dc != nullptr) {
            render_gdi_fallback(frame, target_rect, fallback_dc);
            if (overlay_painter != nullptr) {
                overlay_painter(fallback_dc);
            }
            ++metrics_.frames_presented;
            return true;
        }
        return false;
    }

    metrics_.dxgi_format = texture_dxgi_format_;

    // Clear background to container color RGB(24, 27, 34)
    constexpr FLOAT clear_color[4] = { 24.0f / 255.0f, 27.0f / 255.0f, 34.0f / 255.0f, 1.0f };
    context_->ClearRenderTargetView(render_target_view_, clear_color);

    // Setup viewport to target_rect
    const int target_width = target_rect.right - target_rect.left;
    const int target_height = target_rect.bottom - target_rect.top;
    if (target_width > 0 && target_height > 0) {
        D3D11_VIEWPORT vp{};
        vp.TopLeftX = static_cast<FLOAT>(target_rect.left);
        vp.TopLeftY = static_cast<FLOAT>(target_rect.top);
        vp.Width = static_cast<FLOAT>(target_width);
        vp.Height = static_cast<FLOAT>(target_height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);

        context_->OMSetRenderTargets(1, &render_target_view_, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertex_shader_, nullptr, 0);
        context_->PSSetShader(pixel_shader_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &frame_srv_);
        context_->PSSetSamplers(0, 1, &sampler_state_);
        context_->Draw(4, 0);
    }

    // Draw overlay on top of presented frame if GDI compatible surface is available
    if (overlay_painter != nullptr && gdi_compatible_ && swapchain_ != nullptr) {
        ID3D11RenderTargetView* null_rtv = nullptr;
        context_->OMSetRenderTargets(1, &null_rtv, nullptr);

        IDXGISurface1* gdi_surface = nullptr;
        const HRESULT hr_surf = swapchain_->GetBuffer(0, __uuidof(IDXGISurface1), reinterpret_cast<void**>(&gdi_surface));
        if (SUCCEEDED(hr_surf) && gdi_surface != nullptr) {
            HDC surface_dc = nullptr;
            if (SUCCEEDED(gdi_surface->GetDC(FALSE, &surface_dc)) && surface_dc != nullptr) {
                overlay_painter(surface_dc);
                gdi_surface->ReleaseDC(nullptr);
            }
            safe_release(gdi_surface);
        }

        context_->OMSetRenderTargets(1, &render_target_view_, nullptr);
    }

    const std::uint64_t present_start_us = qpc_now_us();
    const UINT sync_interval = options_.vsync_enabled ? 1 : 0;
    const HRESULT hr = swapchain_->Present(sync_interval, 0);
    const std::uint64_t present_end_us = qpc_now_us();
    metrics_.last_present_duration_us = (present_end_us >= present_start_us) ? (present_end_us - present_start_us) : 0;

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        diagnostic_ = "DXGI Present returned device lost/reset. Attempting recovery...";
        handle_device_lost();
        return false;
    }

    if (FAILED(hr)) {
        diagnostic_ = "DXGI Present failed: hr=" + std::to_string(hr);
        return false;
    }

    ++metrics_.frames_presented;
    metrics_.hardware_accelerated = true;
    return true;
}

ViewportBackend ViewportD3D11Renderer::active_backend() const noexcept {
    return active_backend_;
}

std::string_view ViewportD3D11Renderer::backend_name() const noexcept {
    return viewport_backend_name(active_backend_);
}

std::string ViewportD3D11Renderer::diagnostic() const {
    return diagnostic_;
}

ViewportPresentationMetrics ViewportD3D11Renderer::metrics() const {
    auto m = metrics_;
    m.diagnostic = diagnostic_;
    return m;
}

void ViewportD3D11Renderer::clear_metrics() {
    metrics_ = {};
    diagnostic_.clear();
}

bool ViewportD3D11Renderer::initialize_d3d11() {
    UINT create_device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    create_device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL created_feature_level{};

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        create_device_flags,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        &device_,
        &created_feature_level,
        &context_);

    if (FAILED(hr) && options_.allow_warp_fallback) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            create_device_flags,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            &device_,
            &created_feature_level,
            &context_);
    }

    if (FAILED(hr) || device_ == nullptr || context_ == nullptr) {
        diagnostic_ = "D3D11CreateDevice failed: hr=" + std::to_string(hr);
        return false;
    }

    IDXGIDevice* dxgi_device = nullptr;
    hr = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device));
    if (FAILED(hr) || dxgi_device == nullptr) {
        diagnostic_ = "QueryInterface for IDXGIDevice failed: hr=" + std::to_string(hr);
        return false;
    }

    IDXGIAdapter* dxgi_adapter = nullptr;
    hr = dxgi_device->GetAdapter(&dxgi_adapter);
    safe_release(dxgi_device);
    if (FAILED(hr) || dxgi_adapter == nullptr) {
        diagnostic_ = "GetAdapter failed: hr=" + std::to_string(hr);
        return false;
    }

    IDXGIFactory* dxgi_factory = nullptr;
    hr = dxgi_adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgi_factory));
    safe_release(dxgi_adapter);
    if (FAILED(hr) || dxgi_factory == nullptr) {
        diagnostic_ = "GetParent for IDXGIFactory failed: hr=" + std::to_string(hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = static_cast<UINT>(std::max(1, client_width_));
    scd.BufferDesc.Height = static_cast<UINT>(std::max(1, client_height_));
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = window_;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;

    hr = dxgi_factory->CreateSwapChain(device_, &scd, &swapchain_);
    if (SUCCEEDED(hr) && swapchain_ != nullptr) {
        gdi_compatible_ = true;
    } else {
        scd.Flags = 0;
        hr = dxgi_factory->CreateSwapChain(device_, &scd, &swapchain_);
        gdi_compatible_ = false;
    }
    safe_release(dxgi_factory);
    if (FAILED(hr) || swapchain_ == nullptr) {
        diagnostic_ = "CreateSwapChain failed: hr=" + std::to_string(hr);
        return false;
    }

    if (!create_swapchain_render_target()) {
        return false;
    }

    if (!create_pipeline_shaders()) {
        return false;
    }

    return true;
}

bool ViewportD3D11Renderer::create_swapchain_render_target() {
    safe_release(render_target_view_);

    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer));
    if (FAILED(hr) || back_buffer == nullptr) {
        diagnostic_ = "swapchain GetBuffer failed: hr=" + std::to_string(hr);
        return false;
    }

    hr = device_->CreateRenderTargetView(back_buffer, nullptr, &render_target_view_);
    safe_release(back_buffer);
    if (FAILED(hr) || render_target_view_ == nullptr) {
        diagnostic_ = "CreateRenderTargetView failed: hr=" + std::to_string(hr);
        return false;
    }

    return true;
}

bool ViewportD3D11Renderer::create_pipeline_shaders() {
    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* error_blob = nullptr;

    HRESULT hr = D3DCompile(
        g_shader_hlsl, sizeof(g_shader_hlsl) - 1,
        nullptr, nullptr, nullptr,
        "VSMain", "vs_4_0", 0, 0, &vs_blob, &error_blob);

    if (FAILED(hr)) {
        if (error_blob != nullptr) {
            diagnostic_ = "Vertex Shader compilation error: " +
                std::string(static_cast<const char*>(error_blob->GetBufferPointer()));
            safe_release(error_blob);
        } else {
            diagnostic_ = "Vertex Shader compilation failed: hr=" + std::to_string(hr);
        }
        return false;
    }

    hr = D3DCompile(
        g_shader_hlsl, sizeof(g_shader_hlsl) - 1,
        nullptr, nullptr, nullptr,
        "PSMain", "ps_4_0", 0, 0, &ps_blob, &error_blob);

    if (FAILED(hr)) {
        safe_release(vs_blob);
        if (error_blob != nullptr) {
            diagnostic_ = "Pixel Shader compilation error: " +
                std::string(static_cast<const char*>(error_blob->GetBufferPointer()));
            safe_release(error_blob);
        } else {
            diagnostic_ = "Pixel Shader compilation failed: hr=" + std::to_string(hr);
        }
        return false;
    }

    safe_release(error_blob);

    hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader_);
    safe_release(vs_blob);
    if (FAILED(hr) || vertex_shader_ == nullptr) {
        safe_release(ps_blob);
        diagnostic_ = "CreateVertexShader failed: hr=" + std::to_string(hr);
        return false;
    }

    hr = device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader_);
    safe_release(ps_blob);
    if (FAILED(hr) || pixel_shader_ == nullptr) {
        diagnostic_ = "CreatePixelShader failed: hr=" + std::to_string(hr);
        return false;
    }

    D3D11_SAMPLER_DESC samp_desc{};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;

    hr = device_->CreateSamplerState(&samp_desc, &sampler_state_);
    if (FAILED(hr) || sampler_state_ == nullptr) {
        diagnostic_ = "CreateSamplerState failed: hr=" + std::to_string(hr);
        return false;
    }

    return true;
}

void ViewportD3D11Renderer::release_swapchain_resources() {
    if (context_ != nullptr) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    safe_release(render_target_view_);
}

void ViewportD3D11Renderer::release_all_resources() {
    release_swapchain_resources();
    safe_release(sampler_state_);
    safe_release(pixel_shader_);
    safe_release(vertex_shader_);
    safe_release(frame_srv_);
    safe_release(frame_texture_);
    safe_release(swapchain_);
    safe_release(context_);
    safe_release(device_);

    texture_width_ = 0;
    texture_height_ = 0;
    texture_dxgi_format_ = 0;
    texture_format_ = VideoPixelFormat::Unknown;
}

void ViewportD3D11Renderer::handle_device_lost() {
    release_all_resources();
    if (window_ != nullptr && IsWindow(window_) != FALSE && initialize_d3d11()) {
        active_backend_ = ViewportBackend::Direct3D11;
        diagnostic_ = "Direct3D 11 device recovered successfully after device lost.";
        metrics_.hardware_accelerated = true;
    } else {
        active_backend_ = ViewportBackend::GdiFallback;
        diagnostic_ = "Direct3D 11 device lost; transitioned to GDI fallback.";
        metrics_.hardware_accelerated = false;
    }
}

} // namespace fang::studio
