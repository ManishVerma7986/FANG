#include "player_viewport.hpp"

#include <algorithm>
#include <string>

namespace fang::studio {

namespace {

COLORREF background_color_for_state(fang::broker::RuntimeLifecycleState state) noexcept {
    switch (state) {
    case fang::broker::RuntimeLifecycleState::Stopped:
        return RGB(20, 24, 30);
    case fang::broker::RuntimeLifecycleState::Starting:
    case fang::broker::RuntimeLifecycleState::ProcessCreated:
    case fang::broker::RuntimeLifecycleState::OwnershipVerified:
    case fang::broker::RuntimeLifecycleState::GuestBooting:
        return RGB(16, 32, 54);
    case fang::broker::RuntimeLifecycleState::Ready:
        return RGB(10, 36, 48);
    case fang::broker::RuntimeLifecycleState::Stopping:
        return RGB(42, 32, 18);
    case fang::broker::RuntimeLifecycleState::RecoveryRequired:
    case fang::broker::RuntimeLifecycleState::Failed:
        return RGB(50, 18, 24);
    }
    return RGB(20, 24, 30);
}

}

bool PlayerViewport::register_class(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(instance, player_viewport_class_name, &existing) != 0) {
        return true;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = PlayerViewport::window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr; // Handled in WM_PAINT
    window_class.lpszClassName = player_viewport_class_name;

    return RegisterClassExW(&window_class) != 0;
}

PlayerViewport::PlayerViewport(HWND parent, HINSTANCE instance, int control_id,
    std::shared_ptr<StudioInputRouter> router,
    std::shared_ptr<StudioVideoConsumer> video_consumer,
    std::unique_ptr<IViewportRenderer> renderer)
    : parent_(parent), router_(std::move(router)),
      video_consumer_(std::move(video_consumer)),
      renderer_(std::move(renderer)) {
    register_class(instance);

    font_title_ = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    font_body_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    font_small_ = CreateFontW(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    window_ = CreateWindowExW(
        0,
        player_viewport_class_name,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN,
        0, 0, 0, 0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
        instance,
        this);

    if (!renderer_) {
        renderer_ = std::make_unique<ViewportD3D11Renderer>();
    }
    if (window_ != nullptr && !renderer_->is_initialized()) {
        renderer_->initialize(window_);
    }
}

PlayerViewport::~PlayerViewport() {
    if (renderer_) {
        renderer_->shutdown();
    }
    if (window_ != nullptr && IsWindow(window_) != FALSE) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_title_ != nullptr) DeleteObject(font_title_);
    if (font_body_ != nullptr) DeleteObject(font_body_);
    if (font_small_ != nullptr) DeleteObject(font_small_);
}

void PlayerViewport::move(int x, int y, int width, int height, bool repaint) {
    if (window_ != nullptr) {
        MoveWindow(window_, x, y, width, height, repaint ? TRUE : FALSE);
        if (renderer_) {
            renderer_->resize(width, height);
        }
    }
}

void PlayerViewport::show(bool visible) {
    if (window_ != nullptr) {
        ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
    }
}

void PlayerViewport::update_runtime_state(const fang::broker::RuntimeStatusData& status,
    bool request_pending) {
    const bool state_changed = cached_status_.state != status.state ||
        cached_status_.guest_ready != status.guest_ready ||
        request_pending_ != request_pending;

    cached_status_ = status;
    request_pending_ = request_pending;

    if (router_) {
        router_->set_adb_serial(status.adb_serial);
        router_->set_guest_ready(status.state == fang::broker::RuntimeLifecycleState::Ready && status.guest_ready);
    }

    if (!status.guest_ready && window_ != nullptr && GetCapture() == window_) {
        ReleaseCapture();
        active_touch_pt_.reset();
        dragging_ = false;
    }

    if (video_consumer_ && (!status.guest_ready || status.state != fang::broker::RuntimeLifecycleState::Ready)) {
        video_consumer_->clear();
    }

    if (state_changed && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void PlayerViewport::update_session_state(const StudioSessionSnapshot& session) {
    session_ = session;
    fang::broker::RuntimeStatusData status;
    status.generation_id = session.generation_id;
    status.avd_name = session.avd_name;
    status.adb_serial = session.adb_serial;
    status.process_id = session.process_id;
    status.guest_ready = session.guest_ready;
    status.diagnostic = session.diagnostic;

    switch (session.phase) {
    case StudioSessionPhase::NotStarted:
    case StudioSessionPhase::Stopped:
        status.state = fang::broker::RuntimeLifecycleState::Stopped;
        break;
    case StudioSessionPhase::Starting:
        status.state = fang::broker::RuntimeLifecycleState::Starting;
        break;
    case StudioSessionPhase::ReadyAwaitingVideo:
    case StudioSessionPhase::ActiveStreaming:
        status.state = fang::broker::RuntimeLifecycleState::Ready;
        break;
    case StudioSessionPhase::Stopping:
        status.state = fang::broker::RuntimeLifecycleState::Stopping;
        break;
    case StudioSessionPhase::Failed:
        status.state = fang::broker::RuntimeLifecycleState::Failed;
        break;
    }

    update_runtime_state(status, false);
}

void PlayerViewport::set_presentation_mode(bool embedded) {
    if (embedded_ != embedded) {
        embedded_ = embedded;
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

void PlayerViewport::release_focus_to_parent() {
    has_focus_ = false;
    if (window_ != nullptr && GetCapture() == window_) {
        ReleaseCapture();
        active_touch_pt_.reset();
        dragging_ = false;
    }
    if (parent_ != nullptr) {
        SetFocus(parent_);
    }
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

LRESULT CALLBACK PlayerViewport::window_procedure(HWND window, UINT message,
    WPARAM w_param, LPARAM l_param) {
    auto* self = reinterpret_cast<PlayerViewport*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        self = static_cast<PlayerViewport*>(create->lpCreateParams);
        if (self != nullptr) {
            self->window_ = window;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(window, message, w_param, l_param);
    }

    if (self != nullptr) {
        return self->handle_message(message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT PlayerViewport::handle_message(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window_, &ps);
        paint(dc);
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // Prevent flicker; painted in WM_PAINT
    case WM_SIZE: {
        const int w = LOWORD(l_param);
        const int h = HIWORD(l_param);
        if (renderer_) {
            renderer_->resize(w, h);
        }
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_SETFOCUS:
        has_focus_ = true;
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        has_focus_ = false;
        if (GetCapture() == window_) {
            ReleaseCapture();
            active_touch_pt_.reset();
            dragging_ = false;
        }
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        const POINT pt{static_cast<short>(LOWORD(l_param)), static_cast<short>(HIWORD(l_param))};
        RECT client_rect{};
        GetClientRect(window_, &client_rect);

        const auto mapped = map_client_to_display(pt, client_rect, {});
        if (mapped.has_value()) {
            SetCapture(window_);
            active_touch_pt_ = mapped;
            dragging_ = false;
            drag_start_pt_ = pt;

            if (router_) {
                router_->route_pointer_down(pt, client_rect, {});
            }
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (GetCapture() == window_) {
            const POINT pt{static_cast<short>(LOWORD(l_param)), static_cast<short>(HIWORD(l_param))};
            RECT client_rect{};
            GetClientRect(window_, &client_rect);

            const int dx = pt.x - drag_start_pt_.x;
            const int dy = pt.y - drag_start_pt_.y;
            if (!dragging_ && (dx * dx + dy * dy >= 16)) {
                dragging_ = true;
            }

            const auto mapped = map_client_to_display(pt, client_rect, {});
            if (mapped.has_value()) {
                active_touch_pt_ = mapped;
                InvalidateRect(window_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (GetCapture() == window_) {
            const POINT pt{static_cast<short>(LOWORD(l_param)), static_cast<short>(HIWORD(l_param))};
            RECT client_rect{};
            GetClientRect(window_, &client_rect);

            if (router_) {
                if (dragging_) {
                    router_->route_pointer_drag(drag_start_pt_, pt, client_rect, 100, {});
                } else {
                    router_->route_pointer_up(pt, client_rect, {});
                }
            }

            active_touch_pt_.reset();
            dragging_ = false;
            ReleaseCapture();
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        active_touch_pt_.reset();
        dragging_ = false;
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (w_param == VK_ESCAPE) {
            release_focus_to_parent();
            return 0;
        }
        if (router_) {
            router_->route_key_down(w_param);
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, w_param, l_param);
}

void PlayerViewport::paint(HDC target_dc) {
    RECT client_rect{};
    GetClientRect(window_, &client_rect);
    const int width = client_rect.right - client_rect.left;
    const int height = client_rect.bottom - client_rect.top;
    if (width <= 0 || height <= 0) return;

    // Check if we have a live streaming frame
    std::shared_ptr<const VideoFrame> live_frame;
    if (video_consumer_ != nullptr && embedded_) {
        live_frame = video_consumer_->current_frame();
    }

    RECT inner_area{client_rect.left + 8, client_rect.top + 28, client_rect.right - 8, client_rect.bottom - 28};
    RECT screen_rect = calculate_display_viewport_rect(inner_area, {});
    const int screen_width = screen_rect.right - screen_rect.left;
    const int screen_height = screen_rect.bottom - screen_rect.top;

    const bool is_ready = (cached_status_.state == fang::broker::RuntimeLifecycleState::Ready && cached_status_.guest_ready);
    const bool can_render_video = is_ready && live_frame && live_frame->is_valid() && screen_width > 0 && screen_height > 0;

    // Overlay painter lambda to draw bezel, banners, watermark badge, touch feedback, and footer
    auto draw_overlays = [&](HDC dc) {
        // 1. Focus banner and border
        const RECT header_rect{client_rect.left + 4, client_rect.top + 4, client_rect.right - 4, client_rect.top + 26};
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, font_small_);
        if (has_focus_) {
            SetTextColor(dc, RGB(0, 190, 255));
            DrawTextW(dc, L"[ Active Focus  |  Press Esc to return to host controls ]", -1,
                const_cast<LPRECT>(&header_rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            SetTextColor(dc, RGB(140, 150, 165));
            DrawTextW(dc, L"[ Viewport Unfocused  |  Click inside to interact ]", -1,
                const_cast<LPRECT>(&header_rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // 2. Draw outer phone bezel
        RECT bezel_rect{screen_rect.left - 6, screen_rect.top - 6, screen_rect.right + 6, screen_rect.bottom + 6};
        HBRUSH bezel_brush = CreateSolidBrush(RGB(40, 44, 52));
        HPEN bezel_pen = CreatePen(PS_SOLID, 1, has_focus_ ? RGB(0, 162, 237) : RGB(65, 72, 85));
        SelectObject(dc, bezel_brush);
        SelectObject(dc, bezel_pen);
        RoundRect(dc, bezel_rect.left, bezel_rect.top, bezel_rect.right, bezel_rect.bottom, 12, 12);
        DeleteObject(bezel_brush);
        DeleteObject(bezel_pen);

        if (can_render_video) {
            // Watermark & transport diagnostic overlay
            SelectObject(dc, font_small_);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0, 255, 180));
            std::wstring backend_badge = renderer_ && renderer_->active_backend() == ViewportBackend::Direct3D11
                ? L" | D3D11 HW" : L" | GDI Fallback";
            std::wstring live_badge = L"[LIVE STREAM] " + std::to_wstring(live_frame->width) + L"x" +
                std::to_wstring(live_frame->height) + L" | Frame #" + std::to_wstring(live_frame->sequence) + backend_badge;
            RECT live_rect{screen_rect.left + 8, screen_rect.top + 8, screen_rect.right - 8, screen_rect.top + 26};
            DrawTextW(dc, live_badge.c_str(), -1, &live_rect, DT_LEFT | DT_SINGLELINE);

            // Touch feedback indicator on top of video frame
            if (active_touch_pt_.has_value()) {
                const double norm_x = static_cast<double>(active_touch_pt_->x) / 1080.0;
                const double norm_y = static_cast<double>(active_touch_pt_->y) / 1920.0;
                const int touch_screen_x = screen_rect.left + static_cast<int>(norm_x * screen_width);
                const int touch_screen_y = screen_rect.top + static_cast<int>(norm_y * screen_height);

                HPEN touch_pen = CreatePen(PS_SOLID, 2, RGB(0, 255, 180));
                HBRUSH touch_brush = CreateSolidBrush(RGB(0, 180, 140));
                SelectObject(dc, touch_pen);
                SelectObject(dc, touch_brush);
                Ellipse(dc, touch_screen_x - 12, touch_screen_y - 12, touch_screen_x + 12, touch_screen_y + 12);
                DeleteObject(touch_pen);
                DeleteObject(touch_brush);
            }

            // Screen footer info with host upload/present metrics
            SelectObject(dc, font_small_);
            SetTextColor(dc, RGB(180, 230, 250));
            RECT footer_info{screen_rect.left + 10, screen_rect.bottom - 44, screen_rect.right - 10, screen_rect.bottom - 8};

            std::wstring metrics_line = L"";
            if (renderer_) {
                const auto m = renderer_->metrics();
                metrics_line = L"\r\nUpload: " + std::to_wstring(m.last_upload_duration_us) + L" us | Present: " +
                    std::to_wstring(m.last_present_duration_us) + L" us";
            }
            std::wstring backend_title = renderer_ && renderer_->active_backend() == ViewportBackend::Direct3D11
                ? L"Presentation: Direct3D 11 (Hardware Accelerated)"
                : L"Presentation: GDI (Software Fallback)";

            std::wstring footer_text = backend_title + metrics_line;
            DrawTextW(dc, footer_text.c_str(), -1, &footer_info, DT_CENTER);
        }

        // Viewport footer info
        RECT bottom_rect{client_rect.left + 8, client_rect.bottom - 24, client_rect.right - 8, client_rect.bottom - 4};
        SelectObject(dc, font_small_);
        SetTextColor(dc, RGB(110, 120, 135));
        std::wstring stage_footer = renderer_ && renderer_->active_backend() == ViewportBackend::Direct3D11
            ? L"Stage 4 Integrated Studio Viewport | Direct3D 11 (Hardware Accelerated)"
            : L"Stage 4 Integrated Studio Viewport | GDI (Software Fallback)";
        DrawTextW(dc, stage_footer.c_str(), -1, const_cast<LPRECT>(&bottom_rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    if (can_render_video && renderer_ && renderer_->active_backend() == ViewportBackend::Direct3D11) {
        if (renderer_->present_frame(*live_frame, screen_rect, draw_overlays, nullptr)) {
            return;
        }
    }

    // GDI Rendering Path (either when fallback is active, non-video state, or D3D11 unavailable)
    HDC mem_dc = CreateCompatibleDC(target_dc);
    HBITMAP mem_bitmap = CreateCompatibleBitmap(target_dc, width, height);
    auto* old_bitmap = SelectObject(mem_dc, mem_bitmap);

    // 1. Fill container background
    HBRUSH bg_brush = CreateSolidBrush(RGB(24, 27, 34));
    FillRect(mem_dc, &client_rect, bg_brush);
    DeleteObject(bg_brush);

    // 2. Draw screen background
    const COLORREF screen_bg = background_color_for_state(cached_status_.state);
    HBRUSH screen_brush = CreateSolidBrush(screen_bg);
    FillRect(mem_dc, &screen_rect, screen_brush);
    DeleteObject(screen_brush);

    if (can_render_video) {
        // Present via renderer GDI fallback path
        if (renderer_) {
            renderer_->present_frame(*live_frame, screen_rect, draw_overlays, mem_dc);
        }
    } else if (is_ready) {
        // Active Ready screen / Waiting for first video frame
        draw_overlays(mem_dc);
        SelectObject(mem_dc, font_title_);
        SetTextColor(mem_dc, RGB(0, 220, 180));
        RECT title_rect{screen_rect.left + 10, screen_rect.top + 16, screen_rect.right - 10, screen_rect.top + 40};
        DrawTextW(mem_dc, L"Android Reference Guest", -1, &title_rect, DT_CENTER | DT_SINGLELINE);

        SelectObject(mem_dc, font_body_);
        SetTextColor(mem_dc, RGB(180, 210, 230));
        RECT sub_rect{screen_rect.left + 10, screen_rect.top + 42, screen_rect.right - 10, screen_rect.top + 64};
        std::wstring serial_info = L"Online | Target: " + std::wstring(cached_status_.adb_serial.begin(), cached_status_.adb_serial.end());
        DrawTextW(mem_dc, serial_info.c_str(), -1, &sub_rect, DT_CENTER | DT_SINGLELINE);

        // Grid lines to visualize touch surface
        HPEN grid_pen = CreatePen(PS_DOT, 1, RGB(20, 50, 68));
        SelectObject(mem_dc, grid_pen);
        for (int y = screen_rect.top + 80; y < screen_rect.bottom - 60; y += 40) {
            MoveToEx(mem_dc, screen_rect.left + 10, y, nullptr);
            LineTo(mem_dc, screen_rect.right - 10, y);
        }
        for (int x = screen_rect.left + 30; x < screen_rect.right; x += 40) {
            MoveToEx(mem_dc, x, screen_rect.top + 80, nullptr);
            LineTo(mem_dc, x, screen_rect.bottom - 60);
        }
        DeleteObject(grid_pen);

        // Center message indicating first frame wait
        RECT center_box{screen_rect.left + 12, screen_rect.top + screen_height / 3,
            screen_rect.right - 12, screen_rect.top + screen_height / 3 + 30};
        SelectObject(mem_dc, font_title_);
        SetTextColor(mem_dc, RGB(255, 200, 70));
        DrawTextW(mem_dc, L"[WAITING FOR FIRST VIDEO FRAME]", -1, &center_box, DT_CENTER | DT_SINGLELINE);

        SelectObject(mem_dc, font_body_);
        SetTextColor(mem_dc, RGB(160, 185, 210));
        RECT center_hint{screen_rect.left + 12, screen_rect.top + screen_height / 3 + 32,
            screen_rect.right - 12, screen_rect.top + screen_height / 3 + 80};
        DrawTextW(mem_dc, L"Runtime is ready. Shared-memory video transport attached.", -1, &center_hint, DT_CENTER | DT_WORDBREAK);

        if (session_.input_state == StudioInputBindingState::AmbiguousOrMultiple) {
            RECT ambig_rect{screen_rect.left + 10, screen_rect.top + screen_height / 2 + 30,
                screen_rect.right - 10, screen_rect.top + screen_height / 2 + 70};
            SetTextColor(mem_dc, RGB(255, 100, 100));
            DrawTextW(mem_dc, L"[INPUT BLOCKED: Ambiguous ADB Target]", -1, &ambig_rect, DT_CENTER | DT_SINGLELINE);
        }

        // Touch feedback indicator
        if (active_touch_pt_.has_value()) {
            const double norm_x = static_cast<double>(active_touch_pt_->x) / 1080.0;
            const double norm_y = static_cast<double>(active_touch_pt_->y) / 1920.0;
            const int touch_screen_x = screen_rect.left + static_cast<int>(norm_x * screen_width);
            const int touch_screen_y = screen_rect.top + static_cast<int>(norm_y * screen_height);

            HPEN touch_pen = CreatePen(PS_SOLID, 2, RGB(0, 255, 180));
            HBRUSH touch_brush = CreateSolidBrush(RGB(0, 180, 140));
            SelectObject(mem_dc, touch_pen);
            SelectObject(mem_dc, touch_brush);
            Ellipse(mem_dc, touch_screen_x - 12, touch_screen_y - 12, touch_screen_x + 12, touch_screen_y + 12);
            DeleteObject(touch_pen);
            DeleteObject(touch_brush);
        }

        SelectObject(mem_dc, font_small_);
        SetTextColor(mem_dc, RGB(120, 170, 190));
        RECT footer_info{screen_rect.left + 10, screen_rect.bottom - 36, screen_rect.right - 10, screen_rect.bottom - 8};
        std::wstring input_line = session_.input_state == StudioInputBindingState::Bound
            ? L"Touch & Keyboard Forwarding: ACTIVE"
            : (session_.input_state == StudioInputBindingState::AmbiguousOrMultiple
                ? L"Touch & Keyboard Forwarding: BLOCKED (Ambiguous Target)"
                : L"Touch & Keyboard Forwarding: UNAVAILABLE");
        DrawTextW(mem_dc, (input_line + L"\r\nResolution: 1080x1920 logical (awaiting video transport)").c_str(), -1,
            &footer_info, DT_CENTER);
    } else {
        // Stopped, Booting, Stopping, or Failed
        draw_overlays(mem_dc);
        SetTextColor(mem_dc, RGB(220, 225, 235));
        RECT title_rect{screen_rect.left + 10, screen_rect.top + screen_height / 3,
            screen_rect.right - 10, screen_rect.top + screen_height / 3 + 30};

        std::wstring state_label;
        std::wstring hint_label;
        if (cached_status_.state == fang::broker::RuntimeLifecycleState::Stopped) {
            state_label = L"Reference Runtime: Stopped";
            hint_label = L"Select an installed AVD and click Start";
        } else if (cached_status_.state == fang::broker::RuntimeLifecycleState::Stopping) {
            state_label = L"Runtime Stopping...";
            hint_label = L"Graceful shutdown in progress";
        } else if (cached_status_.state == fang::broker::RuntimeLifecycleState::Failed ||
                   cached_status_.state == fang::broker::RuntimeLifecycleState::RecoveryRequired) {
            state_label = L"Runtime Failed";
            hint_label = std::wstring(cached_status_.diagnostic.begin(), cached_status_.diagnostic.end());
            if (hint_label.empty()) hint_label = L"Supervisor ownership retained";
        } else {
            state_label = L"Booting Android Guest...";
            hint_label = L"Awaiting Android ADB readiness...";
        }

        DrawTextW(mem_dc, state_label.c_str(), -1, &title_rect, DT_CENTER | DT_SINGLELINE);

        SelectObject(mem_dc, font_body_);
        SetTextColor(mem_dc, RGB(150, 160, 175));
        RECT hint_rect{screen_rect.left + 12, title_rect.bottom + 8,
            screen_rect.right - 12, title_rect.bottom + 60};
        DrawTextW(mem_dc, hint_label.c_str(), -1, &hint_rect, DT_CENTER | DT_WORDBREAK);
    }

    BitBlt(target_dc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bitmap);
    DeleteObject(mem_bitmap);
    DeleteDC(mem_dc);
}

}
