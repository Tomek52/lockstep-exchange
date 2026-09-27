# Architecture fitness function (ADR-0002): scans a layer's sources and fails if
# they include anything the layer must not depend on.
#
# Usage: cmake -DLAYER=<name> -DROOT=<dir>
#              -DALLOWED_PROJECT=<prefix;...>      e.g. "lockstep/domain/"
#              [-DALLOWED_STD=<header;...>]        restrict std headers (domain only)
#              -P check_includes.cmake
#
# Classification of an include target:
#   "lockstep/..."        project header -> must match an ALLOWED_PROJECT prefix
#   "<name>" without '/'  standard header -> must be in ALLOWED_STD if that is set
#   anything else         third-party (grpcpp/, google/, ...) -> always forbidden

cmake_minimum_required(VERSION 3.28)

foreach(_var LAYER ROOT ALLOWED_PROJECT)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "check_includes.cmake: ${_var} is required")
  endif()
endforeach()

file(GLOB_RECURSE _files "${ROOT}/*.hpp" "${ROOT}/*.cpp")
if(NOT _files)
  message(FATAL_ERROR "check_includes.cmake: no sources found under ${ROOT}")
endif()

set(_violations "")
foreach(_file IN LISTS _files)
  file(STRINGS "${_file}" _lines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
  foreach(_line IN LISTS _lines)
    string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"].*$" "\\1" _inc "${_line}")
    set(_ok FALSE)
    if(_inc MATCHES "^lockstep/")
      foreach(_prefix IN LISTS ALLOWED_PROJECT)
        string(FIND "${_inc}" "${_prefix}" _pos)
        if(_pos EQUAL 0)
          set(_ok TRUE)
        endif()
      endforeach()
    elseif(NOT _inc MATCHES "/")
      if(NOT DEFINED ALLOWED_STD OR _inc IN_LIST ALLOWED_STD)
        set(_ok TRUE)
      endif()
    endif()
    if(NOT _ok)
      file(RELATIVE_PATH _rel "${ROOT}" "${_file}")
      list(APPEND _violations "  ${_rel}: #include <${_inc}>")
    endif()
  endforeach()
endforeach()

if(_violations)
  list(JOIN _violations "\n" _msg)
  message(FATAL_ERROR
    "Architecture violation in layer '${LAYER}' (ADR-0002, CLAUDE.md):\n${_msg}")
endif()
message(STATUS "layer '${LAYER}': include rules satisfied")
