# Generated protobuf/gRPC code for the shared contracts in proto/, and the
# codec that maps it to domain types. Only adapters may link these.
lockstep_add_proto_library(lockstep_proto
  PROTO_ROOT ${PROJECT_SOURCE_DIR}/proto
  PROTOS
    lockstep/v1/common.proto
    lockstep/v1/order_entry.proto
    lockstep/v1/market_data.proto
    lockstep/v1/risk.proto)
add_library(lockstep::proto ALIAS lockstep_proto)

add_library(lockstep_codec STATIC src/order_entry_codec.cpp)
add_library(lockstep::codec ALIAS lockstep_codec)

target_include_directories(lockstep_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lockstep_codec
  PUBLIC lockstep::app lockstep::proto
  PRIVATE lockstep::compile_options)
