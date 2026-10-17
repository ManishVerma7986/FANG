#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "broker_core.hpp"
#include "broker_ownership.hpp"
#include "broker_pipe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace fang::broker;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::uint16_t read_u16(std::span<const std::byte> input, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1]) << 8);
}

std::uint32_t read_u32(std::span<const std::byte> input, std::size_t offset) {
    std::uint32_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index])) <<
            (index * 8);
    }
    return value;
}

void write_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) {
    output[offset] = static_cast<std::byte>(value & 0xff);
    output[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
}

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
}

void test_protocol_validation_and_reply_shape() {
    const Request request{RequestType::Ping, 0x123456789abcdef0ULL};
    const auto frame = encode_request(request);
    const auto decoded = decode_request(frame);
    require(decoded.request.has_value() && decoded.error == ProtocolError::None &&
        decoded.request->request_id == request.request_id &&
        decoded.request->type == RequestType::Ping,
        "A valid versioned request envelope did not decode.");

    auto unsupported = frame;
    write_u16(unsupported, 4, protocol_version + 1);
    require(decode_request(unsupported).error == ProtocolError::UnsupportedVersion,
        "An unsupported protocol version was accepted.");

    auto unknown = frame;
    write_u16(unknown, 6, 0x7fff);
    require(decode_request(unknown).error == ProtocolError::UnknownType,
        "An unknown message type was accepted.");

    auto oversized = frame;
    write_u32(oversized, 16, static_cast<std::uint32_t>(maximum_payload_size + 1));
    require(decode_request(oversized).error == ProtocolError::Oversized,
        "An oversized message was accepted.");

    require(decode_request(std::span<const std::byte>(frame).first(wire_header_size - 1)).error ==
        ProtocolError::Truncated, "A truncated header was accepted.");

    auto malformed = frame;
    malformed.resize(wire_header_size + 1);
    write_u32(malformed, 16, 1);
    require(decode_request(malformed).error == ProtocolError::UnexpectedPayload,
        "A request with an unsupported payload was accepted.");

    auto incomplete_payload = frame;
    write_u32(incomplete_payload, 16, 2);
    incomplete_payload.push_back(std::byte{0});
    require(decode_request(incomplete_payload).error == ProtocolError::Truncated,
        "A truncated request payload was accepted.");

    auto bad_magic = frame;
    bad_magic[0] = std::byte{'X'};
    require(decode_request(bad_magic).error == ProtocolError::InvalidMagic,
        "A malformed message signature was accepted.");

    const std::vector<std::byte> reply_data{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    const auto reply_bytes = encode_reply(request.request_id,
        {BrokerErrorCode::None, LifecycleState::Ready, "pong", reply_data});
    const auto reply = decode_reply(reply_bytes);
    require(reply.reply.has_value() && reply.error == ProtocolError::None &&
        reply.request_id == request.request_id && reply.reply->message == "pong" &&
        reply.reply->state == LifecycleState::Ready && reply.reply->data == reply_data,
        "The broker response did not preserve its request identifier, status, and structured data.");

    auto malformed_reply = reply_bytes;
    write_u32(malformed_reply, wire_header_size + 7, 4);
    require(decode_reply(malformed_reply).error == ProtocolError::MalformedReply,
        "A reply with an inconsistent structured-data length was accepted.");

    const StartReferenceRuntimeRequest start_request{
        reference_runtime_configuration_version, "FANG-StageA-API36-x86_64"};
    const auto start_payload = encode_start_reference_runtime_request(start_request);
    const auto decoded_start = decode_start_reference_runtime_request(start_payload);
    require(decoded_start.has_value() && decoded_start->avd_name == start_request.avd_name,
        "A valid reference-runtime start request did not round-trip.");
    auto invalid_avd_payload = start_payload;
    invalid_avd_payload.back() = std::byte{'/'};
    require(!decode_start_reference_runtime_request(invalid_avd_payload).has_value(),
        "A reference-runtime request containing a path separator was accepted.");
    auto unsupported_config_payload = start_payload;
    unsupported_config_payload[0] = std::byte{2};
    require(!decode_start_reference_runtime_request(unsupported_config_payload).has_value(),
        "An unsupported reference-runtime configuration version was accepted.");

    const RuntimeGenerationRequest generation_request{"01234567-89ab-4cde-8fab-0123456789ab"};
    const auto generation_payload = encode_runtime_generation_request(generation_request);
    const auto decoded_generation = decode_runtime_generation_request(generation_payload);
    require(decoded_generation.has_value() &&
        decoded_generation->generation_id == generation_request.generation_id,
        "A valid runtime generation request did not round-trip.");
    require(!decode_runtime_generation_request(start_payload).has_value(),
        "A non-generation payload was accepted as a runtime generation request.");

    RuntimeStatusData runtime_status;
    runtime_status.state = RuntimeLifecycleState::GuestBooting;
    runtime_status.failure_stage = RuntimeFailureStage::None;
    runtime_status.process_id = 4200;
    runtime_status.process_creation_time = 987654321;
    runtime_status.active_process_count = 2;
    runtime_status.elapsed_ms = 12000;
    runtime_status.generation_id = generation_request.generation_id;
    runtime_status.avd_name = start_request.avd_name;
    runtime_status.adb_serial = "emulator-5554";
    runtime_status.diagnostic = "Waiting for sys.boot_completed=1";
    const auto runtime_status_bytes = encode_runtime_status_data(runtime_status);
    const auto decoded_runtime_status = decode_runtime_status_data(runtime_status_bytes);
    require(decoded_runtime_status.has_value() &&
        decoded_runtime_status->state == RuntimeLifecycleState::GuestBooting &&
        decoded_runtime_status->process_id == runtime_status.process_id &&
        decoded_runtime_status->process_creation_time == runtime_status.process_creation_time &&
        decoded_runtime_status->active_process_count == runtime_status.active_process_count &&
        decoded_runtime_status->generation_id == runtime_status.generation_id &&
        decoded_runtime_status->adb_serial == runtime_status.adb_serial &&
        decoded_runtime_status->diagnostic == runtime_status.diagnostic,
        "A structured runtime status payload did not round-trip.");
    auto malformed_runtime_status = runtime_status_bytes;
    malformed_runtime_status[2] = std::byte{0xff};
    require(!decode_runtime_status_data(malformed_runtime_status).has_value(),
        "An unknown runtime lifecycle state was accepted.");
}

void test_request_id_deduplication_and_lifecycle() {
    RequestIdWindow identifiers(2);
    require(identifiers.observe(1) && !identifiers.observe(1),
        "A duplicate request identifier was not rejected.");
    require(identifiers.observe(2) && identifiers.observe(3) && identifiers.observe(1),
        "The bounded idempotent-request history did not advance.");
    require(!identifiers.observe(0), "A zero request identifier was accepted.");

    BrokerLifecycle lifecycle;
    BrokerProtocolService service(lifecycle, 4);
    require(lifecycle.state() == LifecycleState::Starting && lifecycle.mark_ready(),
        "The broker did not transition from Starting to Ready.");
    const auto ping = service.dispatch({RequestType::Ping, 12});
    require(ping.error == BrokerErrorCode::None && ping.message == "pong",
        "A valid ping was not handled.");
    const auto duplicate = service.dispatch({RequestType::GetStatus, 12});
    require(duplicate.error == BrokerErrorCode::DuplicateRequestId,
        "A request identifier reused for a different message was accepted.");
    require(service.dispatch({RequestType::GetStatus, 13}).error == BrokerErrorCode::None,
        "A new request identifier was rejected.");
    require(lifecycle.request_shutdown() && lifecycle.state() == LifecycleState::ShuttingDown,
        "The broker did not enter ShuttingDown.");
    require(service.dispatch({RequestType::Ping, 14}).error == BrokerErrorCode::ShuttingDown,
        "The broker accepted a request after shutdown began.");
    require(lifecycle.mark_stopped() && lifecycle.state() == LifecycleState::Stopped,
        "The broker did not finish shutdown.");
    require(!lifecycle.mark_ready(), "The lifecycle accepted an invalid restart transition.");

    BrokerLifecycle failed_lifecycle;
    failed_lifecycle.fail({BrokerErrorCode::InternalFailure, 5, "initialization failed"});
    require(failed_lifecycle.state() == LifecycleState::Failed &&
        failed_lifecycle.last_error().native_error == 5,
        "A structured initialization error was not retained.");
}

void test_client_identity_authorization() {
    constexpr std::string_view sid = "S-1-5-21-100-200-300-1001";
    require(!authorize_client(sid, {10, std::string(sid), 1, 1}),
        "A client with the same user SID and consistent process session was rejected.");
    require(authorize_client(sid, {10, "S-1-5-21-100-200-300-1002", 1, 1}).code ==
        BrokerErrorCode::UnauthorizedClient,
        "A different user SID was accepted.");
    require(authorize_client(sid, {10, std::string(sid), 1, 2}).code ==
        BrokerErrorCode::UnauthorizedClient,
        "A token/process session mismatch was accepted.");
    require(authorize_client(sid, {0, std::string(sid), 1, 1}).code ==
        BrokerErrorCode::UnauthorizedClient,
        "An unverifiable client process identifier was accepted.");
}

CanonicalAvdIdentity identity(std::string path = "C:\\Users\\test\\.android\\avd\\device.avd") {
    return {"S-1-5-21-100-200-300-1001", std::move(path)};
}

void test_concurrent_and_independent_launch_reservations() {
    LaunchReservationTable table;
    constexpr std::size_t callers = 16;
    std::mutex held_mutex;
    std::vector<LaunchReservation> held;
    std::atomic<std::size_t> acquired{};
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < callers; ++index) {
        workers.emplace_back([&] {
            auto reservation = table.try_acquire(identity());
            if (reservation.has_value()) {
                acquired.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard lock(held_mutex);
                held.push_back(std::move(*reservation));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(acquired.load(std::memory_order_relaxed) == 1 && held.size() == 1,
        "Concurrent requests for one canonical AVD identity acquired multiple reservations.");
    require(!table.try_acquire(identity()).has_value(),
        "An active AVD reservation was not exclusive.");

    auto independent = table.try_acquire(identity("D:\\avds\\other.avd"));
    require(independent.has_value(),
        "An independent canonical AVD identity incorrectly shared a reservation.");
    independent.reset();
    held.clear();
    require(table.try_acquire(identity()).has_value(),
        "The AVD reservation was not released deterministically.");
    require(!table.try_acquire({"", "C:\\avd"}).has_value(),
        "An incomplete AVD identity acquired a reservation.");
}

class FakeJob final : public INamedJobObject {};

class FakeJournal final : public IOwnershipJournal {
public:
    JournalLoadResult load(const CanonicalAvdIdentity&) override {
        return result;
    }

    bool write_pending(const RuntimeSessionMetadata&, std::string& error) override {
        ++write_count;
        error = write_error;
        return write_succeeds;
    }

    bool mark_empty_verified(const CanonicalAvdIdentity&, std::string_view,
        std::string& error) override {
        ++empty_count;
        error = empty_error;
        return empty_succeeds;
    }

    JournalLoadResult result{JournalLoadState::VerifiedEmpty, std::nullopt, {}};
    bool write_succeeds{true};
    bool empty_succeeds{true};
    std::string write_error;
    std::string empty_error;
    int write_count{};
    int empty_count{};
};

class FakeJobs final : public INamedJobObjectProvider {
public:
    JobCreateResult create_non_killing(const CanonicalAvdIdentity&,
        std::string_view) override {
        ++create_count;
        return create_result;
    }

    JobOpenResult reopen_for_recovery(const CanonicalAvdIdentity&,
        std::string_view, std::wstring_view name) override {
        ++open_count;
        last_name = name;
        return open_result;
    }

    JobCreateResult create_result{JobCreateState::Created, std::make_shared<FakeJob>(), {},
        L"Local\\Fang.RuntimeBroker.Job.fixture"};
    JobOpenResult open_result{JobOpenState::Opened, std::make_shared<FakeJob>(), {}};
    std::wstring last_name;
    int create_count{};
    int open_count{};
};

class FakeMembership final : public IJobMembershipQuery {
public:
    JobMembershipResult query(INamedJobObject&, const RuntimeSessionMetadata& expected) override {
        ++query_count;
        last_root_process_id = expected.root_process_id;
        last_root_creation_time = expected.root_creation_time;
        return result;
    }

    JobMembershipResult result{JobMembershipResult::State::Verified, 0, true, true, {}, {}};
    int query_count{};
    std::uint32_t last_root_process_id{};
    std::uint64_t last_root_creation_time{};
};

RuntimeSessionMetadata record_for(CanonicalAvdIdentity avd, RuntimeLaunchPhase phase) {
    return {std::move(avd), "generation-01", L"Local\\Fang.RuntimeBroker.Job.generation-01",
        phase, 4321, 987654};
}

void test_missing_corrupt_and_failed_metadata_are_ambiguous() {
    FakeJobs jobs;
    FakeMembership membership;
    const auto invalid_identity = assess_recovery({},
        {JournalLoadState::VerifiedEmpty, std::nullopt, {}}, jobs, membership);
    require(invalid_identity.state == RecoveryState::RecoveryRequired &&
        invalid_identity.reason == RecoveryReason::InvalidIdentity,
        "An invalid requested identity did not fail closed.");
    for (const auto [state, expected_reason] : {
             std::pair{JournalLoadState::Missing, RecoveryReason::MetadataMissing},
             std::pair{JournalLoadState::Corrupt, RecoveryReason::MetadataCorrupt},
             std::pair{JournalLoadState::Failed, RecoveryReason::MetadataReadFailed},
             std::pair{JournalLoadState::AccessDenied, RecoveryReason::AccessDenied},
             std::pair{JournalLoadState::Unsupported, RecoveryReason::MetadataUnsupported},
             std::pair{JournalLoadState::IdentityMismatch, RecoveryReason::IdentityMismatch},
             std::pair{JournalLoadState::Stale, RecoveryReason::MetadataStale}}) {
        const auto decision = assess_recovery(identity(), {state, std::nullopt, "fixture error"},
            jobs, membership);
        require(decision.state == RecoveryState::RecoveryRequired &&
            !decision.replacement_launch_permitted() &&
            decision.reason == expected_reason,
            "Missing, corrupt, or unreadable metadata was treated as proof of an empty runtime.");
    }
    const auto unknown_journal = assess_recovery(identity(),
        {static_cast<JournalLoadState>(0xff), std::nullopt, {}}, jobs, membership);
    require(unknown_journal.state == RecoveryState::RecoveryRequired &&
        unknown_journal.reason == RecoveryReason::OwnershipUnknown,
        "An unknown journal state did not map to explicit unknown ownership.");
    require(jobs.open_count == 0 && membership.query_count == 0,
        "Recovery queried or adopted a Job without a validated ownership record.");

    const auto journal_only_empty = assess_recovery(identity(),
        {JournalLoadState::VerifiedEmpty, std::nullopt, {}}, jobs, membership);
    require(journal_only_empty.state == RecoveryState::RecoveryRequired &&
        journal_only_empty.reason == RecoveryReason::OwnershipUnknown &&
        !journal_only_empty.replacement_launch_permitted(),
        "A journal-only empty marker authorized replacement without native owner evidence.");

    const auto empty = assess_recovery(identity(),
        {JournalLoadState::VerifiedEmpty, std::nullopt, {},
            EmptyOwnershipEvidence::LiveOwnerHasNoOutstandingGeneration}, jobs, membership);
    require(empty.state == RecoveryState::EmptyVerified &&
        empty.replacement_launch_permitted(),
        "A positively initialized empty ownership index did not permit a new generation.");

    const auto contradictory = assess_recovery(identity(),
        {JournalLoadState::VerifiedEmpty, record_for(identity(), RuntimeLaunchPhase::Active), {}},
        jobs, membership);
    require(contradictory.state == RecoveryState::RecoveryRequired &&
        !contradictory.replacement_launch_permitted() &&
        contradictory.reason == RecoveryReason::MetadataContradictory,
        "Contradictory empty-index metadata was treated as empty.");
    auto mismatched_record = record_for(identity("D:\\avds\\different.avd"),
        RuntimeLaunchPhase::Active);
    const auto mismatch = assess_recovery(identity(),
        {JournalLoadState::Record, mismatched_record, {}}, jobs, membership);
    require(mismatch.state == RecoveryState::RecoveryRequired &&
        mismatch.reason == RecoveryReason::IdentityMismatch &&
        !mismatch.replacement_launch_permitted(),
        "A journal record for a different canonical identity was accepted.");
    const auto empty_with_error = assess_recovery(identity(),
        {JournalLoadState::VerifiedEmpty, std::nullopt, "index warning"}, jobs, membership);
    require(empty_with_error.state == RecoveryState::RecoveryRequired &&
        !empty_with_error.replacement_launch_permitted() &&
        empty_with_error.reason == RecoveryReason::MetadataContradictory,
        "An ownership index with a warning was treated as positively empty.");
}

void test_job_creation_and_recovery_fail_closed() {
    FakeJobs jobs;
    FakeMembership membership;

    require(assess_new_job_creation(jobs.create_result) == JobCreationDecision::Created,
        "A newly created non-killing Job abstraction was not accepted.");
    for (const auto state : {JobCreateState::NameCollision, JobCreateState::AccessDenied,
             JobCreateState::SecurityMismatch, JobCreateState::Failed}) {
        jobs.create_result = {state, std::make_shared<FakeJob>(), "fixture failure"};
        require(assess_new_job_creation(jobs.create_result) == JobCreationDecision::RecoveryRequired,
            "A conflicting or insecure named kernel object authorized a new runtime.");
    }
    jobs.create_result = {JobCreateState::Created, nullptr, {}};
    require(assess_new_job_creation(jobs.create_result) == JobCreationDecision::RecoveryRequired,
        "A successful-looking Job creation without a handle was accepted.");
    jobs.create_result = {JobCreateState::Created, std::make_shared<FakeJob>(), "creation warning"};
    require(assess_new_job_creation(jobs.create_result) == JobCreationDecision::RecoveryRequired,
        "A Job creation result with an error was accepted.");
    jobs.create_result = {JobCreateState::Created, std::make_shared<FakeJob>(), {}, {}};
    require(assess_new_job_creation(jobs.create_result) == JobCreationDecision::RecoveryRequired,
        "A created Job without its recoverable object name was accepted.");
    jobs.create_result = {JobCreateState::Unsupported, std::make_shared<FakeJob>(),
        "unsupported fixture", {}};
    require(assess_new_job_creation(jobs.create_result) == JobCreationDecision::RecoveryRequired,
        "An unsupported native creation state was accepted.");

    const auto avd = identity();
    const auto metadata = record_for(avd, RuntimeLaunchPhase::Active);
    const JournalLoadResult journal{JournalLoadState::Record, metadata, {}};
    for (const auto [state, expected_reason] : {
             std::pair{JobOpenState::NotFound, RecoveryReason::KernelObjectMissing},
             std::pair{JobOpenState::AccessDenied, RecoveryReason::AccessDenied},
             std::pair{JobOpenState::SecurityMismatch, RecoveryReason::SecurityMismatch},
             std::pair{JobOpenState::Unsupported, RecoveryReason::KernelObjectUnsupported},
             std::pair{JobOpenState::Failed, RecoveryReason::KernelObjectOpenFailed}}) {
        jobs.open_result = {state, std::make_shared<FakeJob>(), "fixture open failure"};
        const auto decision = assess_recovery(avd, journal, jobs, membership);
        require(decision.state == RecoveryState::RecoveryRequired &&
            !decision.replacement_launch_permitted() &&
            decision.reason == expected_reason,
            "A missing, inaccessible, conflicting, or invalid Job object authorized a replacement.");
    }
    jobs.open_result = {static_cast<JobOpenState>(0xff), std::make_shared<FakeJob>(),
        "unknown open state"};
    auto decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        decision.reason == RecoveryReason::OwnershipUnknown &&
        !decision.replacement_launch_permitted(),
        "An unknown Job-open state did not fail closed as unknown ownership.");

    jobs.open_result = {JobOpenState::Opened, nullptr, {}};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        decision.reason == RecoveryReason::KernelObjectOpenFailed,
        "An opened result without a handle was accepted.");
    jobs.open_result = {JobOpenState::Opened, std::make_shared<FakeJob>(),
        "open warning"};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        decision.reason == RecoveryReason::KernelObjectOpenFailed,
        "An opened result carrying an error was accepted.");

    jobs.open_result = {JobOpenState::Opened, std::make_shared<FakeJob>(), {}};
    membership.result = {JobMembershipResult::State::Failed, 0, false, false, {}, "membership query denied"};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        !decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::KernelObjectQueryFailed,
        "A failed Job membership query was interpreted as empty.");

    membership.result = {JobMembershipResult::State::AccessDenied, 0, false, false, {},
        "membership access denied"};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        decision.reason == RecoveryReason::AccessDenied &&
        !decision.replacement_launch_permitted(),
        "An access-denied membership query did not map to fail-closed AccessDenied.");

    membership.result = {JobMembershipResult::State::SecurityMismatch, 0, false, false, {},
        "membership security mismatch"};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        decision.reason == RecoveryReason::SecurityMismatch &&
        !decision.replacement_launch_permitted(),
        "A membership security mismatch did not fail closed.");

    membership.result = {JobMembershipResult::State::Unsupported, 0, false, false, {},
        "membership query unsupported"};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        decision.reason == RecoveryReason::KernelObjectUnsupported &&
        !decision.replacement_launch_permitted(),
        "An unsupported membership query did not map to fail-closed Unsupported.");

    membership.result = {static_cast<JobMembershipResult::State>(0xff),
        0, false, false, {}, "unknown membership state"};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        decision.reason == RecoveryReason::OwnershipUnknown &&
        !decision.replacement_launch_permitted(),
        "An unknown membership state did not fail closed as unknown ownership.");

    membership.result = {JobMembershipResult::State::Verified, 0, true, true, {}, "membership warning"};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        !decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::KernelObjectQueryFailed,
        "A membership query carrying an error was treated as verified.");

    membership.result = {JobMembershipResult::State::Verified, 2, false, false, {{4321, 987654}, {4322, 987655}}, {}};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RuntimeMayBeActive &&
        !decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::ActiveMembers,
        "A nonempty process group was treated as stopped.");

    membership.result = {JobMembershipResult::State::Verified, 0, false, false,
        {{4322, 987655}}, {}};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RuntimeMayBeActive &&
        decision.reason == RecoveryReason::ActiveMembers &&
        !decision.replacement_launch_permitted(),
        "A contradictory nonempty member list with zero accounting count was treated as empty.");

    membership.result = {JobMembershipResult::State::Verified, 0, false, false, {}, {}};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        !decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::RootIdentityUnverified,
        "An empty count without verified root exit was treated as stopped.");

    membership.result = {JobMembershipResult::State::Verified, 0, false, true, {}, {}};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        !decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::RootIdentityUnverified,
        "A root exit without matching the recorded process identity was treated as stopped.");

    membership.result = {JobMembershipResult::State::Verified, 0, true, false, {}, {}};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        !decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::RootExitUnverified,
        "Verified root identity without verified root exit was treated as stopped.");

    auto incomplete_record = metadata;
    incomplete_record.root_process_id = 0;
    membership.result = {JobMembershipResult::State::Verified, 0, true, true, {}, {}};
    decision = assess_recovery(avd,
        {JournalLoadState::Record, incomplete_record, {}}, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        !decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::RootIdentityUnverified,
        "A missing root identity authorized replacement after process creation was attempted.");

    auto unlaunched = record_for(avd, RuntimeLaunchPhase::JobCreated);
    unlaunched.root_process_id = 0;
    unlaunched.root_creation_time = 0;
    membership.result = {JobMembershipResult::State::Verified, 0, false, false, {}, {}};
    decision = assess_recovery(avd, {JournalLoadState::Record, unlaunched, {}},
        jobs, membership);
    require(decision.state == RecoveryState::EmptyVerified &&
        decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::None,
        "A verified-empty Job created before any process-creation attempt was unnecessarily ambiguous.");

    membership.result = {JobMembershipResult::State::Verified, 0, true, true, {}, {}};
    decision = assess_recovery(avd, journal, jobs, membership);
    require(decision.state == RecoveryState::EmptyVerified &&
        decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::None,
        "A fully verified empty process group did not allow recovery to finish.");
    require(jobs.last_name == metadata.named_job_object &&
        membership.last_root_process_id == metadata.root_process_id &&
        membership.last_root_creation_time == metadata.root_creation_time,
        "Recovery did not validate the recorded Job and root process identity.");

    auto contradictory_record = unlaunched;
    contradictory_record.root_process_id = 4321;
    decision = assess_recovery(avd,
        {JournalLoadState::Record, contradictory_record, {}}, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        !decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::MetadataContradictory,
        "A root identity recorded before process creation was accepted.");

    auto invalid_phase = metadata;
    invalid_phase.phase = static_cast<RuntimeLaunchPhase>(0xff);
    decision = assess_recovery(avd,
        {JournalLoadState::Record, invalid_phase, {}}, jobs, membership);
    require(decision.state == RecoveryState::RecoveryRequired &&
        !decision.replacement_launch_permitted() &&
        decision.reason == RecoveryReason::InvalidMetadata,
        "An unknown runtime launch phase authorized recovery.");
}

struct TestPipeServer {
    BrokerLifecycle lifecycle;
    BrokerProtocolService protocol{lifecycle};
    BrokerPipeServer server;
    std::promise<BrokerError> completion;
    std::shared_future<BrokerError> completion_future;
    std::thread thread;

    explicit TestPipeServer(const UserIdentity& identity, std::uint32_t request_timeout_ms = 5000)
        : server(pipe_name_for_user(identity.sid), identity.sid, lifecycle, protocol,
            request_timeout_ms) {
        completion_future = completion.get_future().share();
        thread = std::thread([this] {
            completion.set_value(server.run());
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (lifecycle.state() == LifecycleState::Starting &&
            std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (lifecycle.state() != LifecycleState::Ready) {
            server.request_stop();
            if (thread.joinable()) {
                thread.join();
            }
            throw std::runtime_error("The named-pipe broker did not become Ready.");
        }
    }

    ~TestPipeServer() {
        server.request_stop();
        if (thread.joinable()) {
            thread.join();
        }
        if (lifecycle.state() == LifecycleState::Failed) {
            const auto error = lifecycle.last_error();
            std::cerr << "broker server failed with code " << static_cast<unsigned int>(error.code)
                      << ", Win32 " << error.native_error << ": " << error.message << '\n';
        }
    }

    BrokerError stop_and_join() {
        require(server.request_stop(), "The broker shutdown event could not be signaled.");
        require(completion_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "The broker did not cancel outstanding pipe I/O during shutdown.");
        auto error = completion_future.get();
        if (thread.joinable()) {
            thread.join();
        }
        return error;
    }
};

HANDLE connect_pipe(std::wstring_view pipe_name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        HANDLE pipe = CreateFileW(std::wstring(pipe_name).c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return pipe;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            break;
        }
        WaitNamedPipeW(std::wstring(pipe_name).c_str(), 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return INVALID_HANDLE_VALUE;
}

bool write_all(HANDLE pipe, const std::byte* bytes, DWORD size) {
    DWORD completed{};
    while (completed < size) {
        DWORD written{};
        if (WriteFile(pipe, bytes + completed, size - completed, &written, nullptr) == 0 ||
            written == 0) {
            return false;
        }
        completed += written;
    }
    return true;
}

bool read_all(HANDLE pipe, std::byte* bytes, DWORD size) {
    DWORD completed{};
    while (completed < size) {
        DWORD received{};
        if (ReadFile(pipe, bytes + completed, size - completed, &received, nullptr) == 0) {
            std::cerr << "named pipe client read failed with Win32 error " << GetLastError() << '\n';
            return false;
        }
        if (received == 0) {
            std::cerr << "named pipe client read returned zero bytes\n";
            return false;
        }
        completed += received;
    }
    return true;
}

ReplyParseResult pipe_request(std::wstring_view pipe_name, std::vector<std::byte> frame,
    bool fragmented = false) {
    HANDLE pipe = connect_pipe(pipe_name);
    require(pipe != INVALID_HANDLE_VALUE, "Could not connect to the local broker pipe.");
    bool sent = true;
    if (fragmented) {
        std::size_t offset{};
        while (offset < frame.size()) {
            const auto chunk = std::min<std::size_t>(3, frame.size() - offset);
            sent = sent && write_all(pipe, frame.data() + offset, static_cast<DWORD>(chunk));
            offset += chunk;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } else {
        sent = write_all(pipe, frame.data(), static_cast<DWORD>(frame.size()));
    }
    require(sent, "Could not write the complete broker request.");

    std::array<std::byte, wire_header_size> header{};
    require(read_all(pipe, header.data(), static_cast<DWORD>(header.size())),
        "Could not read the broker response header.");
    const auto payload_size = read_u32(header, 16);
    require(payload_size <= maximum_payload_size, "The broker returned an oversized response.");
    std::vector<std::byte> response(header.begin(), header.end());
    response.resize(wire_header_size + payload_size);
    if (payload_size > 0) {
        require(read_all(pipe, response.data() + wire_header_size, payload_size),
            "Could not read the complete broker response.");
    }
    const std::byte acknowledgement{0xa5};
    require(write_all(pipe, &acknowledgement, 1),
        "Could not acknowledge the complete broker response.");
    CloseHandle(pipe);
    return decode_reply(response);
}

void test_named_pipe_transport_partial_io_disconnect_and_shutdown() {
    const auto identity_result = current_user_identity();
    require(identity_result.succeeded, "Could not determine the offline test user's identity.");
    const auto& identity = identity_result.identity;
    const auto pipe_name = pipe_name_for_user(identity.sid);
    require(!pipe_name.empty(), "Could not construct the user's broker pipe name.");
    TestPipeServer test_server(identity);
    const auto duplicate_run = test_server.server.run();
    require(duplicate_run.code == BrokerErrorCode::InternalFailure &&
        test_server.lifecycle.state() == LifecycleState::Ready,
        "A concurrent second run changed the active broker lifecycle.");

    auto reply = pipe_request(pipe_name, encode_request({RequestType::Ping, 101}), true);
    require(reply.reply.has_value() && reply.reply->error == BrokerErrorCode::None &&
        reply.reply->message == "pong",
        "The broker did not handle a fragmented named-pipe request.");

    reply = pipe_request(pipe_name, encode_request({RequestType::Ping, 101}));
    require(reply.reply.has_value() && reply.reply->error == BrokerErrorCode::DuplicateRequestId,
        "The broker did not reject a repeated request identifier.");
    reply = pipe_request(pipe_name, encode_request({RequestType::GetStatus, 102}));
    require(reply.reply.has_value() && reply.reply->error == BrokerErrorCode::None &&
        reply.reply->state == LifecycleState::Ready,
        "The broker did not serve a fresh status request after a repeated id.");

    auto unsupported = encode_request({RequestType::Ping, 103});
    write_u16(unsupported, 4, protocol_version + 1);
    reply = pipe_request(pipe_name, std::move(unsupported));
    require(reply.reply.has_value() &&
        reply.reply->error == BrokerErrorCode::UnsupportedVersion,
        "The named-pipe server did not return an explicit unsupported-version error.");

    auto oversized = encode_request({RequestType::Ping, 104});
    write_u32(oversized, 16, static_cast<std::uint32_t>(maximum_payload_size + 1));
    reply = pipe_request(pipe_name, std::move(oversized));
    require(reply.reply.has_value() &&
        reply.reply->error == BrokerErrorCode::OversizedMessage,
        "The named-pipe server did not reject an oversized envelope.");

    auto unknown = encode_request({RequestType::Ping, 105});
    write_u16(unknown, 6, 0x7fff);
    reply = pipe_request(pipe_name, std::move(unknown));
    require(reply.reply.has_value() &&
        reply.reply->error == BrokerErrorCode::UnknownMessageType,
        "The named-pipe server did not reject an unknown message type.");

    auto malformed_signature = encode_request({RequestType::Ping, 109});
    malformed_signature[0] = std::byte{'X'};
    reply = pipe_request(pipe_name, std::move(malformed_signature));
    require(reply.reply.has_value() &&
        reply.reply->error == BrokerErrorCode::InvalidMessage,
        "The named-pipe server did not return an explicit malformed-signature error.");

    auto malformed_payload = encode_request({RequestType::Ping, 106});
    write_u32(malformed_payload, 16, 1);
    malformed_payload.push_back(std::byte{0});
    reply = pipe_request(pipe_name, std::move(malformed_payload));
    require(reply.reply.has_value() &&
        reply.reply->error == BrokerErrorCode::InvalidMessage,
        "The named-pipe server did not reject a malformed payload.");

    HANDLE partial = connect_pipe(pipe_name);
    require(partial != INVALID_HANDLE_VALUE, "Could not connect for the disconnect test.");
    auto partial_request = encode_request({RequestType::Ping, 107});
    require(write_all(partial, partial_request.data(), 7),
        "Could not send the partial request fixture.");
    CloseHandle(partial);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    reply = pipe_request(pipe_name, encode_request({RequestType::Ping, 108}));
    require(reply.reply.has_value() && reply.reply->error == BrokerErrorCode::None,
        "A client disconnect during a partial request stopped the broker from serving clients.");

    HANDLE idle_client = connect_pipe(pipe_name);
    require(idle_client != INVALID_HANDLE_VALUE,
        "Could not connect an idle client before broker shutdown.");
    const auto shutdown_error = test_server.stop_and_join();
    CloseHandle(idle_client);
    require(!shutdown_error && test_server.lifecycle.state() == LifecycleState::Stopped,
        "Broker shutdown with a connected idle client did not complete cleanly.");
}

void test_stalled_request_times_out_and_server_recovers() {
    const auto identity_result = current_user_identity();
    require(identity_result.succeeded, "Could not determine the offline test user's identity.");
    const auto pipe_name = pipe_name_for_user(identity_result.identity.sid);
    TestPipeServer test_server(identity_result.identity, 250);

    HANDLE stalled_client = connect_pipe(pipe_name);
    require(stalled_client != INVALID_HANDLE_VALUE,
        "Could not connect for the stalled-request timeout test.");
    auto partial_request = encode_request({RequestType::Ping, 201});
    require(write_all(stalled_client, partial_request.data(), 7),
        "Could not send the partial request fixture.");
    std::atomic<std::size_t> dribbled_bytes{};
    std::thread dribbler([&] {
        for (std::size_t offset = 7; offset < partial_request.size(); ++offset) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!write_all(stalled_client, partial_request.data() + offset, 1)) {
                break;
            }
            dribbled_bytes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto reply = pipe_request(pipe_name, encode_request({RequestType::Ping, 202}));
    dribbler.join();
    CloseHandle(stalled_client);
    require(reply.reply.has_value() && reply.reply->error == BrokerErrorCode::None &&
        reply.reply->message == "pong",
        "A stalled incomplete request prevented the broker from serving the next client.");
    require(dribbled_bytes.load(std::memory_order_relaxed) < partial_request.size() - 7,
        "A client kept the broker occupied by slowly completing a request past its deadline.");

    HANDLE malformed_client = connect_pipe(pipe_name);
    require(malformed_client != INVALID_HANDLE_VALUE,
        "Could not connect for the malformed-payload timeout test.");
    auto malformed_header = encode_request({RequestType::Ping, 203});
    write_u16(malformed_header, 6, 0x7fff);
    write_u32(malformed_header, 16, 1);
    require(write_all(malformed_client, malformed_header.data(), wire_header_size),
        "Could not send the malformed header fixture.");

    const auto recovery_reply = pipe_request(pipe_name, encode_request({RequestType::Ping, 204}));
    CloseHandle(malformed_client);
    require(recovery_reply.reply.has_value() &&
        recovery_reply.reply->error == BrokerErrorCode::None &&
        recovery_reply.reply->message == "pong",
        "A timeout while discarding a malformed payload stopped the broker.");
    require(!test_server.stop_and_join(), "The broker did not shut down after the timeout test.");
}

void test_initialization_failure_is_reported() {
    BrokerLifecycle lifecycle;
    BrokerProtocolService protocol(lifecycle);
    BrokerPipeServer server(L"\\\\.\\pipe\\Fang.RuntimeBroker.invalid", "not-a-sid",
        lifecycle, protocol);
    const auto error = server.run();
    require(error.code == BrokerErrorCode::InternalFailure &&
        lifecycle.state() == LifecycleState::Failed &&
        lifecycle.last_error().code == BrokerErrorCode::InternalFailure,
        "A failed security-descriptor initialization was not reported as Failed.");
}

}

int main() {
    try {
        test_protocol_validation_and_reply_shape();
        test_request_id_deduplication_and_lifecycle();
        test_client_identity_authorization();
        test_concurrent_and_independent_launch_reservations();
        test_missing_corrupt_and_failed_metadata_are_ambiguous();
        test_job_creation_and_recovery_fail_closed();
        test_named_pipe_transport_partial_io_disconnect_and_shutdown();
        test_stalled_request_times_out_and_server_recovers();
        test_initialization_failure_is_reported();
    } catch (const std::exception& error) {
        std::cerr << "broker test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "9 broker foundation test groups passed\n";
    return 0;
}
