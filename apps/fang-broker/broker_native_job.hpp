#pragma once

#include "broker_ownership.hpp"

#include <string>

namespace fang::broker {

class NativeJobObject final : public INamedJobObject {
public:
    ~NativeJobObject() override;
    NativeJobObject(const NativeJobObject&) = delete;
    NativeJobObject& operator=(const NativeJobObject&) = delete;

    void* native_handle() const noexcept;
    std::wstring_view object_name() const noexcept;

private:
    friend class NativeNamedJobObjectProvider;
    friend class NativeJobMembershipQuery;
    NativeJobObject(void* handle, std::string user_sid, std::wstring object_name) noexcept;

    void* handle_{};
    std::string user_sid_;
    std::wstring object_name_;
};

class NativeNamedJobObjectProvider final : public INamedJobObjectProvider {
public:
    explicit NativeNamedJobObjectProvider(std::string expected_user_sid);

    JobCreateResult create_non_killing(const CanonicalAvdIdentity& identity,
        std::string_view generation_id) override;
    JobOpenResult reopen_for_recovery(const CanonicalAvdIdentity& identity,
        std::string_view generation_id, std::wstring_view object_name) override;

private:
    std::string expected_user_sid_;
};

class NativeJobMembershipQuery final : public IJobMembershipQuery {
public:
    explicit NativeJobMembershipQuery(std::string expected_user_sid);

    JobMembershipResult query(INamedJobObject& job,
        const RuntimeSessionMetadata& expected_runtime) override;

private:
    std::string expected_user_sid_;
};

std::string create_runtime_generation_id(std::string& error);
bool expected_job_object_name(const CanonicalAvdIdentity& identity,
    std::string_view generation_id, std::wstring& object_name, std::string& error);

}
