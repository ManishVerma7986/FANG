#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "broker_supervisor.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string_view>
#include <utility>

namespace fang::broker {
namespace {

std::uint64_t tick_milliseconds() noexcept {
    return static_cast<std::uint64_t>(GetTickCount64());
}

void set_diagnostic(RuntimeStatusData& status, std::string_view value) {
    constexpr std::size_t diagnostic_limit = 1024;
    status.diagnostic.assign(value.substr(0, diagnostic_limit));
}

std::string serial_for(const ReferenceRuntimeConfig& config) {
    return "emulator-" + std::to_string(config.emulator_port);
}

bool serial_present(std::string_view output, std::string_view serial) {
    std::istringstream lines{std::string(output)};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.size() > serial.size() && line.compare(0, serial.size(), serial) == 0 &&
            (line[serial.size()] == ' ' || line[serial.size()] == '\t')) return true;
    }
    return false;
}

bool serial_ready(std::string_view output, std::string_view serial) {
    std::istringstream lines{std::string(output)};
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.size() <= serial.size() || line.compare(0, serial.size(), serial) != 0 ||
            (line[serial.size()] != ' ' && line[serial.size()] != '\t')) continue;
        const auto state_begin = line.find_first_not_of(" \t", serial.size());
        if (state_begin == std::string::npos) return false;
        const auto state_end = line.find_first_of(" \t", state_begin);
        const auto state = state_end == std::string::npos
            ? line.substr(state_begin)
            : line.substr(state_begin, state_end - state_begin);
        return state == "device";
    }
    return false;
}

std::string command_diagnostic(const ReferenceCommandResult& result) {
    std::string message = result.standard_error;
    if (!result.standard_output.empty()) {
        if (!message.empty()) message += " ";
        message += result.standard_output;
    }
    if (result.timed_out) message += " (ADB command timed out)";
    else if (!result.succeeded) message += " (ADB exit code " + std::to_string(result.exit_code) + ")";
    if (result.native_error != ERROR_SUCCESS) {
        message += " (Win32 error " + std::to_string(result.native_error) + ")";
    }
    if (message.size() > 1024) message.resize(1024);
    return message;
}

}

BrokerReply OwnershipSupervisor::runtime_status_reply(BrokerErrorCode error,
        const RuntimeStatusData& status, std::string message) const {
    BrokerReply reply{error, LifecycleState::Ready, std::move(message)};
    reply.data = encode_runtime_status_data(status);
    return reply;
}

BrokerReply OwnershipSupervisor::start_reference_runtime(const Request& request) {
    const auto request_data = decode_start_reference_runtime_request(request.payload);
    RuntimeStatusData status;
    if (!request_data.has_value()) {
        status.state = RuntimeLifecycleState::Failed;
        status.failure_stage = RuntimeFailureStage::ValidateConfiguration;
        set_diagnostic(status, "The reference runtime request is malformed or unsupported.");
        return runtime_status_reply(BrokerErrorCode::InvalidRuntimeConfiguration,
            status, status.diagnostic);
    }
    status.avd_name = request_data->avd_name;
    if (lifecycle_ != SupervisorLifecycle::Ready) {
        status.state = RuntimeLifecycleState::RecoveryRequired;
        set_diagnostic(status, "The supervisor is not in a verified Ready lifecycle.");
        return runtime_status_reply(BrokerErrorCode::RecoveryRequired,
            status, status.diagnostic);
    }

    std::string error;
    auto resolved = resolve_reference_runtime_config(request_data->avd_name, error);
    if (!resolved.has_value()) {
        status.state = RuntimeLifecycleState::Failed;
        status.failure_stage = error.find("SDK") != std::string::npos
            ? RuntimeFailureStage::ResolveSdk : RuntimeFailureStage::ResolveAvd;
        set_diagnostic(status, error);
        return runtime_status_reply(BrokerErrorCode::InvalidRuntimeConfiguration,
            status, status.diagnostic);
    }
    if (!valid_emulator_port(resolved->emulator_port)) {
        status.state = RuntimeLifecycleState::Failed;
        status.failure_stage = RuntimeFailureStage::ValidateConfiguration;
        set_diagnostic(status, "Configured emulator port " +
            std::to_string(resolved->emulator_port) + " is invalid.");
        return runtime_status_reply(BrokerErrorCode::InvalidRuntimeConfiguration,
            status, status.diagnostic);
    }
    for (const auto& [existing_key, existing_gen] : owned_generations_) {
        if (!existing_gen.empty_verified &&
            existing_gen.runtime_config.emulator_port == resolved->emulator_port) {
            status.state = RuntimeLifecycleState::Failed;
            status.failure_stage = RuntimeFailureStage::CheckAdbPort;
            set_diagnostic(status, "Emulator port " + std::to_string(resolved->emulator_port) +
                " is already allocated to active runtime generation " +
                existing_gen.runtime_status.generation_id + ".");
            return runtime_status_reply(BrokerErrorCode::RuntimeAlreadyExists,
                status, status.diagnostic);
        }
    }
    status.adb_serial = serial_for(*resolved);
    const auto devices = run_reference_adb(*resolved, {L"devices"},
        resolved->adb_command_timeout_ms);
    if (!devices.succeeded) {
        status.state = RuntimeLifecycleState::Failed;
        status.failure_stage = RuntimeFailureStage::CheckAdbPort;
        set_diagnostic(status, command_diagnostic(devices));
        return runtime_status_reply(BrokerErrorCode::RuntimeStartFailed,
            status, status.diagnostic);
    }
    if (serial_present(devices.standard_output, status.adb_serial)) {
        status.state = RuntimeLifecycleState::Failed;
        status.failure_stage = RuntimeFailureStage::CheckAdbPort;
        set_diagnostic(status, "The configured ADB serial is already present; the supervisor will not adopt it.");
        return runtime_status_reply(BrokerErrorCode::RuntimeAlreadyExists,
            status, status.diagnostic);
    }

    CanonicalAvdIdentity identity{user_sid_, resolved->canonical_avd_location};
    if (!identity.valid()) {
        status.state = RuntimeLifecycleState::Failed;
        status.failure_stage = RuntimeFailureStage::ResolveAvd;
        set_diagnostic(status, "The resolved AVD identity is invalid.");
        return runtime_status_reply(BrokerErrorCode::InvalidRuntimeConfiguration,
            status, status.diagnostic);
    }
    const auto existing = owned_generations_.find(identity.key());
    if (existing != owned_generations_.end() && !existing->second.empty_verified) {
        if (!existing->second.runtime_status.generation_id.empty()) {
            std::string status_error;
            refresh_runtime_status(existing->second, status_error);
            status = existing->second.runtime_status;
            if (!status_error.empty()) set_diagnostic(status, status_error);
        } else {
            status.state = RuntimeLifecycleState::RecoveryRequired;
            set_diagnostic(status, "This AVD identity already has an outstanding reservation.");
        }
        return runtime_status_reply(BrokerErrorCode::RuntimeAlreadyExists,
            status, "A runtime generation already exists for this canonical AVD identity.");
    }

    const auto reservation = reserve_identity({RequestType::ReserveIdentity,
        request.request_id, encode_identity_payload(identity)});
    if (reservation.error != BrokerErrorCode::None) {
        status.state = reservation.error == BrokerErrorCode::RecoveryRequired ||
            reservation.error == BrokerErrorCode::StoreNotInitialized
            ? RuntimeLifecycleState::RecoveryRequired : RuntimeLifecycleState::Failed;
        status.failure_stage = RuntimeFailureStage::CreateJob;
        set_diagnostic(status, reservation.message);
        return runtime_status_reply(reservation.error, status, reservation.message);
    }
    auto held = owned_generations_.find(identity.key());
    if (held == owned_generations_.end() || held->second.job == nullptr) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        status.state = RuntimeLifecycleState::RecoveryRequired;
        status.failure_stage = RuntimeFailureStage::CreateJob;
        set_diagnostic(status, "Reservation succeeded without a retained supervisor Job handle.");
        return runtime_status_reply(BrokerErrorCode::RecoveryRequired,
            status, status.diagnostic);
    }

    auto& generation = held->second;
    generation.runtime_config = std::move(*resolved);
    generation.started_at_ms = tick_milliseconds();
    generation.shutdown_requested_at_ms = 0;
    generation.runtime_status = status;
    generation.runtime_status.state = RuntimeLifecycleState::Starting;
    generation.runtime_status.generation_id = generation.metadata.generation_id;
    auto process = ReferenceRuntimeProcess::create_suspended(
        generation.runtime_config, *generation.job, error);
    if (!process) {
        generation.runtime_status.state = RuntimeLifecycleState::Failed;
        generation.runtime_status.failure_stage = RuntimeFailureStage::CreateProcess;
        set_diagnostic(generation.runtime_status, error);
        const auto durable = journal_.load(identity);
        const auto decision = assess_recovery(identity, durable, jobs_, membership_);
        if (decision.replacement_launch_permitted() &&
            finalize_generation(identity, generation.metadata.generation_id, decision, error)) {
            generation.runtime_status.state = RuntimeLifecycleState::Failed;
        } else {
            lifecycle_ = SupervisorLifecycle::RecoveryRequired;
            generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
            if (!error.empty()) set_diagnostic(generation.runtime_status, error);
        }
        return runtime_status_reply(BrokerErrorCode::RuntimeStartFailed,
            generation.runtime_status, generation.runtime_status.diagnostic);
    }

    generation.process = std::move(process);
    generation.runtime_status.process_id = generation.process->process_id();
    generation.runtime_status.process_creation_time = generation.process->creation_time();
    generation.runtime_status.state = RuntimeLifecycleState::ProcessCreated;
    if (!error.empty() || generation.process->creation_time() == 0) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        set_diagnostic(generation.runtime_status, error.empty()
            ? "Contained process creation time is unavailable."
            : error);
        return runtime_status_reply(BrokerErrorCode::RecoveryRequired,
            generation.runtime_status, generation.runtime_status.diagnostic);
    }

    generation.metadata.phase = RuntimeLaunchPhase::ProcessCreationAttempted;
    generation.metadata.root_process_id = generation.process->process_id();
    generation.metadata.root_creation_time = generation.process->creation_time();
    if (!journal_.write_pending(generation.metadata, error)) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        set_diagnostic(generation.runtime_status, error);
        return runtime_status_reply(BrokerErrorCode::RecoveryRequired,
            generation.runtime_status, generation.runtime_status.diagnostic);
    }

    if (!generation.process->resume(error)) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::CreateProcess;
        set_diagnostic(generation.runtime_status, error);
        return runtime_status_reply(BrokerErrorCode::RecoveryRequired,
            generation.runtime_status, generation.runtime_status.diagnostic);
    }
    const auto observation = membership_.query(*generation.job, generation.metadata);
    const auto root = std::find_if(observation.processes.begin(), observation.processes.end(),
        [&](const auto& member) {
            return member.process_id == generation.metadata.root_process_id &&
                member.creation_time == generation.metadata.root_creation_time;
        });
    if (observation.state != JobMembershipResult::State::Verified || !observation.error.empty() ||
        !observation.root_identity_verified || root == observation.processes.end()) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        generation.runtime_status.active_process_count = observation.active_process_count;
        set_diagnostic(generation.runtime_status, observation.error.empty()
            ? "Fresh native evidence did not confirm the expected root in its Job."
            : observation.error);
        return runtime_status_reply(BrokerErrorCode::RecoveryRequired,
            generation.runtime_status, generation.runtime_status.diagnostic);
    }
    generation.runtime_status.state = RuntimeLifecycleState::OwnershipVerified;
    generation.runtime_status.active_process_count = observation.active_process_count;
    generation.runtime_status.elapsed_ms = tick_milliseconds() - generation.started_at_ms;
    set_diagnostic(generation.runtime_status,
        "Emulator process is contained and its PID/creation time were verified; waiting for Android boot.");
    return runtime_status_reply(BrokerErrorCode::None, generation.runtime_status,
        generation.runtime_status.diagnostic);
}

bool OwnershipSupervisor::refresh_runtime_status(OwnedGeneration& generation,
        std::string& error) {
    error.clear();
    if (generation.empty_verified) {
        if (generation.runtime_status.state != RuntimeLifecycleState::Failed) {
            generation.runtime_status.state = RuntimeLifecycleState::Stopped;
        }
        return true;
    }
    if (!generation.process || !generation.job) {
        error = "The supervisor has no retained process and Job handles for this runtime generation.";
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        set_diagnostic(generation.runtime_status, error);
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return false;
    }
    const auto durable = journal_.load(generation.metadata.identity);
    if (durable.state != JournalLoadState::Record || !durable.record.has_value() ||
        durable.record->generation_id != generation.metadata.generation_id) {
        error = "Durable runtime generation is missing or inconsistent.";
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        set_diagnostic(generation.runtime_status, error);
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return false;
    }
    const auto decision = assess_recovery(generation.metadata.identity, durable, jobs_, membership_);
    if (decision.state == RecoveryState::RecoveryRequired) {
        error = decision.detail;
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        set_diagnostic(generation.runtime_status, error);
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return false;
    }
    const auto observation = membership_.query(*generation.job, *durable.record);
    if (observation.state != JobMembershipResult::State::Verified || !observation.error.empty()) {
        error = observation.error.empty() ? "Native Job membership is not verified." : observation.error;
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        set_diagnostic(generation.runtime_status, error);
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return false;
    }
    generation.runtime_status.active_process_count = observation.active_process_count;
    generation.runtime_status.process_id = generation.metadata.root_process_id;
    generation.runtime_status.process_creation_time = generation.metadata.root_creation_time;
    generation.runtime_status.elapsed_ms = tick_milliseconds() - generation.started_at_ms;

    std::string process_error;
    const bool root_running = generation.process->is_running(process_error);
    if (!process_error.empty()) {
        error = process_error;
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        set_diagnostic(generation.runtime_status, error);
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return false;
    }
    if (observation.active_process_count == 0 && !root_running) {
        const auto previous_failure = generation.runtime_status.failure_stage;
        const bool unexpected_exit = generation.shutdown_requested_at_ms == 0;
        std::uint32_t root_exit_code{};
        std::string exit_error;
        const bool exit_code_available = generation.process->exit_code(root_exit_code, exit_error);
        if (!decision.replacement_launch_permitted() ||
            !finalize_generation(generation.metadata.identity,
                generation.metadata.generation_id, decision, error)) {
            generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
            generation.runtime_status.failure_stage = RuntimeFailureStage::Finalize;
            set_diagnostic(generation.runtime_status, error.empty()
                ? "Fresh native evidence did not permit finalization."
                : error);
            lifecycle_ = SupervisorLifecycle::RecoveryRequired;
            return false;
        }
        if (previous_failure == RuntimeFailureStage::GuestBoot || unexpected_exit ||
            !exit_code_available || root_exit_code != 0) {
            generation.runtime_status.state = RuntimeLifecycleState::Failed;
            generation.runtime_status.failure_stage = previous_failure == RuntimeFailureStage::None
                ? RuntimeFailureStage::CreateProcess : previous_failure;
            set_diagnostic(generation.runtime_status,
                !exit_code_available ? exit_error : "The Emulator exited before the requested runtime state was reached.");
        }
        return true;
    }
    if (generation.runtime_status.state == RuntimeLifecycleState::Stopping) {
        if (generation.shutdown_requested_at_ms != 0 &&
            tick_milliseconds() - generation.shutdown_requested_at_ms >=
                generation.runtime_config.shutdown_timeout_ms) {
            error = "Graceful Emulator shutdown timed out; owned process membership remains active.";
            generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
            generation.runtime_status.failure_stage = RuntimeFailureStage::Shutdown;
            set_diagnostic(generation.runtime_status, error);
            lifecycle_ = SupervisorLifecycle::RecoveryRequired;
            return false;
        }
        return true;
    }

    if (!root_running && observation.active_process_count > 0) {
        error = "The Emulator root exited while Job members remain active.";
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        set_diagnostic(generation.runtime_status, error);
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return false;
    }
    if (observation.active_process_count == 0) {
        error = "The Emulator root is running while its Job reports no active members.";
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        generation.runtime_status.failure_stage = RuntimeFailureStage::VerifyOwnership;
        set_diagnostic(generation.runtime_status, error);
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return false;
    }

    const auto elapsed = tick_milliseconds() - generation.started_at_ms;
    if (elapsed >= generation.runtime_config.readiness_timeout_ms &&
        !generation.runtime_status.guest_ready) {
        generation.runtime_status.failure_stage = RuntimeFailureStage::GuestBoot;
        set_diagnostic(generation.runtime_status,
            "Android did not report sys.boot_completed=1 before the configured readiness timeout.");
        if (request_graceful_shutdown(generation, error)) return true;
        generation.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        if (!error.empty()) set_diagnostic(generation.runtime_status, error);
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return false;
    }

    const auto devices = run_reference_adb(generation.runtime_config, {L"devices"},
        generation.runtime_config.adb_command_timeout_ms);
    if (!devices.succeeded) {
        generation.runtime_status.state = RuntimeLifecycleState::GuestBooting;
        generation.runtime_status.failure_stage = RuntimeFailureStage::AdbTransport;
        set_diagnostic(generation.runtime_status, command_diagnostic(devices));
        return true;
    }
    if (!serial_ready(devices.standard_output, generation.runtime_status.adb_serial)) {
        generation.runtime_status.state = RuntimeLifecycleState::GuestBooting;
        generation.runtime_status.failure_stage = RuntimeFailureStage::AdbTransport;
        std::string raw_devices = devices.standard_output;
        while (!raw_devices.empty() && (raw_devices.back() == '\r' || raw_devices.back() == '\n' ||
                raw_devices.back() == ' ' || raw_devices.back() == '\t')) raw_devices.pop_back();
        while (!raw_devices.empty() && (raw_devices.front() == '\r' || raw_devices.front() == '\n')) raw_devices.erase(raw_devices.begin());
        set_diagnostic(generation.runtime_status,
            "Waiting for " + generation.runtime_status.adb_serial + " to reach device state (" +
            (raw_devices.empty() ? "empty adb devices" : raw_devices) + ")");
        return true;
    }
    const auto boot = run_reference_adb(generation.runtime_config,
        {L"-s", std::wstring(generation.runtime_status.adb_serial.begin(),
            generation.runtime_status.adb_serial.end()), L"shell", L"getprop",
            L"sys.boot_completed"}, generation.runtime_config.adb_command_timeout_ms);
    if (!boot.succeeded) {
        generation.runtime_status.state = RuntimeLifecycleState::GuestBooting;
        generation.runtime_status.failure_stage = RuntimeFailureStage::GuestBoot;
        set_diagnostic(generation.runtime_status, command_diagnostic(boot));
        return true;
    }
    std::string boot_value = boot.standard_output;
    while (!boot_value.empty() && (boot_value.back() == '\r' || boot_value.back() == '\n' ||
            boot_value.back() == ' ' || boot_value.back() == '\t')) boot_value.pop_back();
    while (!boot_value.empty() && (boot_value.front() == ' ' || boot_value.front() == '\t')) {
        boot_value.erase(boot_value.begin());
    }
    if (boot_value == "1") {
        generation.runtime_status.state = RuntimeLifecycleState::Ready;
        generation.runtime_status.guest_ready = true;
        generation.runtime_status.failure_stage = RuntimeFailureStage::None;
        set_diagnostic(generation.runtime_status,
            "The exact ADB instance reports sys.boot_completed=1 and native Job ownership remains verified.");
    } else {
        generation.runtime_status.state = RuntimeLifecycleState::GuestBooting;
        generation.runtime_status.failure_stage = RuntimeFailureStage::GuestBoot;
        generation.runtime_status.guest_ready = false;
        set_diagnostic(generation.runtime_status,
            "Waiting for the exact ADB instance to report sys.boot_completed=1.");
    }
    return true;
}

bool OwnershipSupervisor::request_graceful_shutdown(OwnedGeneration& generation,
        std::string& error) {
    error.clear();
    if (!generation.process || !generation.job) {
        error = "The supervisor lacks retained process or Job handles for graceful shutdown.";
        return false;
    }
    if (!generation.process->resumed()) {
        if (!generation.process->resume(error)) return false;
    }
    const auto durable = journal_.load(generation.metadata.identity);
    if (durable.state != JournalLoadState::Record || !durable.record.has_value() ||
        durable.record->generation_id != generation.metadata.generation_id) {
        error = "Durable runtime identity is missing or stale; shutdown remains recovery-required.";
        return false;
    }
    const auto decision = assess_recovery(generation.metadata.identity, durable, jobs_, membership_);
    if (decision.state == RecoveryState::RecoveryRequired) {
        error = decision.detail;
        return false;
    }
    const auto observation = membership_.query(*generation.job, *durable.record);
    if (observation.state != JobMembershipResult::State::Verified || !observation.error.empty()) {
        error = observation.error.empty() ? "Native membership query failed before shutdown." : observation.error;
        return false;
    }
    if (observation.active_process_count == 0) {
        if (!decision.replacement_launch_permitted() ||
            !finalize_generation(generation.metadata.identity,
                generation.metadata.generation_id, decision, error)) {
            if (error.empty()) error = "The empty Job did not have a finalizable recovery decision.";
            return false;
        }
        return true;
    }
    const auto adb_shutdown = run_reference_adb(generation.runtime_config,
        {L"-s", std::wstring(generation.runtime_status.adb_serial.begin(),
            generation.runtime_status.adb_serial.end()), L"emu", L"kill"},
        generation.runtime_config.adb_command_timeout_ms);
    if (!adb_shutdown.succeeded && !post_graceful_close_to_job_window(
            *generation.job, observation.processes, error)) {
        const auto adb_error = command_diagnostic(adb_shutdown);
        if (!error.empty()) error = adb_error + "; " + error;
        else error = adb_error;
        return false;
    }
    generation.shutdown_requested_at_ms = tick_milliseconds();
    generation.runtime_status.state = RuntimeLifecycleState::Stopping;
    generation.runtime_status.guest_ready = false;
    set_diagnostic(generation.runtime_status, adb_shutdown.succeeded
        ? "Graceful ADB Emulator shutdown was requested; waiting for verified Job emptiness."
        : "Graceful WM_CLOSE was posted to a freshly verified Job window; waiting for verified Job emptiness.");
    return true;
}

BrokerReply OwnershipSupervisor::query_runtime_status(const Request& request) {
    const auto generation_request = decode_runtime_generation_request(request.payload);
    RuntimeStatusData status;
    if (!generation_request.has_value()) {
        status.state = RuntimeLifecycleState::Failed;
        status.failure_stage = RuntimeFailureStage::ValidateConfiguration;
        set_diagnostic(status, "Runtime status request must contain a valid generation identifier.");
        return runtime_status_reply(BrokerErrorCode::InvalidMessage, status, status.diagnostic);
    }
    status.generation_id = generation_request->generation_id;
    auto held = std::find_if(owned_generations_.begin(), owned_generations_.end(),
        [&](const auto& entry) { return entry.second.metadata.generation_id == status.generation_id; });
    if (held == owned_generations_.end()) {
        const auto store = journal_.load_store();
        const bool durable_generation_exists = std::any_of(store.entries.begin(), store.entries.end(),
            [&](const auto& entry) { return entry.runtime_generation_id == status.generation_id; });
        status.state = durable_generation_exists ? RuntimeLifecycleState::RecoveryRequired
            : RuntimeLifecycleState::Failed;
        set_diagnostic(status, durable_generation_exists
            ? "The durable generation exists but this supervisor has no retained runtime handles."
            : "No runtime generation matches the requested identifier.");
        return runtime_status_reply(durable_generation_exists ? BrokerErrorCode::RecoveryRequired
                : BrokerErrorCode::RuntimeNotFound,
            status, status.diagnostic);
    }
    std::string error;
    refresh_runtime_status(held->second, error);
    status = held->second.runtime_status;
    if (!error.empty()) set_diagnostic(status, error);
    const auto result = status.state == RuntimeLifecycleState::RecoveryRequired
        ? BrokerErrorCode::RecoveryRequired : BrokerErrorCode::None;
    return runtime_status_reply(result, status, status.diagnostic);
}

BrokerReply OwnershipSupervisor::shutdown_runtime(const Request& request) {
    const auto generation_request = decode_runtime_generation_request(request.payload);
    RuntimeStatusData status;
    if (!generation_request.has_value()) {
        status.state = RuntimeLifecycleState::Failed;
        status.failure_stage = RuntimeFailureStage::ValidateConfiguration;
        set_diagnostic(status, "Shutdown request must contain a valid generation identifier.");
        return runtime_status_reply(BrokerErrorCode::InvalidMessage, status, status.diagnostic);
    }
    status.generation_id = generation_request->generation_id;
    auto held = std::find_if(owned_generations_.begin(), owned_generations_.end(),
        [&](const auto& entry) { return entry.second.metadata.generation_id == status.generation_id; });
    if (held == owned_generations_.end()) {
        const auto store = journal_.load_store();
        const bool durable_generation_exists = std::any_of(store.entries.begin(), store.entries.end(),
            [&](const auto& entry) { return entry.runtime_generation_id == status.generation_id; });
        status.state = RuntimeLifecycleState::RecoveryRequired;
        status.failure_stage = RuntimeFailureStage::Shutdown;
        set_diagnostic(status, durable_generation_exists
            ? "The durable generation has no in-memory Job handle; manual recovery is required."
            : "No runtime generation matches the requested shutdown.");
        return runtime_status_reply(durable_generation_exists ? BrokerErrorCode::RecoveryRequired
                : BrokerErrorCode::RuntimeNotFound,
            status, status.diagnostic);
    }
    std::string error;
    refresh_runtime_status(held->second, error);
    if (held->second.empty_verified) {
        held->second.runtime_status.state = RuntimeLifecycleState::Stopped;
        return runtime_status_reply(BrokerErrorCode::None, held->second.runtime_status,
            "Runtime is verified stopped.");
    }
    if (held->second.runtime_status.state == RuntimeLifecycleState::Stopped) {
        return runtime_status_reply(BrokerErrorCode::None, held->second.runtime_status,
            "Runtime is already verified stopped.");
    }
    if (held->second.runtime_status.state == RuntimeLifecycleState::Stopping && error.empty()) {
        return runtime_status_reply(BrokerErrorCode::None, held->second.runtime_status,
            held->second.runtime_status.diagnostic);
    }
    if (!error.empty() && held->second.runtime_status.state == RuntimeLifecycleState::RecoveryRequired &&
        held->second.runtime_status.failure_stage == RuntimeFailureStage::VerifyOwnership) {
        status = held->second.runtime_status;
        return runtime_status_reply(BrokerErrorCode::RecoveryRequired, status, error);
    }
    if (!request_graceful_shutdown(held->second, error)) {
        held->second.runtime_status.state = RuntimeLifecycleState::RecoveryRequired;
        held->second.runtime_status.failure_stage = RuntimeFailureStage::Shutdown;
        set_diagnostic(held->second.runtime_status, error);
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return runtime_status_reply(BrokerErrorCode::RuntimeShutdownFailed,
            held->second.runtime_status, error);
    }
    return runtime_status_reply(BrokerErrorCode::None, held->second.runtime_status,
        held->second.runtime_status.diagnostic);
}

}
