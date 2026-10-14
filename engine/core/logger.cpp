#include "logger.hpp"

#include <chrono>
#include <ostream>
#include <string_view>

namespace fang::core {
namespace {

void write_json_string(std::ostream& output, std::string_view value) {
    output.put('"');
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
                constexpr char hex[] = "0123456789abcdef";
                output << "\\u00" << hex[(character >> 4) & 0x0f] << hex[character & 0x0f];
            } else {
                output.put(static_cast<char>(character));
            }
        }
    }
    output.put('"');
}

}

Logger::Logger(std::ostream& output, LogLevel minimum_level, Clock clock)
    : output_(output), minimum_level_(minimum_level), clock_(std::move(clock)) {}

Result<void> Logger::write(LogLevel level, std::string_view code, std::string_view context, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(minimum_level_)) {
        return Result<void>::success();
    }

    std::lock_guard lock(mutex_);
    try {
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(clock_().time_since_epoch()).count();
        output_ << "{\"timestamp_unix_ms\":" << timestamp << ",\"level\":";
        write_json_string(output_, log_level_name(level));
        output_ << ",\"code\":";
        write_json_string(output_, code);
        output_ << ",\"context\":";
        write_json_string(output_, context);
        output_ << ",\"message\":";
        write_json_string(output_, message);
        output_ << "}\n";
        if (!output_) {
            return Result<void>::failure({ErrorCode::LogWriteFailed, "failed to write structured log record", "output stream"});
        }
    } catch (const std::exception& exception) {
        return Result<void>::failure({ErrorCode::LogWriteFailed, exception.what(), "output stream"});
    }
    return Result<void>::success();
}

Result<void> Logger::write_error(const CoreError& error) {
    return write(LogLevel::Error, error_code_name(error.code), error.context, error.message);
}

}