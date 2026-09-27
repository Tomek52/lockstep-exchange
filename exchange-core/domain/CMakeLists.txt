# The domain is the heart of the system and depends on nothing but the C++
# standard library: no gRPC, no protobuf, no threads, no I/O (ADR-0002).
add_library(lockstep_domain STATIC
  src/order_book.cpp
  src/risk_state.cpp
  src/shard_engine.cpp)
add_library(lockstep::domain ALIAS lockstep_domain)

target_include_directories(lockstep_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lockstep_domain PRIVATE lockstep::compile_options)

lockstep_restrict_links(lockstep_domain ALLOW lockstep::compile_options)
