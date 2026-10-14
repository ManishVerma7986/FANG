#include "host_timing.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_statistics() {
    const auto summary = fang::host_timing::summarize({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    require(summary.minimum_ns == 1, "minimum mismatch");
    require(summary.maximum_ns == 10, "maximum mismatch");
    require(std::abs(summary.mean_ns - 5.5) < 0.0001, "mean mismatch");
    require(std::abs(summary.median_ns - 5.5) < 0.0001, "median mismatch");
    require(summary.p50_ns == 5, "nearest-rank p50 mismatch");
    require(summary.p95_ns == 10, "nearest-rank p95 mismatch");
    require(summary.p99_ns == 10, "nearest-rank p99 mismatch");

    const auto single = fang::host_timing::summarize({-4});
    require(single.minimum_ns == -4 && single.maximum_ns == -4, "single-sample bounds mismatch");
    require(single.p50_ns == -4 && single.p95_ns == -4 && single.p99_ns == -4, "single-sample percentile mismatch");
}

void test_argument_validation() {
    const auto valid = fang::host_timing::parse_arguments({
        "--duration-ms", "250", "--interval-us", "500", "--runs", "2", "--output", "timing.json"
    });
    require(valid.error.empty(), "valid arguments rejected");
    require(valid.options.duration_ms == 250, "duration parse mismatch");
    require(valid.options.interval_us == 500, "interval parse mismatch");
    require(valid.options.runs == 2, "runs parse mismatch");
    require(valid.options.output_file == "timing.json", "output path parse mismatch");

    require(!fang::host_timing::parse_arguments({"--duration-ms", "0"}).error.empty(), "zero duration accepted");
        require(!fang::host_timing::parse_arguments({"--duration-ms", "1", "--interval-us", "1001"}).error.empty(), "interval longer than duration accepted");
    require(!fang::host_timing::parse_arguments({"--interval-us", "-1"}).error.empty(), "negative interval accepted");
    require(!fang::host_timing::parse_arguments({"--runs", "101"}).error.empty(), "excessive trial count accepted");
    require(!fang::host_timing::parse_arguments({"--unknown", "1"}).error.empty(), "unknown option accepted");
    require(!fang::host_timing::parse_arguments({"--runs"}).error.empty(), "missing option value accepted");
    require(!fang::host_timing::parse_arguments({"--duration-ms", "not-a-number"}).error.empty(), "malformed integer accepted");
    require(!fang::host_timing::parse_arguments({"--duration-ms", "1000", "--interval-us", "1", "--runs", "2"}).error.empty(), "sample limit was not enforced");
    require(fang::host_timing::parse_arguments({"--help"}).show_help, "help option not recognized");
}

void test_empty_statistics_rejected() {
    bool rejected = false;
    try {
        static_cast<void>(fang::host_timing::summarize({}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "empty sample set was not rejected");
}

}

int main() {
    try {
        test_statistics();
        test_argument_validation();
        test_empty_statistics_rejected();
        std::cout << "3 host timing test groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "test failure: " << exception.what() << '\n';
        return 1;
    }
}