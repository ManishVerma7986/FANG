#include "broker_ownership.hpp"

#include <utility>

namespace fang::broker {

struct LaunchReservation::SharedState {
    std::mutex mutex;
    std::unordered_set<std::string> identities;
};

namespace {

bool contains_nul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

RecoveryDecision recovery_required(RecoveryReason reason, std::string detail) {
    return {RecoveryState::RecoveryRequired, reason, std::move(detail)};
}

bool valid_phase(RuntimeLaunchPhase phase) noexcept {
    return phase == RuntimeLaunchPhase::Prepared ||
        phase == RuntimeLaunchPhase::JobCreated ||
        phase == RuntimeLaunchPhase::ProcessCreationAttempted ||
        phase == RuntimeLaunchPhase::Active ||
        phase == RuntimeLaunchPhase::StopRequested;
}

}

bool CanonicalAvdIdentity::valid() const noexcept {
    return !user_sid.empty() && !canonical_avd_location.empty() &&
        !contains_nul(user_sid) && !contains_nul(canonical_avd_location);
}

std::string CanonicalAvdIdentity::key() const {
    if (!valid()) {
        return {};
    }
    return std::to_string(user_sid.size()) + ":" + user_sid +
        std::to_string(canonical_avd_location.size()) + ":" + canonical_avd_location;
}

LaunchReservation::LaunchReservation(std::shared_ptr<SharedState> state, std::string identity_key)
    : state_(std::move(state)), identity_key_(std::move(identity_key)) {}

LaunchReservation::LaunchReservation(LaunchReservation&& other) noexcept
    : state_(std::move(other.state_)), identity_key_(std::move(other.identity_key_)) {}

LaunchReservation& LaunchReservation::operator=(LaunchReservation&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        identity_key_ = std::move(other.identity_key_);
    }
    return *this;
}

LaunchReservation::~LaunchReservation() {
    release();
}

LaunchReservation::operator bool() const noexcept {
    return state_ != nullptr;
}

void LaunchReservation::release() noexcept {
    if (!state_) {
        return;
    }
    auto state = std::move(state_);
    {
        std::lock_guard lock(state->mutex);
        state->identities.erase(identity_key_);
    }
    identity_key_.clear();
}

LaunchReservationTable::LaunchReservationTable()
    : state_(std::make_shared<LaunchReservation::SharedState>()) {}

std::optional<LaunchReservation> LaunchReservationTable::try_acquire(
    const CanonicalAvdIdentity& identity) {
    if (!identity.valid()) {
        return std::nullopt;
    }
    auto key = identity.key();
    std::lock_guard lock(state_->mutex);
    if (!state_->identities.insert(key).second) {
        return std::nullopt;
    }
    return LaunchReservation(state_, std::move(key));
}

JobCreationDecision assess_new_job_creation(const JobCreateResult& result) noexcept {
    return result.state == JobCreateState::Created && result.job != nullptr &&
    result.error.empty() && !result.object_name.empty()
        ? JobCreationDecision::Created
        : JobCreationDecision::RecoveryRequired;
}

RecoveryDecision assess_recovery(const CanonicalAvdIdentity& identity,
    const JournalLoadResult& journal, INamedJobObjectProvider& jobs,
    IJobMembershipQuery& membership) {
    if (!identity.valid()) {
        return recovery_required(RecoveryReason::InvalidIdentity,
            "The canonical AVD identity is invalid or unresolved.");
    }
    switch (journal.state) {
    case JournalLoadState::Missing:
        return recovery_required(RecoveryReason::MetadataMissing,
            "Ownership metadata is missing; an old runtime cannot be ruled out.");
    case JournalLoadState::Corrupt:
        return recovery_required(RecoveryReason::MetadataCorrupt,
            "Ownership metadata is corrupt; an old runtime cannot be ruled out.");
    case JournalLoadState::Failed:
        return recovery_required(RecoveryReason::MetadataReadFailed, journal.error.empty()
            ? "Ownership metadata could not be read."
            : "Ownership metadata could not be read: " + journal.error);
    case JournalLoadState::AccessDenied:
        return recovery_required(RecoveryReason::AccessDenied, journal.error.empty()
            ? "Access to ownership metadata was denied."
            : "Access to ownership metadata was denied: " + journal.error);
    case JournalLoadState::Unsupported:
        return recovery_required(RecoveryReason::MetadataUnsupported, journal.error.empty()
            ? "The ownership metadata schema or storage operation is unsupported."
            : "Ownership metadata is unsupported: " + journal.error);
    case JournalLoadState::IdentityMismatch:
        return recovery_required(RecoveryReason::IdentityMismatch, journal.error.empty()
            ? "Ownership metadata belongs to a different user or AVD identity."
            : "Ownership metadata identity mismatch: " + journal.error);
    case JournalLoadState::Stale:
        return recovery_required(RecoveryReason::MetadataStale, journal.error.empty()
            ? "Ownership metadata is stale and cannot authorize replacement."
            : "Ownership metadata is stale: " + journal.error);
    case JournalLoadState::VerifiedEmpty:
        if (journal.record.has_value() || !journal.error.empty()) {
            return recovery_required(RecoveryReason::MetadataContradictory,
                "The ownership journal's verified-empty result is contradictory or incomplete.");
        }
        if (journal.empty_evidence !=
            EmptyOwnershipEvidence::LiveOwnerHasNoOutstandingGeneration) {
            return recovery_required(RecoveryReason::OwnershipUnknown,
                "A journal empty marker alone cannot establish native ownership is empty.");
        }
        return {RecoveryState::EmptyVerified, RecoveryReason::None,
            "The live owner confirms no outstanding generation and the initialized index agrees."};
    case JournalLoadState::Record:
        break;
    default:
        return recovery_required(RecoveryReason::OwnershipUnknown,
            "The ownership journal reported an unknown state.");
    }

    if (!journal.record.has_value() || !journal.error.empty()) {
        return recovery_required(RecoveryReason::MetadataContradictory,
            "The ownership journal record is missing or contains an error.");
    }
    const auto& record = *journal.record;
    if (!record.identity.valid()) {
        return recovery_required(RecoveryReason::InvalidIdentity,
            "The ownership record contains an invalid AVD identity.");
    }
    if (record.identity.key() != identity.key()) {
        return recovery_required(RecoveryReason::IdentityMismatch,
            "The ownership record belongs to a different AVD identity.");
    }
    if (record.generation_id.empty() || record.named_job_object.empty() ||
        !valid_phase(record.phase)) {
        return recovery_required(RecoveryReason::InvalidMetadata,
            "The ownership record is incomplete or contains an unknown launch phase.");
    }
    if (record.phase < RuntimeLaunchPhase::ProcessCreationAttempted &&
        (record.root_process_id != 0 || record.root_creation_time != 0)) {
        return recovery_required(RecoveryReason::MetadataContradictory,
            "The ownership record contains a root process identity before process creation.");
    }

    const auto opened = jobs.reopen_for_recovery(record.identity, record.generation_id,
        record.named_job_object);
    if (opened.state != JobOpenState::Opened) {
        RecoveryReason reason = RecoveryReason::OwnershipUnknown;
        switch (opened.state) {
        case JobOpenState::NotFound:
            reason = RecoveryReason::KernelObjectMissing;
            break;
        case JobOpenState::AccessDenied:
            reason = RecoveryReason::AccessDenied;
            break;
        case JobOpenState::SecurityMismatch:
            reason = RecoveryReason::SecurityMismatch;
            break;
        case JobOpenState::Unsupported:
            reason = RecoveryReason::KernelObjectUnsupported;
            break;
        case JobOpenState::Failed:
            reason = RecoveryReason::KernelObjectOpenFailed;
            break;
        case JobOpenState::Opened:
            break;
        default:
            break;
        }
        const auto detail = opened.error.empty()
            ? "The recorded named Job Object could not be reopened and validated."
            : "The recorded named Job Object could not be reopened and validated: " + opened.error;
        return recovery_required(reason, detail);
    }
    if (!opened.job || !opened.error.empty()) {
        return recovery_required(RecoveryReason::KernelObjectOpenFailed,
            opened.error.empty()
                ? "The Job Object provider returned no validated handle."
                : "The Job Object provider returned an invalid result: " + opened.error);
    }

    const auto observation = membership.query(*opened.job, record);
    switch (observation.state) {
    case JobMembershipResult::State::Verified:
        if (!observation.error.empty()) {
            return recovery_required(RecoveryReason::KernelObjectQueryFailed,
                "Job Object membership query returned an error: " + observation.error);
        }
        break;
    case JobMembershipResult::State::AccessDenied:
        return recovery_required(RecoveryReason::AccessDenied,
            observation.error.empty()
                ? "Access to query Job Object membership was denied."
                : "Access to query Job Object membership was denied: " + observation.error);
    case JobMembershipResult::State::SecurityMismatch:
        return recovery_required(RecoveryReason::SecurityMismatch,
            observation.error.empty()
                ? "Job Object membership validation found a security mismatch."
                : "Job Object membership validation found a security mismatch: " + observation.error);
    case JobMembershipResult::State::Unsupported:
        return recovery_required(RecoveryReason::KernelObjectUnsupported,
            observation.error.empty()
                ? "The Job Object membership query is not supported."
                : "The Job Object membership query is not supported: " + observation.error);
    case JobMembershipResult::State::Failed:
        return recovery_required(RecoveryReason::KernelObjectQueryFailed,
            observation.error.empty()
                ? "Job Object membership could not be verified."
                : "Job Object membership could not be verified: " + observation.error);
    default:
        return recovery_required(RecoveryReason::OwnershipUnknown,
            "The Job Object membership provider returned an unknown result state.");
    }
    if (observation.active_process_count > 0 || !observation.processes.empty()) {
        return {RecoveryState::RuntimeMayBeActive, RecoveryReason::ActiveMembers,
            "The recorded Job Object has one or more active processes; replacement launch is prohibited."};
    }
    if (record.phase >= RuntimeLaunchPhase::ProcessCreationAttempted) {
        if (record.root_process_id == 0 || record.root_creation_time == 0 ||
            !observation.root_identity_verified) {
            return recovery_required(RecoveryReason::RootIdentityUnverified,
                "The Job Object appears empty, but the recorded root identity is not verified.");
        }
        if (!observation.root_exit_verified) {
            return recovery_required(RecoveryReason::RootExitUnverified,
                "The Job Object appears empty, but root process exit is not verified.");
        }
    }
    return {RecoveryState::EmptyVerified, RecoveryReason::None,
        "The recorded Job Object is empty and the launch record permits shutdown to be verified."};
}

}
