#pragma once

#include "configuration.hpp"
#include "error.hpp"

#include <chrono>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <string_view>

namespace fang::core {

class Logger {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    explicit Logger(
        std::ostream& output,
        LogLevel minimum_level = LogLevel::Info,
        Clock clock = std::chrono::system_clock::now);

    Result<void> write(LogLevel level, std::string_view code, std::string_view context, std::string_view message);
    Result<void> write_error(const CoreError& error);

private:
    std::ostream& output_;
    LogLevel minimum_level_;
    Clock clock_;
    std::mutex mutex_;
};

}