#pragma once

#include "broker_ownership.hpp"

#include <string>
#include <string_view>

namespace fang::broker {

class OwnershipSupervisor;

enum class OwnershipStoreState : std::uint8_t {
    Initialized,
    Missing,
    Corrupt,
    AccessDenied,
    Unsupported,
    Stale,
    Failed
};

enum class OwnershipIndexEntryState : std::uint8_t {
    Pending,
    Owned,
    EmptyVerified
};

struct OwnershipIndexEntry {
    CanonicalAvdIdentity identity;
    std::string runtime_generation_id;
    OwnershipIndexEntryState state{OwnershipIndexEntryState::Pending};
};

struct OwnershipStoreLoadResult {
    OwnershipStoreState state{OwnershipStoreState::Failed};
    std::vector<OwnershipIndexEntry> entries;
    std::string error;
};

class NativeOwnershipJournal final : public IOwnershipJournal {
public:
    NativeOwnershipJournal(std::string user_sid, std::uint32_t owner_session_id,
        std::uint32_t owner_process_id, std::string owner_generation_id,
        std::wstring storage_root = {});

    JournalLoadResult load(const CanonicalAvdIdentity& identity) override;
    bool write_pending(const RuntimeSessionMetadata& metadata, std::string& error) override;
    bool mark_empty_verified(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::string& error) override;
    OwnershipStoreLoadResult load_store();
    bool initialize_store_explicit(std::string& error);

    // Authoritative durable verification for external runners and tools:
    // validates that owner-index.fji and the .fjn journal tombstone prove
    // that the exact expected generation was finalized as EmptyVerified.
    static JournalLoadResult verify_durable_finalized(
        const CanonicalAvdIdentity& identity,
        std::string_view expected_generation,
        const std::wstring& storage_root = {});

    // Preflight inspection: verifies that no active or pending generation exists
    // for this canonical identity in owner-index.fji or the identity journal.
    static bool check_durable_initially_clean(
        const CanonicalAvdIdentity& identity,
        std::string& detail,
        const std::wstring& storage_root = {});

    // Utility helpers for canonical paths and permissions
    static bool get_journal_path(const std::wstring& root,
        const CanonicalAvdIdentity& identity, std::wstring& path, std::string& error);
    static std::wstring get_owner_store_path(const std::wstring& root);
    static bool apply_private_acl(const std::wstring& path,
        const std::string& sid, std::string& error);

private:
    friend class OwnershipSupervisor;
    // Supervisor-only: persists the verified-empty tombstone after the caller
    // has already obtained a native-backed EmptyVerified RecoveryDecision.
    // Never exposed through IOwnershipJournal; direct tombstone writes via
    // mark_empty_verified() fail closed.
    bool write_verified_empty_tombstone(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::string& error);
    bool claim_identity(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::string& error);
    bool mark_identity_owned(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::string& error);
    bool mark_identity_empty(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, const RecoveryDecision& verified_decision,
        std::string& error);
    bool write_next_generation(const RuntimeSessionMetadata& metadata,
        std::string_view previous_generation_id,
        const RecoveryDecision& verified_previous, std::string& error);

    std::string user_sid_;
    std::uint32_t owner_session_id_{};
    std::uint32_t owner_process_id_{};
    std::string owner_generation_id_;
    std::wstring storage_root_;
};

}
