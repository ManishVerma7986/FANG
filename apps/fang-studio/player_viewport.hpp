#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "broker_protocol.hpp"
#include "studio_input_router.hpp"
#include "studio_session.hpp"
#include "video_transport.hpp"
#include "viewport_renderer.hpp"
#include "input_mapping_engine.hpp"

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
        std::shared_ptr<StudioVideoConsumer> video_consumer = nullptr,
        std::unique_ptr<IViewportRenderer> renderer = nullptr,
        std::shared_ptr<InputMappingEngine> mapping_engine = nullptr);
    PlayerViewport(const PlayerViewport&) = delete;
    PlayerViewport& operator=(const PlayerViewport&) = delete;
    ~PlayerViewport();

    HWND handle() const noexcept { return window_; }
    bool is_valid() const noexcept { return window_ != nullptr && IsWindow(window_) != FALSE; }

    void move(int x, int y, int width, int height, bool repaint = true);
    void show(bool visible = true);
    void update_runtime_state(const fang::broker::RuntimeStatusData& status, bool request_pending);
    void update_session_state(const StudioSessionSnapshot& session);
    void set_presentation_mode(bool embedded);
    bool is_embedded() const noexcept { return embedded_; }
    bool has_focus() const noexcept { return has_focus_; }
    void release_focus_to_parent();

    const fang::broker::RuntimeStatusData& cached_status() const noexcept { return cached_status_; }
    const StudioSessionSnapshot& session_state() const noexcept { return session_; }
    std::shared_ptr<StudioInputRouter> router() const noexcept { return router_; }
    std::shared_ptr<StudioVideoConsumer> video_consumer() const noexcept { return video_consumer_; }
    void set_video_consumer(std::shared_ptr<StudioVideoConsumer> consumer) noexcept { video_consumer_ = std::move(consumer); }

    std::shared_ptr<InputMappingEngine> mapping_engine() const noexcept { return mapping_engine_; }
    void set_mapping_engine(std::shared_ptr<InputMappingEngine> engine) noexcept { mapping_engine_ = std::move(engine); }

    IViewportRenderer* renderer() const noexcept { return renderer_.get(); }
    ViewportBackend active_backend() const noexcept {
        return renderer_ ? renderer_->active_backend() : ViewportBackend::None;
    }
    ViewportPresentationMetrics presentation_metrics() const {
        return renderer_ ? renderer_->metrics() : ViewportPresentationMetrics{};
    }

private:
    static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT handle_message(UINT message, WPARAM w_param, LPARAM l_param);
    void paint(HDC target_dc);

    HWND window_{};
    HWND parent_{};
    std::shared_ptr<StudioInputRouter> router_;
    fang::broker::RuntimeStatusData cached_status_;
    StudioSessionSnapshot session_;
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
    std::unique_ptr<IViewportRenderer> renderer_;
    std::shared_ptr<InputMappingEngine> mapping_engine_;
    POINT last_mouse_pt_{};
};

}
