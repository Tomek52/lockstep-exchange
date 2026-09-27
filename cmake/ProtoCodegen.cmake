# Protobuf + gRPC code generation from the shared contracts in proto/.
# Both builds (CMake here, tonic-build in rust/crates/lockstep-proto/build.rs)
# compile the same .proto files; CI's proto job guards compatibility (ADR-0006).
#
# gRPC/protobuf come from Ubuntu 24.04 system packages (ADR-0007).

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# lockstep_add_proto_library(<target> PROTO_ROOT <dir> PROTOS <file>...)
#   Generates *.pb.{h,cc} and *.grpc.pb.{h,cc} into the build tree and wraps them
#   in a static library. Generated headers are exposed as SYSTEM includes so that
#   our strict warning set does not apply to code we do not own.
function(lockstep_add_proto_library target)
  cmake_parse_arguments(ARG "" "PROTO_ROOT" "PROTOS" ${ARGN})
  set(_out "${CMAKE_CURRENT_BINARY_DIR}/gen")
  file(MAKE_DIRECTORY "${_out}")

  set(_srcs "")
  set(_hdrs "")
  foreach(_proto IN LISTS ARG_PROTOS)
    string(REGEX REPLACE "\\.proto$" "" _stem "${_proto}")
    set(_gen
      "${_out}/${_stem}.pb.cc" "${_out}/${_stem}.pb.h"
      "${_out}/${_stem}.grpc.pb.cc" "${_out}/${_stem}.grpc.pb.h")
    add_custom_command(
      OUTPUT ${_gen}
      COMMAND protobuf::protoc
        --proto_path=${ARG_PROTO_ROOT}
        --cpp_out=${_out}
        --grpc_out=${_out}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        ${ARG_PROTO_ROOT}/${_proto}
      DEPENDS "${ARG_PROTO_ROOT}/${_proto}" protobuf::protoc gRPC::grpc_cpp_plugin
      COMMENT "protoc ${_proto}"
      VERBATIM)
    list(APPEND _srcs "${_out}/${_stem}.pb.cc" "${_out}/${_stem}.grpc.pb.cc")
    list(APPEND _hdrs "${_out}/${_stem}.pb.h" "${_out}/${_stem}.grpc.pb.h")
  endforeach()

  add_library(${target} STATIC ${_srcs} ${_hdrs})
  target_include_directories(${target} SYSTEM PUBLIC "${_out}")
  target_link_libraries(${target} PUBLIC protobuf::libprotobuf gRPC::grpc++)
  target_compile_features(${target} PUBLIC cxx_std_23)
  # Generated code is not ours to lint.
  set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "")
endfunction()
