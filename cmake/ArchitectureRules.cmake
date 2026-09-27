# Mechanical enforcement of the hexagonal dependency rule (ADR-0002).
#
# lockstep_restrict_links(<target> ALLOW <dep>...)
#   Registers a check that runs at the end of configuration (after every
#   target_link_libraries call has been seen) and fails the configure step if
#   <target> links anything outside the allow-list. This turns "domain must not
#   depend on gRPC/threads/IO" from a code-review convention into a build error.

function(lockstep_restrict_links target)
  cmake_parse_arguments(ARG "" "" "ALLOW" ${ARGN})
  set_property(GLOBAL APPEND PROPERTY LOCKSTEP_RESTRICTED_TARGETS ${target})
  set_property(GLOBAL PROPERTY LOCKSTEP_ALLOW_${target} "${ARG_ALLOW}")
endfunction()

function(_lockstep_check_restricted_links)
  get_property(_targets GLOBAL PROPERTY LOCKSTEP_RESTRICTED_TARGETS)
  foreach(_t IN LISTS _targets)
    get_property(_allowed GLOBAL PROPERTY LOCKSTEP_ALLOW_${_t})
    set(_deps "")
    foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_v ${_t} ${_prop})
      if(_v)
        list(APPEND _deps ${_v})
      endif()
    endforeach()
    list(REMOVE_DUPLICATES _deps)
    foreach(_d IN LISTS _deps)
      # Generator expressions such as $<LINK_ONLY:...> wrap real dependencies.
      string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _d "${_d}")
      if(NOT _d IN_LIST _allowed)
        message(FATAL_ERROR
          "Architecture violation (ADR-0002): target '${_t}' links '${_d}', "
          "which is not in its allow-list: [${_allowed}]")
      endif()
    endforeach()
  endforeach()
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL _lockstep_check_restricted_links)
