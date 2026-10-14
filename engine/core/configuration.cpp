#include "configuration.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace fang::core {
namespace {

std::string_view trim(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

CoreError parse_error(std::size_t line_number, std::string message) {
    return {ErrorCode::ConfigurationParseFailed, std::move(message), "line " + std::to_string(line_number)};
}

Result<LogLevel> parse_log_level(std::string_view value, std::size_t line_number) {
    if (value == "trace") return Result<LogLevel>::success(LogLevel::Trace);
    if (value == "debug") return Result<LogLevel>::success(LogLevel::Debug);
    if (value == "info") return Result<LogLevel>::success(LogLevel::Info);
    if (value == "warning") return Result<LogLevel>::success(LogLevel::Warning);
    if (value == "error") return Result<LogLevel>::success(LogLevel::Error);
    return Result<LogLevel>::failure(parse_error(line_number, "logging.level must be trace, debug, info, warning, or error"));
}

}

std::string_view log_level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    }
    return "unknown";
}

Result<void> CoreConfig::validate() const {
    if (schema_version != current_configuration_version) {
        return Result<void>::failure({
            ErrorCode::UnsupportedConfigurationVersion,
            "configuration schema version is not supported",
            std::to_string(schema_version)
        });
    }
    if (log_level_name(minimum_log_level) == "unknown") {
        return Result<void>::failure({
            ErrorCode::InvalidConfiguration,
            "minimum log level is invalid",
            "logging.level"
        });
    }
    return Result<void>::success();
}

CoreConfig default_configuration() noexcept {
    return {};
}

Result<CoreConfig> load_configuration(std::istream& input) {
    auto config = default_configuration();
    std::unordered_set<std::string> seen_keys;
    std::string line;
    std::size_t line_number = 0;
    bool has_version = false;

    while (std::getline(input, line)) {
        ++line_number;
        auto content = trim(line);
        if (content.empty() || content.front() == '#' || content.front() == ';') {
            continue;
        }

        const auto separator = content.find('=');
        if (separator == std::string_view::npos) {
            return Result<CoreConfig>::failure(parse_error(line_number, "expected key=value"));
        }
        const auto key_view = trim(content.substr(0, separator));
        const auto value = trim(content.substr(separator + 1));
        if (key_view.empty() || value.empty()) {
            return Result<CoreConfig>::failure(parse_error(line_number, "key and value must not be empty"));
        }
        const std::string key(key_view);
        if (!seen_keys.insert(key).second) {
            return Result<CoreConfig>::failure(parse_error(line_number, "duplicate configuration key: " + key));
        }

        if (key == "schema_version") {
            std::uint32_t version{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), version);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                return Result<CoreConfig>::failure(parse_error(line_number, "schema_version must be an unsigned integer"));
            }
            config.schema_version = version;
            has_version = true;
        } else if (key == "logging.level") {
            auto parsed = parse_log_level(value, line_number);
            if (!parsed) {
                return Result<CoreConfig>::failure(parsed.error());
            }
            config.minimum_log_level = parsed.value();
        } else {
            return Result<CoreConfig>::failure(parse_error(line_number, "unknown configuration key: " + key));
        }
    }

    if (input.bad()) {
        return Result<CoreConfig>::failure({
            ErrorCode::ConfigurationParseFailed,
            "failed while reading configuration",
            "input stream"
        });
    }
    if (!has_version) {
        return Result<CoreConfig>::failure({
            ErrorCode::ConfigurationParseFailed,
            "schema_version is required",
            "configuration"
        });
    }

    auto validation = config.validate();
    if (!validation) {
        return Result<CoreConfig>::failure(validation.error());
    }
    return Result<CoreConfig>::success(std::move(config));
}

}