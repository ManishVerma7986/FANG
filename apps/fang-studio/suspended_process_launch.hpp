#pragma once

namespace fang::studio {

enum class SuspendedLaunchResult {
    Started,
    JobCreationFailed,
    ProcessCreationFailed,
    AssignmentFailed,
    AssignmentCleanupUnverified,
    ResumeFailed,
    ResumeCleanupUnverified
};

enum class OwnedProcessCleanupResult {
    GracefulExit,
    EscalatedExit,
    EscalationFailed,
    EscalationUnconfirmed,
    Unverified
};

template <typename CreateJob, typename CreateSuspendedProcess, typename AssignToJob,
    typename ResumeProcess, typename AbortSuspendedProcess>
SuspendedLaunchResult launch_suspended_process(CreateJob&& create_job,
    CreateSuspendedProcess&& create_process, AssignToJob&& assign_to_job,
    ResumeProcess&& resume_process, AbortSuspendedProcess&& abort_suspended_process) {
    if (!create_job()) {
        return SuspendedLaunchResult::JobCreationFailed;
    }
    if (!create_process()) {
        return SuspendedLaunchResult::ProcessCreationFailed;
    }
    if (!assign_to_job()) {
        return abort_suspended_process()
            ? SuspendedLaunchResult::AssignmentFailed
            : SuspendedLaunchResult::AssignmentCleanupUnverified;
    }
    if (!resume_process()) {
        return abort_suspended_process()
            ? SuspendedLaunchResult::ResumeFailed
            : SuspendedLaunchResult::ResumeCleanupUnverified;
    }
    return SuspendedLaunchResult::Started;
}

template <typename RequestGracefulClose, typename WaitForExit, typename CanEscalate,
    typename Escalate, typename WaitAfterEscalation>
OwnedProcessCleanupResult shutdown_owned_process(RequestGracefulClose&& request_graceful_close,
    WaitForExit&& wait_for_exit, CanEscalate&& can_escalate, Escalate&& escalate,
    WaitAfterEscalation&& wait_after_escalation) {
    static_cast<void>(request_graceful_close());
    if (wait_for_exit()) {
        return OwnedProcessCleanupResult::GracefulExit;
    }
    if (!can_escalate()) {
        return OwnedProcessCleanupResult::Unverified;
    }
    if (!escalate()) {
        return OwnedProcessCleanupResult::EscalationFailed;
    }
    return wait_after_escalation()
        ? OwnedProcessCleanupResult::EscalatedExit
        : OwnedProcessCleanupResult::EscalationUnconfirmed;
}

}