#include "capabilities.hpp"
#include "configuration.hpp"
#include "logger.hpp"
#include "service_host.hpp"

#include <chrono>
#include <atomic>
#include <iostream>
#include <latch>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace fang::core;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_configuration() {
    const auto defaults = default_configuration();
    require(defaults.schema_version == current_configuration_version, "default schema version mismatch");
    require(defaults.minimum_log_level == LogLevel::Info, "default logging config mismatch");
    require(defaults.validate().has_value(), "default configuration did not validate");

    std::istringstream input(
        "# Fang configuration\n"
        "schema_version = 1\n"
        "logging.level = debug\n");
    auto loaded = load_configuration(input);
    require(loaded.has_value(), "valid configuration rejected");
    require(loaded.value().minimum_log_level == LogLevel::Debug, "configured log level mismatch");

    std::istringstream missing_version("logging.level=warning\n");
    auto missing = load_configuration(missing_version);
    require(!missing && missing.error().code == ErrorCode::ConfigurationParseFailed, "missing schema version accepted");

    std::istringstream unsupported("schema_version=9\n");
    auto version = load_configuration(unsupported);
    require(!version && version.error().code == ErrorCode::UnsupportedConfigurationVersion, "unsupported schema version accepted");

    std::istringstream duplicate("schema_version=1\nschema_version=1\n");
    require(!load_configuration(duplicate), "duplicate configuration key accepted");

    std::istringstream unknown("schema_version=1\nlogging.unknown=true\n");
    require(!load_configuration(unknown), "unknown configuration key accepted");

    auto invalid_level = defaults;
    invalid_level.minimum_log_level = static_cast<LogLevel>(99);
    require(!invalid_level.validate(), "invalid programmatic log level accepted");
}

void test_capability_reporting() {
    CapabilityRegistry capabilities;
    require(!capabilities.define(""), "empty capability name accepted");
    require(capabilities.define("host.vulkan").has_value(), "capability definition failed");
    require(!capabilities.define("host.vulkan"), "duplicate capability accepted");
    require(!capabilities.report("guest.opengl", CapabilityState::Verified), "undefined capability was reported");
    require(!capabilities.report("host.vulkan", CapabilityState::Verified), "capability evidence stages were skipped");
    require(capabilities.snapshot().size() == 1, "undefined capability altered the registry");
    require(capabilities.snapshot().front().state == CapabilityState::Unavailable, "new capability did not start unavailable");

    require(capabilities.report("host.vulkan", CapabilityState::Detected, "adapter enumerated").has_value(), "detected report failed");
    require(capabilities.report("host.vulkan", CapabilityState::Initialized, "device creation succeeded").has_value(), "initialized report failed");
    require(capabilities.report("host.vulkan", CapabilityState::Verified, "conformance workload passed").has_value(), "verified report failed");
    const auto snapshot = capabilities.snapshot();
    require(snapshot.front().state == CapabilityState::Verified, "capability state mismatch");
    require(snapshot.front().evidence == "conformance workload passed", "capability evidence mismatch");
    require(capability_state_name(CapabilityState::Verified) == "verified", "capability state serialization mismatch");
    require(capabilities.report("host.vulkan", CapabilityState::Unavailable, "device was lost").has_value(), "capability downgrade failed");
}

void test_structured_logger() {
    std::ostringstream output;
    const auto fixed_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(123456789));
    Logger logger(output, LogLevel::Info, [fixed_time] { return fixed_time; });
    require(logger.write(LogLevel::Debug, "ignored", {}, "filtered").has_value(), "filtered record failed");
    require(output.str().empty(), "filtered record was written");

    require(logger.write(LogLevel::Warning, "device_warning", "display", "line 1\n\"line 2\"").has_value(), "log write failed");
    const auto record = output.str();
    require(record.find("\"timestamp_unix_ms\":123456789") != std::string::npos, "timestamp missing");
    require(record.find("\"level\":\"warning\"") != std::string::npos, "severity missing");
    require(record.find("\"code\":\"device_warning\"") != std::string::npos, "error code missing");
    require(record.find("line 1\\n\\\"line 2\\\"") != std::string::npos, "JSON string escaping failed");

    std::ostringstream error_output;
    Logger error_logger(error_output, LogLevel::Trace, [fixed_time] { return fixed_time; });
    const CoreError error{ErrorCode::InvalidConfiguration, "bad value", "logging.level"};
    require(error_logger.write_error(error).has_value(), "error logging failed");
    require(error_output.str().find("invalid_configuration") != std::string::npos, "stable error code missing from log");

    std::ostringstream failed_output;
    failed_output.setstate(std::ios::badbit);
    Logger failed_logger(failed_output, LogLevel::Trace, [fixed_time] { return fixed_time; });
    auto failed_write = failed_logger.write(LogLevel::Error, "test", {}, "sink failure");
    require(!failed_write && failed_write.error().code == ErrorCode::LogWriteFailed, "logger sink failure was not propagated");

    std::ostringstream concurrent_output;
    Logger concurrent_logger(concurrent_output, LogLevel::Trace, [fixed_time] { return fixed_time; });
    std::atomic<bool> log_failed{};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([worker, &concurrent_logger, &log_failed] {
            for (int index = 0; index < 50; ++index) {
                const auto result = concurrent_logger.write(LogLevel::Info, "concurrent", std::to_string(worker), "record");
                if (!result) {
                    log_failed.store(true);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(!log_failed.load(), "concurrent log write failed");
    std::istringstream lines(concurrent_output.str());
    std::size_t line_count = 0;
    std::string line;
    while (std::getline(lines, line)) {
        require(line.starts_with("{\"timestamp_unix_ms\":"), "concurrent log record is malformed");
        ++line_count;
    }
    require(line_count == 200, "concurrent log records were lost or interleaved");
}

class TestService final : public IService {
public:
    TestService(std::string service_name, std::vector<std::string>& events, bool should_fail = false, bool should_throw = false, bool should_fail_stop = false)
        : service_name_(std::move(service_name)), events_(events), should_fail_(should_fail), should_throw_(should_throw), should_fail_stop_(should_fail_stop) {}

    std::string_view name() const noexcept override { return service_name_; }

    Result<void> start() override {
        events_.push_back("start:" + service_name_);
        if (should_throw_) {
            throw std::runtime_error("startup exception");
        }
        if (should_fail_) {
            return Result<void>::failure({ErrorCode::ServiceInitializationFailed, "startup rejected", "test"});
        }
        return Result<void>::success();
    }

    Result<void> stop() override {
        events_.push_back("stop:" + service_name_);
        if (should_fail_stop_) {
            return Result<void>::failure({ErrorCode::ServiceShutdownFailed, "shutdown rejected", "test"});
        }
        return Result<void>::success();
    }

private:
    std::string service_name_;
    std::vector<std::string>& events_;
    bool should_fail_;
    bool should_throw_;
    bool should_fail_stop_;
};

class BlockingService final : public IService {
public:
    BlockingService(std::latch& entered, std::latch& resume) : entered_(entered), resume_(resume) {}

    std::string_view name() const noexcept override { return "blocking"; }

    Result<void> start() override {
        entered_.count_down();
        resume_.wait();
        return Result<void>::success();
    }

    Result<void> stop() override {
        stopped_ = true;
        return Result<void>::success();
    }

    bool stopped() const noexcept { return stopped_; }

private:
    std::latch& entered_;
    std::latch& resume_;
    bool stopped_{};
};

void test_lifecycle_and_shutdown() {
    std::vector<std::string> events;
    ServiceHost host;
    require(host.state() == LifecycleState::Created, "initial lifecycle state mismatch");
    require(host.add_service(std::make_unique<TestService>("first", events)).has_value(), "first service registration failed");
    require(host.add_service(std::make_unique<TestService>("second", events, false, false, true)).has_value(), "second service registration failed");
    auto duplicate = host.add_service(std::make_unique<TestService>("first", events));
    require(!duplicate && duplicate.error().code == ErrorCode::DuplicateService, "duplicate service name accepted");
    require(host.start().has_value(), "service host failed to start");
    require(host.state() == LifecycleState::Running, "running lifecycle state mismatch");
    require(!host.add_service(std::make_unique<TestService>("late", events)), "service added after startup");
    auto stop_result = host.stop();
    require(!stop_result && stop_result.error().code == ErrorCode::ServiceShutdownFailed, "shutdown error was not propagated");
    require(host.stop().has_value(), "repeated stop was not idempotent");
    require(host.state() == LifecycleState::Stopped, "stopped lifecycle state mismatch");
    require(events == std::vector<std::string>{"start:first", "start:second", "stop:second", "stop:first"}, "shutdown did not continue in reverse order after a failure");
    require(!host.start(), "stopped host restarted unexpectedly");
}

void test_startup_rollback_and_exceptions() {
    std::vector<std::string> events;
    ServiceHost failed_host;
    require(failed_host.add_service(std::make_unique<TestService>("ready", events, false, false, true)).has_value(), "service registration failed");
    require(failed_host.add_service(std::make_unique<TestService>("failed", events, true, false, true)).has_value(), "failing service registration failed");
    require(failed_host.add_service(std::make_unique<TestService>("never_started", events)).has_value(), "later service registration failed");
    auto result = failed_host.start();
    require(!result && result.error().code == ErrorCode::ServiceInitializationFailed, "service startup error was not propagated");
    require(result.error().context.find("rollback failed") != std::string::npos, "rollback failure was not reported with startup failure");
    require(failed_host.state() == LifecycleState::Failed, "startup failure state mismatch");
    require(failed_host.stop().has_value(), "failed host did not shut down cleanly");
    require(events == std::vector<std::string>{"start:ready", "start:failed", "stop:failed", "stop:ready"}, "partial startup rollback mismatch");

    std::vector<std::string> exception_events;
    ServiceHost exception_host;
    require(exception_host.add_service(std::make_unique<TestService>("throws", exception_events, false, true)).has_value(), "throwing service registration failed");
    auto exception_result = exception_host.start();
    require(!exception_result && exception_result.error().message == "startup exception", "startup exception was not translated");
    require(exception_host.state() == LifecycleState::Failed, "exception failure state mismatch");
}

void test_concurrent_start_and_stop() {
    std::latch entered(1);
    std::latch resume(1);
    ServiceHost host;
    auto service = std::make_unique<BlockingService>(entered, resume);
    auto* service_view = service.get();
    require(host.add_service(std::move(service)).has_value(), "blocking service registration failed");

    std::optional<Result<void>> start_result;
    std::optional<Result<void>> stop_result;
    std::thread starting([&host, &start_result] { start_result.emplace(host.start()); });
    entered.wait();
    const bool observed_starting = host.state() == LifecycleState::Starting;
    std::thread stopping([&host, &stop_result] { stop_result.emplace(host.stop()); });
    resume.count_down();
    starting.join();
    stopping.join();

    require(observed_starting, "starting state was not observable during initialization");
    require(start_result && start_result->has_value(), "concurrent start failed");
    require(stop_result && stop_result->has_value(), "concurrent stop failed");
    require(service_view->stopped(), "concurrent stop did not clean up the started service");
    require(host.state() == LifecycleState::Stopped, "concurrent lifecycle did not finish stopped");
}

void test_empty_and_invalid_service_host() {
    ServiceHost empty_host;
    require(empty_host.start().has_value(), "empty host should start");
    require(empty_host.stop().has_value(), "empty host should stop");

    ServiceHost unstarted_host;
    require(unstarted_host.stop().has_value(), "created host should be safely stoppable");
    require(unstarted_host.state() == LifecycleState::Stopped, "created host stop state mismatch");
}

}

int main() {
    try {
        test_configuration();
        test_capability_reporting();
        test_structured_logger();
        test_lifecycle_and_shutdown();
        test_startup_rollback_and_exceptions();
        test_concurrent_start_and_stop();
        test_empty_and_invalid_service_host();
        std::cout << "7 Fang core test groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "test failure: " << exception.what() << '\n';
        return 1;
    }
}