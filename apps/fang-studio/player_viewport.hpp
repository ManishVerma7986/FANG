#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "broker_protocol.hpp"
#include "studio_input_router.hpp"
#include "video_transport.hpp"

#include <memory>
#include <optional>
#include <string>

namespace fang::studio {

inline constexpr wchar_t player_viewport_class_name[] = L"FangStudioPlayerViewport";

class PlayerViewport final {
public:
    static bool register_class(HINSTANCE instance);

    PlayerViewport(HWND parent, HINSTANCE instance, int control_id,
        std::shared_ptr<StudioInputRouter> router,
        std::shared_ptr<StudioVideoConsumer> video_consumer = nullptr);
    PlayerViewport(const PlayerViewport&) = delete;
    PlayerViewport& operator=(const PlayerViewport&) = delete;
    ~PlayerViewport();

    HWND handle() const noexcept { return window_; }
    bool is_valid() const noexcept { return window_ != nullptr && IsWindow(window_) != FALSE; }

    void move(int x, int y, int width, int height, bool repaint = true);
    void show(bool visible = true);
    void update_runtime_state(const fang::broker::RuntimeStatusData& status, bool request_pending);
    void set_presentation_mode(bool embedded);
    bool is_embedded() const noexcept { return embedded_; }
    bool has_focus() const noexcept { return has_focus_; }
    void release_focus_to_parent();

    const fang::broker::RuntimeStatusData& cached_status() const noexcept { return cached_status_; }
    std::shared_ptr<StudioInputRouter> router() const noexcept { return router_; }
    std::shared_ptr<StudioVideoConsumer> video_consumer() const noexcept { return video_consumer_; }
    void set_video_consumer(std::shared_ptr<StudioVideoConsumer> consumer) noexcept { video_consumer_ = std::move(consumer); }

private:
    static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT handle_message(UINT message, WPARAM w_param, LPARAM l_param);
    void paint(HDC target_dc);

    HWND window_{};
    HWND parent_{};
    std::shared_ptr<StudioInputRouter> router_;
    fang::broker::RuntimeStatusData cached_status_;
    bool request_pending_{};
    bool embedded_{true};
    bool has_focus_{};
    bool dragging_{};
    POINT drag_start_pt_{};
    std::optional<POINT> active_touch_pt_;
    HFONT font_title_{};
    HFONT font_body_{};
    HFONT font_small_{};
    std::shared_ptr<StudioVideoConsumer> video_consumer_;
};

}
