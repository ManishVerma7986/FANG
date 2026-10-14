#pragma once

#include "error.hpp"

#include <cstdint>
#include <istream>

namespace fang::core {

inline constexpr std::uint32_t current_configuration_version = 1;

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error
};

std::string_view log_level_name(LogLevel level) noexcept;

struct CoreConfig {
    std::uint32_t schema_version{current_configuration_version};
    LogLevel minimum_log_level{LogLevel::Info};

    Result<void> validate() const;
};

CoreConfig default_configuration() noexcept;
Result<CoreConfig> load_configuration(std::istream& input);

}