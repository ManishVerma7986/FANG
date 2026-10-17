#include "broker_supervisor.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <utility>

namespace fang::broker {
namespace {

constexpr std::size_t maximum_identity_field = 2048;

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    }
}

bool take_u32(std::span<const std::byte> input, std::size_t& offset,
    std::uint32_t& value) {
    if (input.size() - offset < 4) return false;
    value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index])) <<
            (index * 8);
    }
    offset += 4;
    return true;
}

void append_text(std::vector<std::byte>& output, std::string_view value) {
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char character : value) output.push_back(static_cast<std::byte>(character));
}

bool take_text(std::span<const std::byte> input, std::size_t& offset, std::string& value) {
    std::uint32_t length{};
    if (!take_u32(input, offset, length) || length > maximum_identity_field ||
        input.size() - offset < length) return false;
    value.clear();
    value.reserve(length);
    for (std::uint32_t index = 0; index < length; ++index) {
        value.push_back(static_cast<char>(std::to_integer<unsigned char>(input[offset + index])));
    }
    offset += length;
    return value.find('\0') == std::string::npos;
}

bool valid_store(const OwnershipStoreLoadResult& store) {
    return store.state == OwnershipStoreState::Initialized;
}

bool all_store_entries_empty_verified(const OwnershipStoreLoadResult& store) {
    return valid_store(store) && std::all_of(store.entries.begin(), store.entries.end(),
        [](const auto& entry) { return entry.state == OwnershipIndexEntryState::EmptyVerified; });
}

std::string identity_state_text(RecoveryState state) {
    switch (state) {
    case RecoveryState::EmptyVerified: return "EmptyVerified";
    case RecoveryState::RuntimeMayBeActive: return "RuntimeMayBeActive";
    case RecoveryState::RecoveryRequired: return "RecoveryRequired";
    }
    return "RecoveryRequired";
}

std::uint64_t monotonic_milliseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void set_runtime_diagnostic(RuntimeStatusData& status, std::string_view diagnostic) {
    constexpr std::size_t maximum_diagnostic = 1024;
    status.diagnostic.assign(diagnostic.substr(0, maximum_diagnostic));
}

bool exact_adb_device_is_ready(std::string_view output, std::string_view serial) {
    std::istringstream lines{std::string(output)};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.size() <= serial.size() || line.compare(0, serial.size(), serial) != 0 ||
            (line[serial.size()] != ' ' && line[serial.size()] != '\t')) continue;
        const auto state_begin = line.find_first_not_of(" \t", serial.size());
        if (state_begin == std::string::npos) return false;
        const auto state_end = line.find_first_of(" \t", state_begin);
        return line.substr(state_begin, state_end - state_begin) == "device";
    }
    return false;
}

bool adb_serial_present(std::string_view output, std::string_view serial) {
    std::istringstream lines{std::string(output)};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.size() > serial.size() && line.compare(0, serial.size(), serial) == 0 &&
            (line[serial.size()] == ' ' || line[serial.size()] == '\t')) return true;
    }
    return false;
}

}

std::vector<std::byte> encode_identity_payload(const CanonicalAvdIdentity& identity) {
    if (!identity.valid() || identity.user_sid.size() > maximum_identity_field ||
        identity.canonical_avd_location.size() > maximum_identity_field) return {};
    std::vector<std::byte> payload;
    append_text(payload, identity.user_sid);
    append_text(payload, identity.canonical_avd_location);
    return payload;
}

bool decode_identity_payload(std::span<const std::byte> payload,
        CanonicalAvdIdentity& identity, std::string& error) {
    std::size_t offset{};
    if (!take_text(payload, offset, identity.user_sid) ||
        !take_text(payload, offset, identity.canonical_avd_location) ||
        offset != payload.size() || !identity.valid()) {
        error = "The supervisor identity payload is malformed or out of bounds.";
        return false;
    }
    return true;
}

std::vector<std::byte> encode_generation_payload(const CanonicalAvdIdentity& identity,
        std::string_view generation_id) {
    auto payload = encode_identity_payload(identity);
    if (payload.empty() || generation_id.size() > maximum_identity_field) return {};
    append_text(payload, generation_id);
    return payload;
}

bool decode_generation_payload(std::span<const std::byte> payload,
        CanonicalAvdIdentity& identity, std::string& generation_id, std::string& error) {
    std::size_t offset{};
    if (!take_text(payload, offset, identity.user_sid) ||
        !take_text(payload, offset, identity.canonical_avd_location) ||
        !take_text(payload, offset, generation_id) || offset != payload.size() ||
        !identity.valid() || generation_id.size() != 36) {
        error = "The supervisor generation payload is malformed or out of bounds.";
        return false;
    }
    return true;
}

OwnershipSupervisor::OwnershipSupervisor(std::string user_sid, std::uint32_t session_id,
    std::uint32_t process_id, std::string owner_generation_id,
    std::wstring storage_root)
    : user_sid_(std::move(user_sid)), session_id_(session_id), process_id_(process_id),
      owner_generation_id_(std::move(owner_generation_id)), request_ids_(4096),
            journal_(user_sid_, session_id_, process_id_, owner_generation_id_,
                std::move(storage_root)),
            jobs_(user_sid_), native_membership_(user_sid_), membership_(native_membership_) {
    const auto store = journal_.load_store();
    if (store.state == OwnershipStoreState::Missing) {
        lifecycle_ = SupervisorLifecycle::StoreUninitialized;
    } else if (all_store_entries_empty_verified(store)) {
        lifecycle_ = SupervisorLifecycle::Ready;
    } else {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
    }
}

SupervisorLifecycle OwnershipSupervisor::lifecycle() const {
    std::lock_guard lock(mutex_);
    return lifecycle_;
}

BrokerReply OwnershipSupervisor::fail_closed(BrokerErrorCode code, std::string message) const {
    return {code, LifecycleState::Ready, std::move(message)};
}

BrokerReply OwnershipSupervisor::status_reply() const {
    return {BrokerErrorCode::None, LifecycleState::Ready,
        "Supervisor lifecycle=" + std::to_string(static_cast<unsigned int>(lifecycle_)) +
        "; transport state is independent of runtime ownership."};
}

BrokerReply OwnershipSupervisor::dispatch(const Request& request) {
    std::lock_guard lock(mutex_);
    if (!request_ids_.observe(request.request_id)) {
        return {BrokerErrorCode::DuplicateRequestId, LifecycleState::Ready,
            "The supervisor request ID was already used recently."};
    }
    switch (request.type) {
    case RequestType::GetStatus:
        if (!request.payload.empty()) {
            return fail_closed(BrokerErrorCode::InvalidMessage,
                "Supervisor status does not accept a payload.");
        }
        return status_reply();
    case RequestType::InitializeOwnerStore:
        return initialize_store(request);
    case RequestType::QueryOwnership:
        return query_identity(request);
    case RequestType::ReserveIdentity:
        return reserve_identity(request);
    case RequestType::FinalizeEmpty:
        return finalize_empty(request);
    case RequestType::StartReferenceRuntime:
        return start_reference_runtime(request);
    case RequestType::QueryRuntimeStatus:
        return query_runtime_status(request);
    case RequestType::ShutdownRuntime:
        return shutdown_runtime(request);
    case RequestType::ReleaseReservation:
        return fail_closed(BrokerErrorCode::InvalidState,
            "Reservations cannot be released without native empty-state verification.");
    case RequestType::ShutdownOwner:
        return fail_closed(BrokerErrorCode::InvalidState,
            "The owner cannot shut down through client RPC in this milestone.");
    case RequestType::Ping:
        if (!request.payload.empty()) {
            return fail_closed(BrokerErrorCode::InvalidMessage, "Ping does not accept a payload.");
        }
        return {BrokerErrorCode::None, LifecycleState::Ready, "supervisor-pong"};
    }
    return fail_closed(BrokerErrorCode::UnknownMessageType,
        "The supervisor operation is unknown.");
}

BrokerReply OwnershipSupervisor::initialize_store(const Request& request) {
    if (!request.payload.empty()) {
        return fail_closed(BrokerErrorCode::InvalidMessage,
            "Explicit store initialization accepts no payload.");
    }
    std::string error;
    if (!journal_.initialize_store_explicit(error)) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            "Authority store initialization failed closed: " + error);
    }
    const auto store = journal_.load_store();
    const bool ready = all_store_entries_empty_verified(store);
    lifecycle_ = ready ? SupervisorLifecycle::Ready : SupervisorLifecycle::RecoveryRequired;
    return {ready ? BrokerErrorCode::None : BrokerErrorCode::RecoveryRequired, LifecycleState::Ready,
        ready
            ? "Authority store explicitly initialized. This operation does not create a Job or authorize a runtime launch."
            : "Authority store exists but contains prior identity records requiring recovery."};
}

BrokerReply OwnershipSupervisor::query_identity(const Request& request) {
    CanonicalAvdIdentity identity;
    std::string error;
    if (!decode_identity_payload(request.payload, identity, error)) {
        return fail_closed(BrokerErrorCode::InvalidMessage, std::move(error));
    }
    if (identity.user_sid != user_sid_) {
        return fail_closed(BrokerErrorCode::UnauthorizedClient,
            "The requested ownership identity belongs to another user SID.");
    }
    const auto store = journal_.load_store();
    if (!valid_store(store)) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(store.state == OwnershipStoreState::AccessDenied
                ? BrokerErrorCode::UnauthorizedClient : BrokerErrorCode::StoreNotInitialized,
            store.error.empty() ? "Authority store is missing or ambiguous." : store.error);
    }
    const auto indexed = std::find_if(store.entries.begin(), store.entries.end(),
        [&](const auto& entry) { return entry.identity.key() == identity.key(); });
    if (indexed == store.entries.end()) {
        const auto record = journal_.load(identity);
        if (record.state != JournalLoadState::Missing) {
            lifecycle_ = SupervisorLifecycle::RecoveryRequired;
            return fail_closed(BrokerErrorCode::RecoveryRequired,
                "An unindexed ownership journal exists; it cannot be treated as empty.");
        }
        return {BrokerErrorCode::None, LifecycleState::Ready,
            "EmptyVerified; explicit store initialized, identity absent from durable index, and live owner has no outstanding generation."};
    }
    const auto record = journal_.load(identity);
    if (record.state != JournalLoadState::Record || !record.record.has_value() ||
        record.record->generation_id != indexed->runtime_generation_id) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            "Indexed runtime and journal generation are missing, stale, or inconsistent.");
    }
    const auto decision = assess_recovery(identity, record, jobs_, membership_);
    if (decision.state == RecoveryState::RecoveryRequired) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            decision.detail);
    }
    return {BrokerErrorCode::None, LifecycleState::Ready,
        identity_state_text(decision.state) + "; " + decision.detail};
}

BrokerReply OwnershipSupervisor::reserve_identity(const Request& request) {
    CanonicalAvdIdentity identity;
    std::string error;
    if (!decode_identity_payload(request.payload, identity, error)) {
        return fail_closed(BrokerErrorCode::InvalidMessage, std::move(error));
    }
    if (identity.user_sid != user_sid_) {
        return fail_closed(BrokerErrorCode::UnauthorizedClient,
            "The requested ownership identity belongs to another user SID.");
    }
    const auto store = journal_.load_store();
    if (!valid_store(store)) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::StoreNotInitialized,
            "The authority store must be explicitly initialized before reservation.");
    }
    const auto indexed = std::find_if(store.entries.begin(), store.entries.end(),
        [&](const auto& entry) { return entry.identity.key() == identity.key(); });
    std::optional<RecoveryDecision> previous_empty;
    std::string previous_generation;
    if (indexed != store.entries.end()) {
        if (indexed->state != OwnershipIndexEntryState::EmptyVerified) {
            return fail_closed(BrokerErrorCode::Busy,
                "This canonical identity already has a pending or owned generation.");
        }
        previous_generation = indexed->runtime_generation_id;
        const auto previous_record = journal_.load(identity);
        if (previous_record.state != JournalLoadState::Record || !previous_record.record.has_value() ||
            previous_record.record->generation_id != previous_generation) {
            lifecycle_ = SupervisorLifecycle::RecoveryRequired;
            return fail_closed(BrokerErrorCode::RecoveryRequired,
                "The empty index entry has no matching durable journal record.");
        }
        previous_empty = assess_recovery(identity, previous_record, jobs_, membership_);
        if (!previous_empty->replacement_launch_permitted()) {
            lifecycle_ = SupervisorLifecycle::RecoveryRequired;
            return fail_closed(BrokerErrorCode::RecoveryRequired,
                "A fresh native query did not verify the previous generation empty: " +
                    previous_empty->detail);
        }
    }
    auto held = owned_generations_.find(identity.key());
    if (held != owned_generations_.end()) {
        if (held->second.job != nullptr && !held->second.empty_verified) {
            return fail_closed(BrokerErrorCode::Busy,
                "This supervisor still holds a Job handle for the identity.");
        }
        // A finalized (EmptyVerified) in-memory entry with no live Job handle
        // is not an outstanding generation: drop it so the next reservation can
        // start a fresh generation. The durable EmptyVerified tombstone plus
        // index entry remain and are revalidated below as the replacement gate.
        owned_generations_.erase(held);
    }

    std::string generation = create_runtime_generation_id(error);
    if (generation.empty()) {
        return fail_closed(BrokerErrorCode::InternalFailure, std::move(error));
    }
    // Crash-safe order: claim the durable store identity BEFORE creating the
    // kernel Job. A crash between claim and Job creation leaves a Pending
    // index entry that fails closed (Busy) until manual recovery; the reverse
    // order would leak a live kernel Job with no durable owner record.
    if (!journal_.claim_identity(identity, generation, error)) {
        return fail_closed(BrokerErrorCode::RecoveryRequired, std::move(error));
    }
    std::wstring object_name;
    if (!expected_job_object_name(identity, generation, object_name, error)) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired, std::move(error));
    }
    RuntimeSessionMetadata metadata{identity, generation, object_name,
        RuntimeLaunchPhase::Prepared, 0, 0, owner_generation_id_, session_id_, process_id_};
    const bool journal_written = previous_empty.has_value()
        ? journal_.write_next_generation(metadata, previous_generation, *previous_empty, error)
        : journal_.write_pending(metadata, error);
    if (!journal_written) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            "Could not persist pending state before Job creation: " + error);
    }
    auto created = jobs_.create_non_killing(identity, generation);
    if (created.state != JobCreateState::Created || created.job == nullptr ||
        created.object_name != object_name) {
        // The durable Pending claim plus the pending journal record remain, so
        // a retry or crash-recovery pass fails closed (Busy) instead of
        // silently dropping the reservation. The supervisor lifecycle enters
        // RecoveryRequired and the operator must resolve the orphaned Pending
        // state manually before another launch.
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            "Native Job creation did not produce the expected new object: " + created.error);
    }
    metadata.phase = RuntimeLaunchPhase::JobCreated;
    if (!journal_.write_pending(metadata, error) ||
        !journal_.mark_identity_owned(identity, generation, error)) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            "The Job exists but durable ownership registration failed: " + error);
    }
    owned_generations_.insert_or_assign(identity.key(),
        OwnedGeneration{metadata, std::move(created.job), {}, {}, {}, 0, 0, false});
    return {BrokerErrorCode::None, LifecycleState::Ready,
        "Reserved generation=" + generation + "; Job is owned by this supervisor; no process was launched."};
}

BrokerReply OwnershipSupervisor::finalize_empty(const Request& request) {
    CanonicalAvdIdentity identity;
    std::string generation;
    std::string error;
    if (!decode_generation_payload(request.payload, identity, generation, error)) {
        return fail_closed(BrokerErrorCode::InvalidMessage, std::move(error));
    }
    if (identity.user_sid != user_sid_) {
        return fail_closed(BrokerErrorCode::UnauthorizedClient,
            "The requested ownership identity belongs to another user SID.");
    }
    const auto record = journal_.load(identity);
    if (record.state != JournalLoadState::Record || !record.record.has_value() ||
        record.record->generation_id != generation) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            "The requested generation has no matching durable record.");
    }
    const auto decision = assess_recovery(identity, record, jobs_, membership_);
    if (!decision.replacement_launch_permitted()) {
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            "Native evidence did not establish verified-empty ownership: " + decision.detail);
    }
    // Persist the per-identity tombstone first (journal record), then flip the
    // authority-store index entry. mark_empty_verified() alone is refused by
    // the journal; write the tombstone through the supervisor-only helper so a
    // direct tombstone write can never manufacture trusted empty state.
    if (!finalize_generation(identity, generation, decision, error)) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            "Could not finalize verified-empty ownership: " + error);
    }
    return {BrokerErrorCode::None, LifecycleState::Ready,
        "EmptyVerified; fresh native Job/root validation and durable finalization completed."};
}

bool OwnershipSupervisor::finalize_generation(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, const RecoveryDecision& decision,
        std::string& error) {
    error.clear();
    if (!decision.replacement_launch_permitted()) {
        error = "Only native-backed EmptyVerified evidence may finalize a runtime generation.";
        return false;
    }
    if (!journal_.write_verified_empty_tombstone(identity, generation_id, error) ||
        !journal_.mark_identity_empty(identity, generation_id, decision, error)) {
        return false;
    }
    const auto held = owned_generations_.find(identity.key());
    if (held != owned_generations_.end() &&
        held->second.metadata.generation_id == generation_id) {
        held->second.metadata.phase = RuntimeLaunchPhase::StopRequested;
        held->second.empty_verified = true;
        held->second.process.reset();
        held->second.runtime_status.state = RuntimeLifecycleState::Stopped;
        held->second.runtime_status.guest_ready = false;
        held->second.runtime_status.active_process_count = 0;
    }
    return true;
}

bool OwnershipSupervisor::shutdown_if_empty(std::string& error) {
    std::lock_guard lock(mutex_);
    error.clear();
    if (lifecycle_ != SupervisorLifecycle::Ready) {
        error = "Supervisor cannot shut down without a verified Ready lifecycle.";
        return false;
    }
    if (owned_generations_.empty()) {
        const auto store = journal_.load_store();
        if (!all_store_entries_empty_verified(store)) {
            error = "Supervisor cannot shut down without a freshly verified empty owner store.";
            return false;
        }
    }
    for (const auto& [key, generation] : owned_generations_) {
        static_cast<void>(key);
        // Fail-closed: entries whose in-memory phase predates process-creation
        // or whose Job handle was released out-of-band cannot be trusted from
        // memory alone. Reload durable state so shutdown requires the same
        // fresh native verification as finalize_empty().
        const auto durable = journal_.load(generation.metadata.identity);
        if (durable.state != JournalLoadState::Record || !durable.record.has_value() ||
            durable.record->generation_id != generation.metadata.generation_id) {
            error = "Supervisor cannot shut down: durable ownership for an owned "
                "generation is missing or ambiguous; manual recovery is required.";
            return false;
        }
        const auto decision = assess_recovery(generation.metadata.identity,
            durable, jobs_, membership_);
        if (!decision.replacement_launch_permitted()) {
            error = "Supervisor cannot shut down while an owned generation is active or uncertain: " +
                decision.detail;
            return false;
        }
    }
    lifecycle_ = SupervisorLifecycle::ShuttingDown;
    return true;
}

}
