# Sanitizers are applied globally (all targets, including tests) so that every
# translation unit we own is instrumented. Third-party libraries from apt are not
# instrumented; see ADR-0007 for why gRPC-linked tests are excluded from TSan runs.

if(LOCKSTEP_SANITIZER STREQUAL "")
  return()
endif()

if("thread" IN_LIST LOCKSTEP_SANITIZER AND "address" IN_LIST LOCKSTEP_SANITIZER)
  message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
endif()

list(JOIN LOCKSTEP_SANITIZER "," _lockstep_san)
message(STATUS "lockstep: sanitizers enabled: ${_lockstep_san}")

add_compile_options(-fsanitize=${_lockstep_san} -fno-omit-frame-pointer -fno-sanitize-recover=all)
add_link_options(-fsanitize=${_lockstep_san})
