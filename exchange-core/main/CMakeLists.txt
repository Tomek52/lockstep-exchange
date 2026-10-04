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

# Offline tool: prints a shard journal, one line per record.
add_executable(journal-dump src/journal_dump.cpp)
target_link_libraries(journal-dump PRIVATE lockstep::journal lockstep::compile_options)

install(TARGETS exchange-core journal-dump RUNTIME DESTINATION bin)

# journal-dump iterates a std::generator (GCC 14 false positive, see CompilerOptions.cmake).
lockstep_allow_std_generator(src/journal_dump.cpp)
