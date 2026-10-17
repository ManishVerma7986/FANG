#include "studio_session.hpp"

#include <algorithm>
#include <sstream>

namespace fang::studio {

namespace {

std::wstring to_wide_str(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), count);
    return result;
}

} // namespace

std::wstring StudioSessionSnapshot::user_status_line() const {
    std::wstring line;
    switch (phase) {
    case StudioSessionPhase::NotStarted:
        line = L"Session: Not Started | Select an installed AVD and click Start";
        break;
    case StudioSessionPhase::Starting:
        line = L"Session: Starting | AVD: " + to_wide_str(avd_name) +
               (process_id != 0 ? L" | PID=" + std::to_wstring(process_id) : L"") +
               L" | Awaiting guest readiness...";
        break;
    case StudioSessionPhase::ReadyAwaitingVideo:
        line = L"Session: Ready | Target: " + to_wide_str(adb_serial) +
               L" | Waiting for first video frame...";
        break;
    case StudioSessionPhase::ActiveStreaming:
        line = L"Session: Active Stream | Target: " + to_wide_str(adb_serial) +
               L" | Presenter: " + backend_status_line();
        break;
    case StudioSessionPhase::Stopping:
        line = L"Session: Stopping | Releasing runtime, transport, and input resources...";
        break;
    case StudioSessionPhase::Stopped:
        line = L"Session: Stopped | Reference runtime terminated cleanly";
        break;
    case StudioSessionPhase::Failed:
        line = L"Session: Failed | " + (diagnostic.empty() ? L"Runtime error occurred" : to_wide_str(diagnostic));
        break;
    }

    if (input_state == StudioInputBindingState::AmbiguousOrMultiple) {
        line += L" [INPUT BLOCKED: Ambiguous target]";
    } else if (input_state == StudioInputBindingState::Unavailable) {
        line += L" [INPUT UNAVAILABLE]";
    }

    return line;
}

std::wstring StudioSessionSnapshot::input_status_line() const {
    switch (input_state) {
    case StudioInputBindingState::Bound:
        return L"Input Target: " + to_wide_str(adb_serial) + L" (Verified & Bound)";
    case StudioInputBindingState::AmbiguousOrMultiple:
        return L"Input Target: Multiple connected devices detected; binding rejected for safety";
    case StudioInputBindingState::Unavailable:
        return L"Input Target: Unavailable (no authoritative device serial)";
    case StudioInputBindingState::Verifying:
        return L"Input Target: Verifying runtime identity...";
    case StudioInputBindingState::Unbound:
    default:
        return L"Input Target: Unbound (runtime not ready)";
    }
}

std::wstring StudioSessionSnapshot::video_status_line() const {
    switch (video_state) {
    case StudioVideoState::Streaming:
        return L"Video: Streaming (" + std::to_wstring(video_frames_presented) + L" frames presented)";
    case StudioVideoState::AwaitingFirstFrame:
        return L"Video: Connected to shared memory transport; awaiting first frame";
    case StudioVideoState::Connecting:
        return L"Video: Attaching to guest shared memory transport...";
    case StudioVideoState::TransportError:
        return L"Video: Transport error (" + to_wide_str(video_diagnostic) + L")";
    case StudioVideoState::Unavailable:
        return L"Video: Transport unavailable";
    case StudioVideoState::Disconnected:
    default:
        return L"Video: Disconnected";
    }
}

std::wstring StudioSessionSnapshot::backend_status_line() const {
    switch (active_backend) {
    case ViewportBackend::Direct3D11:
        return L"Direct3D 11 (Hardware Accelerated)";
    case ViewportBackend::GdiFallback:
        return L"GDI (Software Fallback)";
    case ViewportBackend::None:
    default:
        return L"None";
    }
}

StudioSessionController::StudioSessionController(
    std::shared_ptr<StudioInputRouter> input_router,
    std::shared_ptr<AdbInputDispatcher> input_dispatcher,
    std::shared_ptr<StudioVideoConsumer> video_consumer,
    std::shared_ptr<SharedMemoryVideoTransport> video_transport)
    : input_router_(std::move(input_router)),
      input_dispatcher_(std::move(input_dispatcher)),
      video_consumer_(std::move(video_consumer)),
      video_transport_(std::move(video_transport)) {}

StudioSessionController::~StudioSessionController() {
    notify_shutdown();
}

void StudioSessionController::notify_shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (input_dispatcher_) {
        input_dispatcher_->cancel_pending();
        input_dispatcher_->unbind_session();
    }
    if (input_router_) {
        input_router_->set_guest_ready(false);
        input_router_->set_adb_serial("");
        input_router_->clear_queue();
    }
    if (video_transport_) {
        video_transport_->stop();
    }
    if (video_consumer_) {
        video_consumer_->clear();
    }
    snapshot_.phase = StudioSessionPhase::Stopped;
    snapshot_.input_state = StudioInputBindingState::Unbound;
    snapshot_.video_state = StudioVideoState::Disconnected;
}

void StudioSessionController::reset_session() {
    std::lock_guard<std::mutex> lock(mutex_);
    handle_session_change(fang::broker::RuntimeStatusData{});
    snapshot_ = StudioSessionSnapshot{};
    was_started_ = false;
}

void StudioSessionController::handle_session_change(const fang::broker::RuntimeStatusData& new_runtime) {
    // Invalidate stale input commands and targets
    if (input_dispatcher_) {
        input_dispatcher_->cancel_pending();
        input_dispatcher_->unbind_session();
    }
    if (input_router_) {
        input_router_->set_guest_ready(false);
        input_router_->set_adb_serial("");
        input_router_->clear_queue();
    }

    // Release existing video transport and purge stale frames
    if (video_transport_) {
        video_transport_->stop();
    }
    if (video_consumer_) {
        video_consumer_->clear();
    }

    current_generation_id_ = new_runtime.generation_id;
    current_process_id_ = new_runtime.process_id;
    current_avd_name_ = new_runtime.avd_name;
    current_serial_ = new_runtime.adb_serial;
}

StudioSessionSnapshot StudioSessionController::update(
    const SupervisorRuntimeSnapshot& supervisor_snapshot,
    ViewportBackend active_backend,
    std::size_t frames_presented,
    const std::vector<std::string>& known_devices) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto& runtime = supervisor_snapshot.runtime;

    // Detect session identity transitions (generation change, process rebirth, or restart)
    const bool generation_changed = !runtime.generation_id.empty() &&
        runtime.generation_id != current_generation_id_;
    const bool process_changed = runtime.process_id != 0 &&
        runtime.process_id != current_process_id_ &&
        !current_generation_id_.empty();
    const bool restarting = (runtime.state == fang::broker::RuntimeLifecycleState::Starting &&
        (current_process_id_ != 0 || !current_generation_id_.empty()) &&
        runtime.process_id == 0);

    if (generation_changed || process_changed || restarting) {
        handle_session_change(runtime);
    } else {
        current_generation_id_ = runtime.generation_id;
        current_process_id_ = runtime.process_id;
        current_avd_name_ = runtime.avd_name;
        current_serial_ = runtime.adb_serial;
    }

    StudioSessionSnapshot snap;
    snap.active_backend = active_backend;
    snap.generation_id = runtime.generation_id;
    snap.avd_name = runtime.avd_name;
    snap.adb_serial = runtime.adb_serial;
    snap.process_id = runtime.process_id;
    snap.guest_ready = runtime.guest_ready;
    snap.diagnostic = runtime.diagnostic.empty() ? supervisor_snapshot.message : runtime.diagnostic;
    snap.video_frames_presented = frames_presented;

    // 1. Device identity verification and input target binding
    bool is_ambiguous = false;
    const std::string& serial = runtime.adb_serial;

    if (!known_devices.empty()) {
        // If multiple devices are present in the environment
        if (known_devices.size() > 1) {
            if (serial.empty()) {
                is_ambiguous = true;
            } else {
                const auto match = std::find(known_devices.begin(), known_devices.end(), serial);
                if (match == known_devices.end()) {
                    is_ambiguous = true;
                }
            }
        }
    }

    // Check for delimiter injection or malformed serial
    if (!serial.empty()) {
        if (serial.find(',') != std::string::npos ||
            serial.find(';') != std::string::npos ||
            serial.find(' ') != std::string::npos ||
            !AdbInputDispatcher::is_valid_adb_serial(serial)) {
            is_ambiguous = true;
        }
    }

    snap.is_ambiguous_target = is_ambiguous;

    const bool is_ready = (runtime.state == fang::broker::RuntimeLifecycleState::Ready && runtime.guest_ready);
    const bool is_starting = (
        runtime.state == fang::broker::RuntimeLifecycleState::Starting ||
        runtime.state == fang::broker::RuntimeLifecycleState::ProcessCreated ||
        runtime.state == fang::broker::RuntimeLifecycleState::OwnershipVerified ||
        runtime.state == fang::broker::RuntimeLifecycleState::GuestBooting);

    const bool is_inactive = (
        runtime.state == fang::broker::RuntimeLifecycleState::Stopped ||
        runtime.state == fang::broker::RuntimeLifecycleState::Stopping ||
        runtime.state == fang::broker::RuntimeLifecycleState::Failed ||
        runtime.state == fang::broker::RuntimeLifecycleState::RecoveryRequired);

    if (is_ambiguous) {
        snap.input_state = StudioInputBindingState::AmbiguousOrMultiple;
        snap.input_diagnostic = "Multiple or ambiguous ADB devices detected; input disabled.";
        if (input_dispatcher_) {
            input_dispatcher_->unbind_session();
        }
        if (input_router_) {
            input_router_->set_target_ambiguous(true);
            input_router_->set_guest_ready(false);
            input_router_->set_adb_serial("");
        }
    } else if (is_inactive) {
        snap.input_state = StudioInputBindingState::Unbound;
        snap.input_diagnostic = runtime.diagnostic.empty() ? "No active runtime session." : runtime.diagnostic;
        if (input_dispatcher_) {
            input_dispatcher_->unbind_session();
        }
        if (input_router_) {
            input_router_->set_target_ambiguous(false);
            input_router_->set_guest_ready(false);
            input_router_->set_adb_serial("");
        }
    } else if (is_starting) {
        snap.input_state = StudioInputBindingState::Verifying;
        snap.input_diagnostic = "Runtime starting; verifying guest identity and input binding.";
        if (input_dispatcher_) {
            input_dispatcher_->unbind_session();
        }
        if (input_router_) {
            input_router_->set_target_ambiguous(false);
            input_router_->set_guest_ready(false);
            input_router_->set_adb_serial(serial);
        }
    } else if (is_ready) {
        if (serial.empty()) {
            snap.input_state = StudioInputBindingState::Unavailable;
            snap.input_diagnostic = "Target ADB serial is not available for ready runtime.";
            if (input_dispatcher_) {
                input_dispatcher_->unbind_session();
            }
            if (input_router_) {
                input_router_->set_target_ambiguous(false);
                input_router_->set_guest_ready(false);
                input_router_->set_adb_serial("");
            }
        } else {
            snap.input_state = StudioInputBindingState::Bound;
            if (input_dispatcher_) {
                input_dispatcher_->bind_session_target(serial);
            }
            if (input_router_) {
                input_router_->set_target_ambiguous(false);
                input_router_->set_adb_serial(serial);
                input_router_->set_guest_ready(true);
            }
        }
    } else if (serial.empty()) {
        snap.input_state = StudioInputBindingState::Unavailable;
        snap.input_diagnostic = "Target ADB serial is not available.";
        if (input_dispatcher_) {
            input_dispatcher_->unbind_session();
        }
        if (input_router_) {
            input_router_->set_target_ambiguous(false);
            input_router_->set_guest_ready(false);
            input_router_->set_adb_serial("");
        }
    } else {
        snap.input_state = StudioInputBindingState::Unbound;
        if (input_dispatcher_) {
            input_dispatcher_->unbind_session();
        }
        if (input_router_) {
            input_router_->set_target_ambiguous(false);
            input_router_->set_guest_ready(false);
            input_router_->set_adb_serial(serial);
        }
    }

    // 2. Video transport and frame acquisition
    if (is_ready && !serial.empty() && !is_ambiguous) {
        const auto endpoint = shared_memory_name_for_serial(serial);
        if (endpoint.has_value() && video_transport_) {
            const auto trans_state = video_transport_->state();
            if (trans_state == VideoTransportState::Disconnected ||
                trans_state == VideoTransportState::Closed) {
                video_transport_->start(*endpoint);
            }
        }

        if (video_consumer_ && video_consumer_->has_frame()) {
            snap.video_state = StudioVideoState::Streaming;
        } else if (video_transport_ && video_transport_->state() == VideoTransportState::Error) {
            snap.video_state = StudioVideoState::TransportError;
            snap.video_diagnostic = video_transport_->last_error();
        } else if (video_transport_ && video_transport_->state() == VideoTransportState::Connecting) {
            snap.video_state = StudioVideoState::Connecting;
        } else {
            snap.video_state = StudioVideoState::AwaitingFirstFrame;
        }
    } else {
        if (video_transport_ &&
            video_transport_->state() != VideoTransportState::Disconnected &&
            video_transport_->state() != VideoTransportState::Closed) {
            video_transport_->stop();
        }
        if (runtime.state == fang::broker::RuntimeLifecycleState::Failed ||
            runtime.state == fang::broker::RuntimeLifecycleState::RecoveryRequired) {
            snap.video_state = StudioVideoState::Unavailable;
        } else {
            snap.video_state = StudioVideoState::Disconnected;
        }
    }

    // 3. Overall Session Phase determination
    switch (runtime.state) {
    case fang::broker::RuntimeLifecycleState::Stopped:
        snap.phase = was_started_ ? StudioSessionPhase::Stopped : StudioSessionPhase::NotStarted;
        break;
    case fang::broker::RuntimeLifecycleState::Starting:
    case fang::broker::RuntimeLifecycleState::ProcessCreated:
    case fang::broker::RuntimeLifecycleState::OwnershipVerified:
    case fang::broker::RuntimeLifecycleState::GuestBooting:
        was_started_ = true;
        snap.phase = StudioSessionPhase::Starting;
        break;
    case fang::broker::RuntimeLifecycleState::Ready:
        was_started_ = true;
        if (runtime.guest_ready) {
            if (snap.video_state == StudioVideoState::Streaming) {
                snap.phase = StudioSessionPhase::ActiveStreaming;
            } else {
                snap.phase = StudioSessionPhase::ReadyAwaitingVideo;
            }
        } else {
            snap.phase = StudioSessionPhase::Starting;
        }
        break;
    case fang::broker::RuntimeLifecycleState::Stopping:
        snap.phase = StudioSessionPhase::Stopping;
        break;
    case fang::broker::RuntimeLifecycleState::Failed:
    case fang::broker::RuntimeLifecycleState::RecoveryRequired:
        snap.phase = StudioSessionPhase::Failed;
        break;
    default:
        snap.phase = StudioSessionPhase::Failed;
        break;
    }

    // 4. Collect metrics from subsystems
    if (input_dispatcher_) {
        snap.input_queue_depth = input_dispatcher_->queue_depth();
        snap.input_dispatched_count = input_dispatcher_->dispatched_count();
        snap.input_failed_count = input_dispatcher_->failed_count();
        snap.input_dropped_count = input_dispatcher_->dropped_count();
        if (!input_dispatcher_->last_error().empty() && snap.input_diagnostic.empty()) {
            snap.input_diagnostic = input_dispatcher_->last_error();
        }
    }
    if (input_router_) {
        snap.input_dropped_count += input_router_->dropped_event_count();
    }
    if (video_consumer_) {
        const auto vm = video_consumer_->metrics();
        snap.video_frames_received = vm.frames_received;
    }

    snapshot_ = snap;
    return snap;
}

StudioSessionSnapshot StudioSessionController::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

} // namespace fang::studio
