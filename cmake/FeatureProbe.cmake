# Verifies the C++23 baseline this project relies on and detects the gaps that
# need a fallback. Rationale and the probe results per toolchain: ADR-0009.
#
# Two kinds of checks:
#  * hard requirements -> configure fails with a readable message;
#  * optional features  -> recorded in the summary; the code itself selects the
#    fallback with feature-test macros (e.g. __cpp_lib_flat_map), so a newer
#    standard library switches over automatically without touching CMake.

include(CheckCXXSourceCompiles)

function(_lockstep_require name source)
  check_cxx_source_compiles("${source}" LOCKSTEP_HAVE_${name})
  if(NOT LOCKSTEP_HAVE_${name})
    message(FATAL_ERROR
      "lockstep: the toolchain lacks required C++23 feature '${name}'. "
      "Supported baseline: GCC 14+ or Clang 19+ with libstdc++ 14+ (see ADR-0009).")
  endif()
endfunction()

_lockstep_require(expected [[
  #include <expected>
  std::expected<int, char> f() { return std::unexpected('x'); }
  int main() { return f().has_value() ? 1 : 0; }]])

_lockstep_require(print [[
  #include <print>
  int main() { std::println("{}", 1); }]])

_lockstep_require(generator [[
  #include <generator>
  std::generator<int> g() { co_yield 1; }
  int main() { for (int v : g()) { return v - 1; } }]])

_lockstep_require(move_only_function [[
  #include <functional>
  #include <memory>
  int main() {
    std::move_only_function<int() noexcept> f = [p = std::make_unique<int>(0)]() noexcept { return *p; };
    return f(); }]])

_lockstep_require(ranges_to_zip_chunk_by_enumerate [[
  #include <ranges>
  #include <vector>
  int main() {
    std::vector<int> a{1, 1, 2};
    auto z = std::views::zip(a, a) | std::ranges::to<std::vector>();
    auto c = a | std::views::chunk_by(std::ranges::equal_to{}) | std::ranges::to<std::vector>();
    for (auto [i, v] : std::views::enumerate(a)) { (void)i; (void)v; }
    return static_cast<int>(z.size() + c.size()) - 5; }]])

# Clang 19 implements P0847 but does not define __cpp_explicit_this_parameter,
# so this is probed by compiling, not by the macro.
_lockstep_require(deducing_this [[
  struct S { int v = 0;
    template <typename Self> auto& get(this Self& self) { return self.v; } };
  int main() { S s; const S& c = s; s.get() = 1; return c.get() - 1; }]])

_lockstep_require(if_consteval [[
  constexpr int f() { if consteval { return 1; } else { return 2; } }
  static_assert(f() == 1);
  int main() { return f() - 2; }]])

# --- optional: std::flat_map (libstdc++ 15+, libc++ 20+) ----------------------
check_cxx_source_compiles([[
  #include <flat_map>
  int main() { std::flat_map<int, int> m; m[1] = 2; return m.size() == 1 ? 0 : 1; }]]
  LOCKSTEP_HAVE_STD_FLAT_MAP)

# --- std::stacktrace: may need an extra runtime library -----------------------
# libstdc++ 13/14 ship it in libstdc++exp (formerly libstdc++_libbacktrace).
add_library(lockstep_stacktrace INTERFACE)
add_library(lockstep::stacktrace ALIAS lockstep_stacktrace)

set(_lockstep_stacktrace_src [[
  #include <stacktrace>
  int main() { return std::stacktrace::current().empty() ? 1 : 0; }]])
check_cxx_source_compiles("${_lockstep_stacktrace_src}" LOCKSTEP_STACKTRACE_NO_EXTRA_LIB)
if(LOCKSTEP_STACKTRACE_NO_EXTRA_LIB)
  set(LOCKSTEP_STACKTRACE_LINK "(none)")
else()
  set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
  check_cxx_source_compiles("${_lockstep_stacktrace_src}" LOCKSTEP_STACKTRACE_WITH_STDCXXEXP)
  unset(CMAKE_REQUIRED_LIBRARIES)
  if(LOCKSTEP_STACKTRACE_WITH_STDCXXEXP)
    target_link_libraries(lockstep_stacktrace INTERFACE stdc++exp)
    set(LOCKSTEP_STACKTRACE_LINK "stdc++exp")
  else()
    message(FATAL_ERROR "lockstep: std::stacktrace is unavailable (tried no extra lib and -lstdc++exp)")
  endif()
endif()

function(lockstep_print_feature_summary)
  if(LOCKSTEP_HAVE_STD_FLAT_MAP)
    set(_fm "std::flat_map")
  else()
    set(_fm "lockstep::detail::sorted_vector_map (fallback, ADR-0009)")
  endif()
  message(STATUS "lockstep: ---- toolchain summary ---------------------------------")
  message(STATUS "lockstep: compiler        ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "lockstep: build type      ${CMAKE_BUILD_TYPE}")
  message(STATUS "lockstep: sanitizers      '${LOCKSTEP_SANITIZER}'")
  message(STATUS "lockstep: flat_map        ${_fm}")
  message(STATUS "lockstep: stacktrace lib  ${LOCKSTEP_STACKTRACE_LINK}")
  message(STATUS "lockstep: gRPC            ${gRPC_VERSION}, protobuf ${Protobuf_VERSION}")
  message(STATUS "lockstep: -------------------------------------------------------")
endfunction()
