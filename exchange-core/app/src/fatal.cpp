#include "lockstep/app/fatal.hpp"

#include <atomic>
#include <cstdlib>

namespace lockstep::app {

namespace {
// The process-wide hook is inherently global; it is written once at startup.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<FatalHandler> g_handler{nullptr};
}  // namespace

void set_fatal_handler(FatalHandler handler) noexcept {
    // relaxed: the handler is a plain function pointer (code, not data); no
    // other memory needs to become visible together with it.
    g_handler.store(handler, std::memory_order_relaxed);
}

void fatal(std::string_view message) noexcept {
    // relaxed: nothing to synchronise with; see set_fatal_handler().
    if (const FatalHandler handler = g_handler.load(std::memory_order_relaxed)) {
        handler(message);
    }
    std::abort();
}

}  // namespace lockstep::app
