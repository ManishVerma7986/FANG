#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace fang::core {

enum class ErrorCode {
    InvalidConfiguration,
    InvalidCapabilityReport,
    ConfigurationParseFailed,
    UnsupportedConfigurationVersion,
    InvalidLifecycleTransition,
    InvalidService,
    DuplicateService,
    ServiceInitializationFailed,
    ServiceShutdownFailed,
    LogWriteFailed
};

constexpr std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidConfiguration: return "invalid_configuration";
        case ErrorCode::InvalidCapabilityReport: return "invalid_capability_report";
    case ErrorCode::ConfigurationParseFailed: return "configuration_parse_failed";
    case ErrorCode::UnsupportedConfigurationVersion: return "unsupported_configuration_version";
    case ErrorCode::InvalidLifecycleTransition: return "invalid_lifecycle_transition";
    case ErrorCode::InvalidService: return "invalid_service";
    case ErrorCode::DuplicateService: return "duplicate_service";
    case ErrorCode::ServiceInitializationFailed: return "service_initialization_failed";
        case ErrorCode::ServiceShutdownFailed: return "service_shutdown_failed";
    case ErrorCode::LogWriteFailed: return "log_write_failed";
    }
    return "unknown_error";
}

struct CoreError {
    ErrorCode code;
    std::string message;
    std::string context;
};

template <typename T>
class Result {
public:
    static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result failure(CoreError error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    bool has_value() const noexcept {
        return value_.index() == 0;
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    T& value() {
        if (!has_value()) {
            throw std::logic_error("Result has no value");
        }
        return std::get<0>(value_);
    }

    const T& value() const {
        if (!has_value()) {
            throw std::logic_error("Result has no value");
        }
        return std::get<0>(value_);
    }

    CoreError& error() {
        if (has_value()) {
            throw std::logic_error("Result has no error");
        }
        return std::get<1>(value_);
    }

    const CoreError& error() const {
        if (has_value()) {
            throw std::logic_error("Result has no error");
        }
        return std::get<1>(value_);
    }

private:
    template <std::size_t Index, typename Value>
    explicit Result(std::in_place_index_t<Index> index, Value&& value)
        : value_(index, std::forward<Value>(value)) {}

    std::variant<T, CoreError> value_;
};

template <>
class Result<void> {
public:
    static Result success() {
        return Result(std::nullopt);
    }

    static Result failure(CoreError error) {
        return Result(std::move(error));
    }

    bool has_value() const noexcept {
        return !error_.has_value();
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    const CoreError& error() const {
        if (has_value()) {
            throw std::logic_error("Result has no error");
        }
        return *error_;
    }

private:
    explicit Result(std::optional<CoreError> error) : error_(std::move(error)) {}
    explicit Result(CoreError error) : error_(std::move(error)) {}

    std::optional<CoreError> error_;
};

}