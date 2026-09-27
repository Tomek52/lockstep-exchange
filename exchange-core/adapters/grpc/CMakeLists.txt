# Driving adapter: gRPC services (callback API). Task 013 adds
# src/market_data_service.cpp here.
add_library(lockstep_adapter_grpc STATIC
  src/order_entry_service.cpp
  src/server.cpp)
add_library(lockstep::adapter_grpc ALIAS lockstep_adapter_grpc)

target_include_directories(lockstep_adapter_grpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lockstep_adapter_grpc
  PUBLIC lockstep::app lockstep::codec gRPC::grpc++
  PRIVATE lockstep::compile_options)
