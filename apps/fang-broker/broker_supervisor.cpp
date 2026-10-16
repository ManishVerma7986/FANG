#include "broker_supervisor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

std::string identity_state_text(RecoveryState state) {
    switch (state) {
    case RecoveryState::EmptyVerified: return "EmptyVerified";
    case RecoveryState::RuntimeMayBeActive: return "RuntimeMayBeActive";
    case RecoveryState::RecoveryRequired: return "RecoveryRequired";
    }
    return "RecoveryRequired";
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
      jobs_(user_sid_), membership_(user_sid_) {
    const auto store = journal_.load_store();
    if (store.state == OwnershipStoreState::Missing) {
        lifecycle_ = SupervisorLifecycle::StoreUninitialized;
    } else if (valid_store(store) && store.entries.empty()) {
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
    lifecycle_ = valid_store(store) && store.entries.empty()
        ? SupervisorLifecycle::Ready : SupervisorLifecycle::RecoveryRequired;
    return {BrokerErrorCode::None, LifecycleState::Ready,
        lifecycle_ == SupervisorLifecycle::Ready
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
    if (held != owned_generations_.end() && held->second.job != nullptr &&
        !held->second.empty_verified) {
        return fail_closed(BrokerErrorCode::Busy,
            "This supervisor still holds a Job handle for the identity.");
    }

    std::string generation = create_runtime_generation_id(error);
    if (generation.empty()) {
        return fail_closed(BrokerErrorCode::InternalFailure, std::move(error));
    }
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
        OwnedGeneration{metadata, std::move(created.job), false});
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
    if (!journal_.mark_empty_verified(identity, generation, error) ||
        !journal_.mark_identity_empty(identity, generation, decision, error)) {
        lifecycle_ = SupervisorLifecycle::RecoveryRequired;
        return fail_closed(BrokerErrorCode::RecoveryRequired,
            "Could not finalize verified-empty ownership: " + error);
    }
    auto held = owned_generations_.find(identity.key());
    if (held != owned_generations_.end() &&
        held->second.metadata.generation_id == generation) {
        held->second.metadata.phase = RuntimeLaunchPhase::StopRequested;
        held->second.empty_verified = true;
    }
    return {BrokerErrorCode::None, LifecycleState::Ready,
        "EmptyVerified; fresh native Job/root validation and durable finalization completed."};
}

bool OwnershipSupervisor::shutdown_if_empty(std::string& error) {
    std::lock_guard lock(mutex_);
    for (const auto& [key, generation] : owned_generations_) {
        static_cast<void>(key);
        const JournalLoadResult record{JournalLoadState::Record, generation.metadata, {}};
        const auto decision = assess_recovery(generation.metadata.identity,
            record, jobs_, membership_);
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
