# Composition root.
add_executable(exchange-core
  src/main.cpp
  src/options.cpp)

target_link_libraries(exchange-core PRIVATE
  lockstep::app
  lockstep::journal
  lockstep::adapter_grpc
  lockstep::adapter_risk
  lockstep::support
  lockstep::stacktrace
  lockstep::compile_options)

install(TARGETS exchange-core RUNTIME DESTINATION bin)
