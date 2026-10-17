#pragma once

#include "broker_native_job.hpp"
#include "broker_native_journal.hpp"
#include "broker_protocol.hpp"
#include "broker_reference_runtime.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace fang::broker {

enum class SupervisorLifecycle : std::uint8_t {
    Starting,
    StoreUninitialized,
    Ready,
    RecoveryRequired,
    ShuttingDown,
    Stopped,
    Failed
};

class OwnershipSupervisor final : public IRequestDispatcher {
public:
    OwnershipSupervisor(std::string user_sid, std::uint32_t session_id,
        std::uint32_t process_id, std::string owner_generation_id,
        std::wstring storage_root = {});
#ifdef FANG_SUPERVISOR_TEST_SEAM
    OwnershipSupervisor(std::string user_sid, std::uint32_t session_id,
        std::uint32_t process_id, std::string owner_generation_id,
        std::wstring storage_root, IJobMembershipQuery& membership);
#endif
    OwnershipSupervisor(const OwnershipSupervisor&) = delete;
    OwnershipSupervisor& operator=(const OwnershipSupervisor&) = delete;

    BrokerReply dispatch(const Request& request) override;
    SupervisorLifecycle lifecycle() const;
    bool shutdown_if_empty(std::string& error);
#ifdef FANG_SUPERVISOR_TEST_SEAM
    // Test-only: exposes the raw supervisor-owned Job handle so integration
    // tests can assign a controlled helper process to the exact owned Job.
    // Never exposed over RPC. The handle remains owned by the supervisor;
    // the caller must not close it.
    void* test_job_handle_for(std::string_view identity_key) const;
    bool test_override_expected_generation(std::string_view identity_key,
        std::string_view generation_id);
#endif

private:
    struct OwnedGeneration {
        RuntimeSessionMetadata metadata;
        std::shared_ptr<INamedJobObject> job;
        std::shared_ptr<ReferenceRuntimeProcess> process;
        ReferenceRuntimeConfig runtime_config;
        RuntimeStatusData runtime_status;
        std::uint64_t started_at_ms{};
        std::uint64_t shutdown_requested_at_ms{};
        bool empty_verified{};
    };

    BrokerReply initialize_store(const Request& request);
    BrokerReply query_identity(const Request& request);
    BrokerReply reserve_identity(const Request& request);
    BrokerReply finalize_empty(const Request& request);
    BrokerReply start_reference_runtime(const Request& request);
    BrokerReply query_runtime_status(const Request& request);
    BrokerReply shutdown_runtime(const Request& request);
    BrokerReply runtime_status_reply(BrokerErrorCode error,
        const RuntimeStatusData& status, std::string message) const;
    bool refresh_runtime_status(OwnedGeneration& generation, std::string& error);
    bool request_graceful_shutdown(OwnedGeneration& generation, std::string& error);
    bool finalize_generation(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, const RecoveryDecision& decision,
        std::string& error);
    BrokerReply status_reply() const;
    BrokerReply fail_closed(BrokerErrorCode code, std::string message) const;

    const std::string user_sid_;
    const std::uint32_t session_id_;
    const std::uint32_t process_id_;
    const std::string owner_generation_id_;
    mutable std::mutex mutex_;
    SupervisorLifecycle lifecycle_{SupervisorLifecycle::Starting};
    RequestIdWindow request_ids_;
    NativeOwnershipJournal journal_;
    NativeNamedJobObjectProvider jobs_;
    NativeJobMembershipQuery native_membership_;
    IJobMembershipQuery& membership_;
    std::unordered_map<std::string, OwnedGeneration> owned_generations_;
};

std::vector<std::byte> encode_identity_payload(const CanonicalAvdIdentity& identity);
std::vector<std::byte> encode_generation_payload(const CanonicalAvdIdentity& identity,
    std::string_view generation_id);
bool decode_identity_payload(std::span<const std::byte> payload,
    CanonicalAvdIdentity& identity, std::string& error);
bool decode_generation_payload(std::span<const std::byte> payload,
    CanonicalAvdIdentity& identity, std::string& generation_id, std::string& error);

}
