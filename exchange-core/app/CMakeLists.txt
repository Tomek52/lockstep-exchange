# Application layer: ports (interfaces the adapters implement or call) and the
# threaded runtime that drives the domain. No gRPC, no protobuf, no files.
add_library(lockstep_app STATIC
  src/engine.cpp
  src/fatal.cpp
  src/publisher.cpp
  src/router.cpp
  src/shard_runtime.cpp
  src/subscription.cpp)
add_library(lockstep::app ALIAS lockstep_app)

target_include_directories(lockstep_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lockstep_app
  PUBLIC lockstep::domain lockstep::concurrency
  PRIVATE lockstep::compile_options)

lockstep_restrict_links(lockstep_app
  ALLOW lockstep::domain lockstep::concurrency lockstep::compile_options)
