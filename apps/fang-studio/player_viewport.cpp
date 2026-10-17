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
    std::shared_ptr<StudioVideoConsumer> video_consumer)
    : parent_(parent), router_(std::move(router)), video_consumer_(std::move(video_consumer)) {
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
}

PlayerViewport::~PlayerViewport() {
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

    HDC mem_dc = CreateCompatibleDC(target_dc);
    HBITMAP mem_bitmap = CreateCompatibleBitmap(target_dc, width, height);
    auto* old_bitmap = SelectObject(mem_dc, mem_bitmap);

    // 1. Fill container background
    HBRUSH bg_brush = CreateSolidBrush(RGB(24, 27, 34));
    FillRect(mem_dc, &client_rect, bg_brush);
    DeleteObject(bg_brush);

    // 2. Focus banner and border
    const RECT header_rect{client_rect.left + 4, client_rect.top + 4, client_rect.right - 4, client_rect.top + 26};
    SetBkMode(mem_dc, TRANSPARENT);
    SelectObject(mem_dc, font_small_);
    if (has_focus_) {
        SetTextColor(mem_dc, RGB(0, 190, 255));
        DrawTextW(mem_dc, L"[ Active Focus  |  Press Esc to return to host controls ]", -1,
            const_cast<LPRECT>(&header_rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        SetTextColor(mem_dc, RGB(140, 150, 165));
        DrawTextW(mem_dc, L"[ Viewport Unfocused  |  Click inside to interact ]", -1,
            const_cast<LPRECT>(&header_rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 3. Calculate device screen rectangle (leave space for header/footer and bezel)
    RECT inner_area{client_rect.left + 8, client_rect.top + 28, client_rect.right - 8, client_rect.bottom - 28};
    RECT screen_rect = calculate_display_viewport_rect(inner_area, {});

    // 4. Draw outer phone bezel
    RECT bezel_rect{screen_rect.left - 6, screen_rect.top - 6, screen_rect.right + 6, screen_rect.bottom + 6};
    HBRUSH bezel_brush = CreateSolidBrush(RGB(40, 44, 52));
    HPEN bezel_pen = CreatePen(PS_SOLID, 1, has_focus_ ? RGB(0, 162, 237) : RGB(65, 72, 85));
    SelectObject(mem_dc, bezel_brush);
    SelectObject(mem_dc, bezel_pen);
    RoundRect(mem_dc, bezel_rect.left, bezel_rect.top, bezel_rect.right, bezel_rect.bottom, 12, 12);
    DeleteObject(bezel_brush);
    DeleteObject(bezel_pen);

    // 5. Draw screen area
    const COLORREF screen_bg = background_color_for_state(cached_status_.state);
    HBRUSH screen_brush = CreateSolidBrush(screen_bg);
    FillRect(mem_dc, &screen_rect, screen_brush);
    DeleteObject(screen_brush);

    // 6. Draw state-dependent screen content
    SelectObject(mem_dc, font_title_);
    SetBkMode(mem_dc, TRANSPARENT);

    const int screen_width = screen_rect.right - screen_rect.left;
    const int screen_height = screen_rect.bottom - screen_rect.top;

    std::shared_ptr<const VideoFrame> live_frame;
    if (video_consumer_ != nullptr && embedded_) {
        live_frame = video_consumer_->current_frame();
    }

    if (cached_status_.state == fang::broker::RuntimeLifecycleState::Ready && cached_status_.guest_ready) {
        if (live_frame && live_frame->is_valid() && screen_width > 0 && screen_height > 0) {
            // Live video frame presentation via StretchDIBits
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = static_cast<LONG>(live_frame->width);
            bmi.bmiHeader.biHeight = -static_cast<LONG>(live_frame->height); // top-down DIB
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            const void* pixel_data = live_frame->pixels->data();
            std::vector<std::uint8_t> converted_pixels;
            if (live_frame->format == VideoPixelFormat::RGBA8888) {
                converted_pixels.resize(live_frame->pixels->size());
                const auto* src = live_frame->pixels->data();
                auto* dst = converted_pixels.data();
                const std::size_t total_pixels = static_cast<std::size_t>(live_frame->width) * live_frame->height;
                for (std::size_t i = 0; i < total_pixels; ++i) {
                    dst[i * 4 + 0] = src[i * 4 + 2]; // B
                    dst[i * 4 + 1] = src[i * 4 + 1]; // G
                    dst[i * 4 + 2] = src[i * 4 + 0]; // R
                    dst[i * 4 + 3] = src[i * 4 + 3]; // A
                }
                pixel_data = converted_pixels.data();
            }

            SetStretchBltMode(mem_dc, COLORONCOLOR);
            StretchDIBits(mem_dc,
                screen_rect.left, screen_rect.top,
                screen_width, screen_height,
                0, 0, live_frame->width, live_frame->height,
                pixel_data, &bmi, DIB_RGB_COLORS, SRCCOPY);

            // Watermark & transport diagnostic overlay
            SelectObject(mem_dc, font_small_);
            SetBkMode(mem_dc, TRANSPARENT);
            SetTextColor(mem_dc, RGB(0, 255, 180));
            std::wstring live_badge = L"[LIVE STREAM] " + std::to_wstring(live_frame->width) + L"x" +
                std::to_wstring(live_frame->height) + L" | Frame #" + std::to_wstring(live_frame->sequence);
            RECT live_rect{screen_rect.left + 8, screen_rect.top + 8, screen_rect.right - 8, screen_rect.top + 26};
            DrawTextW(mem_dc, live_badge.c_str(), -1, &live_rect, DT_LEFT | DT_SINGLELINE);

            // Touch feedback indicator on top of video frame
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

            // Screen footer info
            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(180, 230, 250));
            RECT footer_info{screen_rect.left + 10, screen_rect.bottom - 36, screen_rect.right - 10, screen_rect.bottom - 8};
            DrawTextW(mem_dc, L"Video Transport: ACTIVE  |  Touch & Input: ACTIVE\r\nEmbedded Live Surface", -1,
                &footer_info, DT_CENTER);
        } else {
            // Active Ready screen fallback placeholder
            SetTextColor(mem_dc, RGB(0, 220, 180));
            RECT title_rect{screen_rect.left + 10, screen_rect.top + 16, screen_rect.right - 10, screen_rect.top + 40};
            DrawTextW(mem_dc, L"Android Reference Guest", -1, &title_rect, DT_CENTER | DT_SINGLELINE);

            SelectObject(mem_dc, font_body_);
            SetTextColor(mem_dc, RGB(180, 210, 230));
            RECT sub_rect{screen_rect.left + 10, screen_rect.top + 42, screen_rect.right - 10, screen_rect.top + 64};
            std::wstring serial_info = L"Online | " + std::wstring(cached_status_.adb_serial.begin(), cached_status_.adb_serial.end());
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

            // Screen footer info
            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(120, 170, 190));
            RECT footer_info{screen_rect.left + 10, screen_rect.bottom - 36, screen_rect.right - 10, screen_rect.bottom - 8};
            DrawTextW(mem_dc, L"Touch & Keyboard Forwarding: ACTIVE\r\nResolution: 1080x1920 logical (awaiting video transport)", -1,
                &footer_info, DT_CENTER);
        }
    } else {
        // Stopped, Booting, Stopping, or Failed
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

    // 7. Viewport footer info
    RECT bottom_rect{client_rect.left + 8, client_rect.bottom - 24, client_rect.right - 8, client_rect.bottom - 4};
    SelectObject(mem_dc, font_small_);
    SetTextColor(mem_dc, RGB(110, 120, 135));
    DrawTextW(mem_dc, L"Stage 2D Video Transport and Presentation", -1,
        const_cast<LPRECT>(&bottom_rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(target_dc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bitmap);
    DeleteObject(mem_bitmap);
    DeleteDC(mem_dc);
}

}
