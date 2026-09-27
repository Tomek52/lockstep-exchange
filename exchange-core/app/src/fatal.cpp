#include "lockstep/app/fatal.hpp"

#include <atomic>
#include <cstdlib>

namespace lockstep::app {

namespace {
std::atomic<FatalHandler> g_handler{nullptr};
}  // namespace

void set_fatal_handler(FatalHandler handler) noexcept {
    // relaxed: the handler is a plain function pointer (code, not data); no
    // other memory needs to become visible together with it.
    g_handler.store(handler, std::memory_order_relaxed);
}

void fatal(std::string_view message) noexcept {
    if (const FatalHandler handler = g_handler.load(std::memory_order_relaxed)) {
        handler(message);
    }
    std::abort();
}

}  // namespace lockstep::app
