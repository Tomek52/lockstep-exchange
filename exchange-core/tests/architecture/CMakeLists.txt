# Architecture fitness functions (ADR-0002): each layer may include only what
# it is allowed to depend on. Run with: ctest --preset debug -L architecture

set(_core ${LOCKSTEP_CORE_DIR})

# Domain: standard library only, and only the parts without threads, clocks,
# I/O or randomness.
lockstep_add_include_rule(domain ${_core}/domain
  ALLOWED_PROJECT "lockstep/domain/"
  ALLOWED_STD
    algorithm array bit cassert compare concepts cstddef cstdint expected flat_map
    functional initializer_list iterator limits memory optional ranges span string
    string_view tuple type_traits unordered_map unordered_set utility variant vector version)

# Self-test of the fitness function: it must reject a directory full of
# violations, otherwise the rules could silently pass everything.
lockstep_add_include_rule(fixture_impure_domain ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/impure_domain
  ALLOWED_PROJECT "lockstep/domain/"
  ALLOWED_STD cstdint vector)
set_tests_properties(architecture.fixture_impure_domain_includes PROPERTIES WILL_FAIL TRUE)

lockstep_add_include_rule(concurrency ${_core}/concurrency
  ALLOWED_PROJECT "lockstep/concurrency/")

lockstep_add_include_rule(app ${_core}/app
  ALLOWED_PROJECT "lockstep/domain/" "lockstep/concurrency/" "lockstep/app/")

lockstep_add_include_rule(journal ${_core}/adapters/journal
  ALLOWED_PROJECT "lockstep/domain/" "lockstep/concurrency/" "lockstep/app/" "lockstep/journal/")
