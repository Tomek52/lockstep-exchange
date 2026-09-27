#pragma once

// Minimal structured-enough logging for adapters and main. Never used by the
// domain or the app layer (they report through return values / fatal()).

#include <chrono>
#include <cstdio>
#include <format>
#include <print>
#include <string_view>
#include <utility>

namespace lockstep::support {

enum class LogLevel : unsigned char { Info, Warn, Error };

namespace detail {
[[nodiscard]] constexpr std::string_view label(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warn:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
    }
    return "?    ";
}

inline void emit(LogLevel level, std::string_view message) {
    const auto now =
        std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    // One println per line: stdio locks the stream per call, so concurrent
    // log lines do not interleave mid-line.
    std::println(stderr, "{:%FT%T}Z {} {}", now, label(level), message);
}
}  // namespace detail

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    detail::emit(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    detail::emit(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    detail::emit(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace lockstep::support
