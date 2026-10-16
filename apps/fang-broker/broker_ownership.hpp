#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fang::broker {

struct CanonicalAvdIdentity {
    std::string user_sid;
    std::string canonical_avd_location;

    bool valid() const noexcept;
    std::string key() const;
};

class LaunchReservation {
public:
    LaunchReservation() = default;
    LaunchReservation(const LaunchReservation&) = delete;
    LaunchReservation& operator=(const LaunchReservation&) = delete;
    LaunchReservation(LaunchReservation&& other) noexcept;
    LaunchReservation& operator=(LaunchReservation&& other) noexcept;
    ~LaunchReservation();

    explicit operator bool() const noexcept;
    void release() noexcept;

private:
    struct SharedState;
    friend class LaunchReservationTable;
    LaunchReservation(std::shared_ptr<SharedState> state, std::string identity_key);

    std::shared_ptr<SharedState> state_;
    std::string identity_key_;
};

class IAvdLaunchCoordinator {
public:
    virtual ~IAvdLaunchCoordinator() = default;
    virtual std::optional<LaunchReservation> try_acquire(
        const CanonicalAvdIdentity& identity) = 0;
};

class LaunchReservationTable final : public IAvdLaunchCoordinator {
public:
    LaunchReservationTable();
    std::optional<LaunchReservation> try_acquire(
        const CanonicalAvdIdentity& identity) override;

private:
    std::shared_ptr<LaunchReservation::SharedState> state_;
};

enum class RuntimeLaunchPhase : std::uint8_t {
    Prepared,
    JobCreated,
    ProcessCreationAttempted,
    Active,
    StopRequested
};

struct RuntimeSessionMetadata {
    CanonicalAvdIdentity identity;
    std::string generation_id;
    std::wstring named_job_object;
    RuntimeLaunchPhase phase{RuntimeLaunchPhase::Prepared};
    std::uint32_t root_process_id{};
    std::uint64_t root_creation_time{};
    std::string owner_generation_id;
    std::uint32_t owner_session_id{};
    std::uint32_t owner_process_id{};
};

enum class JournalLoadState : std::uint8_t {
    VerifiedEmpty,
    Record,
    Missing,
    Corrupt,
    Failed,
    AccessDenied,
    Unsupported,
    IdentityMismatch,
    Stale
};

enum class EmptyOwnershipEvidence : std::uint8_t {
    None,
    LiveOwnerHasNoOutstandingGeneration
};

struct JournalLoadResult {
    // VerifiedEmpty is valid only when the durable per-user index was read and
    // positively reports no pending or active generation.
    JournalLoadState state{JournalLoadState::Failed};
    std::optional<RuntimeSessionMetadata> record;
    std::string error;
    EmptyOwnershipEvidence empty_evidence{EmptyOwnershipEvidence::None};
};

class IOwnershipJournal {
public:
    virtual ~IOwnershipJournal() = default;
    virtual JournalLoadResult load(const CanonicalAvdIdentity& identity) = 0;
    virtual bool write_pending(const RuntimeSessionMetadata& metadata, std::string& error) = 0;
    virtual bool mark_empty_verified(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::string& error) = 0;
};

class INamedJobObject {
public:
    virtual ~INamedJobObject() = default;
};

enum class JobOpenState : std::uint8_t {
    Opened,
    NotFound,
    AccessDenied,
    SecurityMismatch,
    Unsupported,
    Failed
};

struct JobOpenResult {
    JobOpenState state{JobOpenState::Failed};
    std::shared_ptr<INamedJobObject> job;
    std::string error;
};

enum class JobCreateState : std::uint8_t {
    Created,
    NameCollision,
    AccessDenied,
    SecurityMismatch,
    Unsupported,
    Failed
};

struct JobCreateResult {
    JobCreateState state{JobCreateState::Failed};
    std::shared_ptr<INamedJobObject> job;
    std::string error;
    std::wstring object_name;
};

enum class JobCreationDecision : std::uint8_t {
    Created,
    RecoveryRequired
};

JobCreationDecision assess_new_job_creation(const JobCreateResult& result) noexcept;

class INamedJobObjectProvider {
public:
    virtual ~INamedJobObjectProvider() = default;
    // A native implementation may return Created only for a newly created,
    // ACL-validated Job with kill-on-close disabled and breakaway prohibited.
    // Existing named objects are collisions, never implicit adoption.
    virtual JobCreateResult create_non_killing(const CanonicalAvdIdentity& identity,
        std::string_view generation_id) = 0;
    // Open only a broker-namespaced name from validated metadata and return
    // Opened only after checking the object's owner, DACL, and Job limits.
    virtual JobOpenResult reopen_for_recovery(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::wstring_view object_name) = 0;
};

struct JobMembershipResult {
    enum class State : std::uint8_t {
        Verified,
        AccessDenied,
        SecurityMismatch,
        Unsupported,
        Failed
    };

    State state{State::Failed};
    std::uint32_t active_process_count{};
    bool root_identity_verified{};
    bool root_exit_verified{};
    // A native query pairs every member PID with its process creation time.
    struct ProcessIdentity {
        std::uint32_t process_id{};
        std::uint64_t creation_time{};
    };
    std::vector<ProcessIdentity> processes;
    std::string error;
};

class IJobMembershipQuery {
public:
    virtual ~IJobMembershipQuery() = default;
    // Failed queries remain unknown. Root verification must compare the stored
    // PID and process creation time, not PID alone.
    virtual JobMembershipResult query(INamedJobObject& job,
        const RuntimeSessionMetadata& expected_runtime) = 0;
};

enum class RecoveryState : std::uint8_t {
    EmptyVerified,
    RuntimeMayBeActive,
    RecoveryRequired
};

enum class RecoveryReason : std::uint8_t {
    None,
    OwnershipUnknown,
    InvalidIdentity,
    IdentityMismatch,
    MetadataMissing,
    MetadataCorrupt,
    MetadataReadFailed,
    MetadataUnsupported,
    MetadataStale,
    MetadataContradictory,
    InvalidMetadata,
    AccessDenied,
    SecurityMismatch,
    KernelObjectMissing,
    KernelObjectOpenFailed,
    KernelObjectQueryFailed,
    KernelObjectUnsupported,
    ActiveMembers,
    RootIdentityUnverified,
    RootExitUnverified
};

struct RecoveryDecision {
    RecoveryState state{RecoveryState::RecoveryRequired};
    RecoveryReason reason{RecoveryReason::OwnershipUnknown};
    std::string detail;

    bool replacement_launch_permitted() const noexcept {
        return state == RecoveryState::EmptyVerified && reason == RecoveryReason::None;
    }
};

RecoveryDecision assess_recovery(const CanonicalAvdIdentity& identity,
    const JournalLoadResult& journal, INamedJobObjectProvider& jobs,
    IJobMembershipQuery& membership);

}
