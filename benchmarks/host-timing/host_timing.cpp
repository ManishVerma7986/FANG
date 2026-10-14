#include "host_timing.hpp"

#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef FANG_BUILD_TYPE
#define FANG_BUILD_TYPE "unknown"
#endif

namespace fang::host_timing {
namespace {

constexpr std::uint64_t max_sample_count = 1'000'000;
constexpr std::uint64_t max_duration_ms = 3'600'000;
constexpr std::uint64_t max_runs = 100;
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
constexpr std::uint64_t timer_ticks_per_second = 10'000'000;

bool parse_unsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::uint64_t samples_per_trial(const Options& options) {
    const auto duration_us = options.duration_ms * 1000;
    return duration_us / options.interval_us;
}

std::int64_t nearest_rank(const std::vector<std::int64_t>& sorted, std::uint64_t percentile) {
    const auto rank = (percentile * sorted.size() + 99) / 100;
    return sorted[static_cast<std::size_t>(std::max<std::uint64_t>(rank, 1) - 1)];
}

std::int64_t query_counter() {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "QueryPerformanceCounter");
    }
    return counter.QuadPart;
}

std::int64_t counter_ticks_to_ns(std::int64_t ticks, std::int64_t frequency) {
    const auto nanoseconds = static_cast<long double>(ticks) * nanoseconds_per_second / frequency;
    return static_cast<std::int64_t>(std::llround(nanoseconds));
}

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

struct Timer {
    UniqueHandle handle;
    bool high_resolution{};
};

Timer create_timer() {
    HANDLE handle = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (handle != nullptr) {
        return {UniqueHandle(handle), true};
    }

    handle = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (handle == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateWaitableTimer");
    }
    return {UniqueHandle(handle), false};
}

void wait_until(HANDLE timer, std::int64_t deadline, std::int64_t frequency) {
    const auto now = query_counter();
    const auto remaining = deadline - now;
    if (remaining <= 0) {
        return;
    }

    const auto relative_timer_ticks = std::ceil(
        static_cast<long double>(remaining) * timer_ticks_per_second / frequency);
    LARGE_INTEGER due_time{};
    due_time.QuadPart = -std::max<std::int64_t>(1, static_cast<std::int64_t>(relative_timer_ticks));
    if (!SetWaitableTimer(timer, &due_time, 0, nullptr, nullptr, FALSE)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "SetWaitableTimer");
    }

    const auto wait_result = WaitForSingleObject(timer, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "WaitForSingleObject");
    }
}

std::wstring registry_string(const wchar_t* subkey, const wchar_t* value_name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value_name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value_name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::optional<DWORD> registry_dword(const wchar_t* subkey, const wchar_t* value_name) {
    DWORD value{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value_name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return value;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const auto required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

std::string architecture_name() {
    SYSTEM_INFO information{};
    GetNativeSystemInfo(&information);
    switch (information.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x64";
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";
    case PROCESSOR_ARCHITECTURE_ARM64:
        return "ARM64";
    default:
        return "unknown";
    }
}

std::vector<std::string> display_device_names() {
    std::vector<std::string> names;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) {
            break;
        }
        if ((device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) == 0) {
            names.push_back(utf8(device.DeviceString));
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::string compiler_name() {
#if defined(_MSC_FULL_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#elif defined(__clang_version__)
    return "Clang " __clang_version__;
#else
    return "unknown compiler";
#endif
}

std::string utc_timestamp() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << time.wYear << '-'
           << std::setw(2) << time.wMonth << '-' << std::setw(2) << time.wDay << 'T'
           << std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute << ':'
           << std::setw(2) << time.wSecond << '.' << std::setw(3) << time.wMilliseconds << 'Z';
    return output.str();
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(character) << std::dec;
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    return output.str();
}

void write_summary(std::ostream& output, const Summary& summary) {
    output << "{\"minimum\":" << summary.minimum_ns
           << ",\"maximum\":" << summary.maximum_ns
           << ",\"mean\":" << std::setprecision(12) << summary.mean_ns
           << ",\"median\":" << summary.median_ns
           << ",\"p50\":" << summary.p50_ns
           << ",\"p95\":" << summary.p95_ns
           << ",\"p99\":" << summary.p99_ns << '}';
}

struct HostMetadata {
    std::string architecture;
    std::string windows_version;
    std::string windows_display_version;
    std::string cpu;
    std::vector<std::string> display_device_names;
    std::uint64_t physical_memory_bytes{};
};

HostMetadata collect_metadata() {
    constexpr auto windows_key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    constexpr auto cpu_key = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    HostMetadata metadata;
    metadata.architecture = architecture_name();
    const auto major = registry_dword(windows_key, L"CurrentMajorVersionNumber");
    const auto minor = registry_dword(windows_key, L"CurrentMinorVersionNumber");
    const auto build = registry_string(windows_key, L"CurrentBuildNumber");
    if (major && minor && !build.empty()) {
        metadata.windows_version = std::to_string(*major) + "." + std::to_string(*minor) + "." + utf8(build);
    }
    metadata.windows_display_version = utf8(registry_string(windows_key, L"DisplayVersion"));
    metadata.cpu = utf8(registry_string(cpu_key, L"ProcessorNameString"));
    metadata.display_device_names = display_device_names();
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        metadata.physical_memory_bytes = memory.ullTotalPhys;
    }
    return metadata;
}

Trial run_trial(const Options& options, std::int64_t frequency, HANDLE timer) {
    const auto duration_ticks = static_cast<std::int64_t>(std::ceil(
        static_cast<long double>(frequency) * options.duration_ms / 1000.0L));
    const auto count = samples_per_trial(options);
    const auto start = query_counter();
    const auto end = start + duration_ticks;
    auto previous = start;

    Trial trial;
    trial.samples.reserve(static_cast<std::size_t>(count));
    std::vector<std::int64_t> intervals;
    std::vector<std::int64_t> lateness;
    intervals.reserve(static_cast<std::size_t>(count));
    lateness.reserve(static_cast<std::size_t>(count));

    for (std::uint64_t index = 1; index <= count; ++index) {
        const auto deadline_offset = static_cast<std::int64_t>(std::llround(
            static_cast<long double>(index) * frequency * options.interval_us / 1'000'000.0L));
        const auto deadline = start + deadline_offset;
        if (deadline > end) {
            break;
        }
        wait_until(timer, deadline, frequency);
        const auto observed = query_counter();
        const auto interval_ns = counter_ticks_to_ns(observed - previous, frequency);
        const auto wake_lateness_ns = counter_ticks_to_ns(observed - deadline, frequency);
        trial.samples.push_back({interval_ns, wake_lateness_ns});
        intervals.push_back(interval_ns);
        lateness.push_back(wake_lateness_ns);
        previous = observed;
    }

    trial.sample_count = trial.samples.size();
    trial.last_sample_elapsed_ns = counter_ticks_to_ns(previous - start, frequency);
    trial.interval_summary = summarize(intervals);
    trial.wake_lateness_summary = summarize(lateness);
    return trial;
}

void write_json(std::ostream& output, const Options& options, std::int64_t frequency, bool high_resolution, const std::vector<Trial>& trials) {
    const auto metadata = collect_metadata();
    std::vector<std::int64_t> intervals;
    std::vector<std::int64_t> lateness;
    for (const auto& trial : trials) {
        for (const auto& sample : trial.samples) {
            intervals.push_back(sample.interval_ns);
            lateness.push_back(sample.wake_lateness_ns);
        }
    }

    output << "{\n  \"schema_version\": 1,\n  \"kind\": \"fang-host-timing\",\n"
           << "  \"run_timestamp_utc\": \"" << json_escape(utc_timestamp()) << "\",\n"
           << "  \"clock\": {\"source\": \"QueryPerformanceCounter\", \"frequency_hz\": " << frequency << "},\n"
           << "  \"configuration\": {\"duration_ms\": " << options.duration_ms
           << ", \"interval_us\": " << options.interval_us << ", \"runs\": " << options.runs << "},\n"
           << "  \"metadata\": {\"architecture\": \"" << json_escape(metadata.architecture)
           << "\", \"windows_version\": \"" << json_escape(metadata.windows_version)
           << "\", \"windows_display_version\": \"" << json_escape(metadata.windows_display_version)
           << "\", \"cpu\": \"" << json_escape(metadata.cpu)
           << "\", \"physical_memory_bytes\": " << metadata.physical_memory_bytes
            << ", \"display_device_names\": [";
        for (std::size_t index = 0; index < metadata.display_device_names.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << '"' << json_escape(metadata.display_device_names[index]) << '"';
    }
    output << "], \"compiler\": \"" << compiler_name()
           << "\", \"build_type\": \"" << FANG_BUILD_TYPE
           << "\", \"timer_backend\": \"" << (high_resolution ? "high-resolution waitable timer" : "standard waitable timer fallback") << "\"},\n"
           << "  \"percentile_method\": \"nearest-rank; median is the arithmetic midpoint for even sample counts\",\n"
           << "  \"trials\": [\n";

    for (std::size_t trial_index = 0; trial_index < trials.size(); ++trial_index) {
        const auto& trial = trials[trial_index];
        output << "    {\"trial\": " << trial_index + 1 << ", \"sample_count\": " << trial.sample_count
               << ", \"last_sample_elapsed_ns\": " << trial.last_sample_elapsed_ns << ", \"interval_summary_ns\": ";
        write_summary(output, trial.interval_summary);
        output << ", \"wake_lateness_summary_ns\": ";
        write_summary(output, trial.wake_lateness_summary);
        output << ", \"samples\": [";
        for (std::size_t sample_index = 0; sample_index < trial.samples.size(); ++sample_index) {
            if (sample_index != 0) {
                output << ',';
            }
            const auto& sample = trial.samples[sample_index];
            output << "{\"interval_ns\":" << sample.interval_ns
                   << ",\"wake_lateness_ns\":" << sample.wake_lateness_ns << '}';
        }
        output << "]}" << (trial_index + 1 == trials.size() ? "\n" : ",\n");
    }

    output << "  ],\n  \"combined_interval_summary_ns\": ";
    write_summary(output, summarize(intervals));
    output << ",\n  \"combined_wake_lateness_summary_ns\": ";
    write_summary(output, summarize(lateness));
    output << "\n}\n";
    if (!output) {
        throw std::runtime_error("failed while writing benchmark JSON");
    }
}

}

ParseResult parse_arguments(const std::vector<std::string_view>& arguments) {
    ParseResult result;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto option = arguments[index];
        if (option == "--help" || option == "-h") {
            result.show_help = true;
            return result;
        }
        if (option != "--duration-ms" && option != "--interval-us" && option != "--runs" && option != "--output") {
            result.error = "unknown option: " + std::string(option);
            return result;
        }
        if (++index >= arguments.size()) {
            result.error = "missing value for " + std::string(option);
            return result;
        }
        const auto value = arguments[index];
        if (option == "--output") {
            if (value.empty()) {
                result.error = "--output requires a non-empty path";
                return result;
            }
            result.options.output_file = std::filesystem::path(value);
            continue;
        }

        std::uint64_t parsed_value{};
        if (!parse_unsigned(value, parsed_value)) {
            result.error = "invalid non-negative integer for " + std::string(option) + ": " + std::string(value);
            return result;
        }
        if (option == "--duration-ms") {
            result.options.duration_ms = parsed_value;
        } else if (option == "--interval-us") {
            result.options.interval_us = parsed_value;
        } else {
            result.options.runs = parsed_value;
        }
    }

    if (result.options.duration_ms == 0 || result.options.duration_ms > max_duration_ms) {
        result.error = "--duration-ms must be between 1 and 3600000";
    } else if (result.options.interval_us == 0 || result.options.interval_us > 10'000'000) {
        result.error = "--interval-us must be between 1 and 10000000";
    } else if (result.options.interval_us > result.options.duration_ms * 1000) {
        result.error = "--interval-us must not exceed the measurement duration";
    } else if (result.options.runs == 0 || result.options.runs > max_runs) {
        result.error = "--runs must be between 1 and 100";
    } else if (samples_per_trial(result.options) > max_sample_count / result.options.runs) {
        result.error = "requested configuration exceeds the 1000000-sample total limit";
    }
    return result;
}

Summary summarize(const std::vector<std::int64_t>& values) {
    if (values.empty()) {
        throw std::invalid_argument("cannot summarize an empty sample set");
    }
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const auto middle = sorted.size() / 2;
    const auto median = sorted.size() % 2 == 0
        ? (static_cast<long double>(sorted[middle - 1]) + sorted[middle]) / 2.0L
        : static_cast<long double>(sorted[middle]);
    const auto total = std::accumulate(sorted.begin(), sorted.end(), 0.0L);
    return {
        sorted.front(),
        sorted.back(),
        static_cast<double>(total / sorted.size()),
        static_cast<double>(median),
        nearest_rank(sorted, 50),
        nearest_rank(sorted, 95),
        nearest_rank(sorted, 99)
    };
}

void print_usage(std::ostream& output) {
    output << "Fang host timing benchmark (host scheduling only)\n"
           << "Usage: fang-host-timing [options]\n"
           << "  --duration-ms N   measurement window per trial (default: 1000)\n"
           << "  --interval-us N   requested spacing between wakeups (default: 1000)\n"
           << "  --runs N          repeated trials (default: 3)\n"
           << "  --output PATH     write JSON to PATH instead of stdout\n"
           << "  --help            show this help\n";
}

int run_benchmark(const Options& options, std::ostream& output, std::ostream& errors) {
    try {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "QueryPerformanceFrequency");
        }
        auto timer = create_timer();
        std::vector<Trial> trials;
        trials.reserve(static_cast<std::size_t>(options.runs));
        for (std::uint64_t index = 0; index < options.runs; ++index) {
            trials.push_back(run_trial(options, frequency.QuadPart, timer.handle.get()));
        }
        write_json(output, options, frequency.QuadPart, timer.high_resolution, trials);
        return 0;
    } catch (const std::exception& exception) {
        errors << "error: " << exception.what() << '\n';
        return 1;
    }
}

}