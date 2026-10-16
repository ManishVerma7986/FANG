#pragma once

#include "broker_native_job.hpp"
#include "broker_native_journal.hpp"
#include "broker_protocol.hpp"

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
    OwnershipSupervisor(const OwnershipSupervisor&) = delete;
    OwnershipSupervisor& operator=(const OwnershipSupervisor&) = delete;

    BrokerReply dispatch(const Request& request) override;
    SupervisorLifecycle lifecycle() const;
    bool shutdown_if_empty(std::string& error);

private:
    struct OwnedGeneration {
        RuntimeSessionMetadata metadata;
        std::shared_ptr<INamedJobObject> job;
        bool empty_verified{};
    };

    BrokerReply initialize_store(const Request& request);
    BrokerReply query_identity(const Request& request);
    BrokerReply reserve_identity(const Request& request);
    BrokerReply finalize_empty(const Request& request);
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
    NativeJobMembershipQuery membership_;
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
