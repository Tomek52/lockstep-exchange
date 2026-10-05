# Composition root.
add_executable(exchange-core
  src/main.cpp
  src/options.cpp
  src/engine_config.cpp
  src/shard_digest.cpp)

target_link_libraries(exchange-core PRIVATE
  lockstep::app
  lockstep::journal
  lockstep::config
  lockstep::adapter_grpc
  lockstep::adapter_risk
  lockstep::support
  lockstep::stacktrace
  lockstep::compile_options)

# Offline tool: prints a shard journal, one line per record.
add_executable(journal-dump src/journal_dump.cpp)
target_link_libraries(journal-dump PRIVATE lockstep::journal lockstep::compile_options)

# Offline tool: replays a journal directory and prints each shard's digest
# (task 010), sharing shard_digest.cpp with exchange-core's
# --print-digest-on-exit so the two always agree on the same directory.
add_executable(lockstep-replay
  src/replay_main.cpp
  src/options.cpp
  src/engine_config.cpp
  src/shard_digest.cpp)
target_link_libraries(lockstep-replay PRIVATE
  lockstep::app
  lockstep::journal
  lockstep::config
  lockstep::compile_options)

install(TARGETS exchange-core journal-dump lockstep-replay RUNTIME DESTINATION bin)

# journal-dump/lockstep-replay iterate a std::generator (GCC 14 false
# positive, see CompilerOptions.cmake).
lockstep_allow_std_generator(src/journal_dump.cpp)
lockstep_allow_std_generator(src/shard_digest.cpp)
