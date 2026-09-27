#pragma once

#include <string_view>

namespace lockstep::app {

/// Unrecoverable-error hook. The runtime calls fatal() when continuing would
/// break an invariant it cannot restore - e.g. a journal write failed, so the
/// write-ahead guarantee of ADR-0004 no longer holds. The composition root
/// installs a handler that logs and prints a std::stacktrace (ADR-0008);
/// without one, fatal() just aborts.
using FatalHandler = void (*)(std::string_view message) noexcept;

void set_fatal_handler(FatalHandler handler) noexcept;

[[noreturn]] void fatal(std::string_view message) noexcept;

}  // namespace lockstep::app
