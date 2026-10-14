#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace fang::host_timing {

struct Options {
    std::uint64_t duration_ms{1000};
    std::uint64_t interval_us{1000};
    std::uint64_t runs{3};
    std::filesystem::path output_file;
};

struct ParseResult {
    Options options;
    bool show_help{};
    std::string error;
};

struct Summary {
    std::int64_t minimum_ns{};
    std::int64_t maximum_ns{};
    double mean_ns{};
    double median_ns{};
    std::int64_t p50_ns{};
    std::int64_t p95_ns{};
    std::int64_t p99_ns{};
};

struct Sample {
    std::int64_t interval_ns{};
    std::int64_t wake_lateness_ns{};
};

struct Trial {
    std::uint64_t sample_count{};
    std::int64_t last_sample_elapsed_ns{};
    Summary interval_summary;
    Summary wake_lateness_summary;
    std::vector<Sample> samples;
};

ParseResult parse_arguments(const std::vector<std::string_view>& arguments);
Summary summarize(const std::vector<std::int64_t>& values);
void print_usage(std::ostream& output);
int run_benchmark(const Options& options, std::ostream& output, std::ostream& errors);

}