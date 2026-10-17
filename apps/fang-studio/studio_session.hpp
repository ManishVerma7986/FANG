#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "supervisor_runtime_client.hpp"
#include "studio_input_router.hpp"
#include "adb_input_dispatcher.hpp"
#include "video_transport.hpp"
#include "viewport_renderer.hpp"
#include "input_mapping_engine.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fang::studio {

enum class StudioSessionPhase {
    NotStarted,
    Starting,
    ReadyAwaitingVideo,
    ActiveStreaming,
    Stopping,
    Stopped,
    Failed
};

constexpr std::string_view session_phase_name(StudioSessionPhase phase) noexcept {
    switch (phase) {
    case StudioSessionPhase::NotStarted:         return "NotStarted";
    case StudioSessionPhase::Starting:           return "Starting";
    case StudioSessionPhase::ReadyAwaitingVideo: return "ReadyAwaitingVideo";
    case StudioSessionPhase::ActiveStreaming:    return "ActiveStreaming";
    case StudioSessionPhase::Stopping:           return "Stopping";
    case StudioSessionPhase::Stopped:            return "Stopped";
    case StudioSessionPhase::Failed:             return "Failed";
    default:                                     return "Unknown";
    }
}

enum class StudioInputBindingState {
    Unbound,
    Verifying,
    Bound,
    AmbiguousOrMultiple,
    Unavailable
};

constexpr std::string_view input_binding_state_name(StudioInputBindingState state) noexcept {
    switch (state) {
    case StudioInputBindingState::Unbound:             return "Unbound";
    case StudioInputBindingState::Verifying:           return "Verifying";
    case StudioInputBindingState::Bound:               return "Bound";
    case StudioInputBindingState::AmbiguousOrMultiple: return "AmbiguousOrMultiple";
    case StudioInputBindingState::Unavailable:         return "Unavailable";
    default:                                           return "Unknown";
    }
}

enum class StudioVideoState {
    Disconnected,
    Connecting,
    AwaitingFirstFrame,
    Streaming,
    TransportError,
    Unavailable
};

constexpr std::string_view studio_video_state_name(StudioVideoState state) noexcept {
    switch (state) {
    case StudioVideoState::Disconnected:       return "Disconnected";
    case StudioVideoState::Connecting:         return "Connecting";
    case StudioVideoState::AwaitingFirstFrame: return "AwaitingFirstFrame";
    case StudioVideoState::Streaming:          return "Streaming";
    case StudioVideoState::TransportError:     return "TransportError";
    case StudioVideoState::Unavailable:        return "Unavailable";
    default:                                   return "Unknown";
    }
}

struct StudioSessionSnapshot {
    StudioSessionPhase phase{StudioSessionPhase::NotStarted};
    StudioInputBindingState input_state{StudioInputBindingState::Unbound};
    StudioVideoState video_state{StudioVideoState::Disconnected};
    ViewportBackend active_backend{ViewportBackend::None};

    std::string generation_id;
    std::string avd_name;
    std::string adb_serial;
    std::uint32_t process_id{};
    bool guest_ready{};
    bool is_ambiguous_target{};

    std::string diagnostic;
    std::string input_diagnostic;
    std::string video_diagnostic;

    std::size_t input_queue_depth{};
    std::size_t input_dispatched_count{};
    std::size_t input_failed_count{};
    std::size_t input_dropped_count{};

    std::size_t video_frames_received{};
    std::size_t video_frames_presented{};

    bool is_interactive() const noexcept {
        return (phase == StudioSessionPhase::ActiveStreaming || phase == StudioSessionPhase::ReadyAwaitingVideo) &&
               input_state == StudioInputBindingState::Bound;
    }

    std::wstring user_status_line() const;
    std::wstring input_status_line() const;
    std::wstring video_status_line() const;
    std::wstring backend_status_line() const;
};

class StudioSessionController final {
public:
    StudioSessionController(
        std::shared_ptr<StudioInputRouter> input_router,
        std::shared_ptr<AdbInputDispatcher> input_dispatcher,
        std::shared_ptr<StudioVideoConsumer> video_consumer,
        std::shared_ptr<SharedMemoryVideoTransport> video_transport,
        std::shared_ptr<InputMappingEngine> mapping_engine = nullptr);
    ~StudioSessionController();

    StudioSessionController(const StudioSessionController&) = delete;
    StudioSessionController& operator=(const StudioSessionController&) = delete;

    std::shared_ptr<InputMappingEngine> mapping_engine() const noexcept { return mapping_engine_; }
    void set_mapping_engine(std::shared_ptr<InputMappingEngine> engine) noexcept { mapping_engine_ = std::move(engine); }

    StudioSessionSnapshot update(
        const SupervisorRuntimeSnapshot& supervisor_snapshot,
        ViewportBackend active_backend,
        std::size_t frames_presented,
        const std::vector<std::string>& known_devices = {});

    void notify_shutdown();
    void reset_session();
    StudioSessionSnapshot snapshot() const;

private:
    void handle_session_change(const fang::broker::RuntimeStatusData& new_runtime);

    std::shared_ptr<StudioInputRouter> input_router_;
    std::shared_ptr<AdbInputDispatcher> input_dispatcher_;
    std::shared_ptr<StudioVideoConsumer> video_consumer_;
    std::shared_ptr<SharedMemoryVideoTransport> video_transport_;
    std::shared_ptr<InputMappingEngine> mapping_engine_;

    mutable std::mutex mutex_;
    StudioSessionSnapshot snapshot_;
    std::string current_generation_id_;
    std::uint32_t current_process_id_{};
    std::string current_avd_name_;
    std::string current_serial_;
    bool was_started_{};
};

} // namespace fang::studio
