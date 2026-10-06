# Composition-root helpers shared between exchange-core and lockstep-replay
# (task 010). Compiles the production shard_digest.cpp directly, since it is
# a main/ source rather than a library target.
lockstep_add_test(lockstep_main_tests
  LABELS unit
  SOURCES
    shard_digest_test.cpp
    options_test.cpp
    ${LOCKSTEP_CORE_DIR}/main/src/shard_digest.cpp
    ${LOCKSTEP_CORE_DIR}/main/src/options.cpp
  LINK lockstep::app lockstep::journal lockstep::config)
target_include_directories(lockstep_main_tests PRIVATE ${LOCKSTEP_CORE_DIR}/main/src)

# shard_digest.cpp iterates a std::generator (GCC 14 false positive, see
# CompilerOptions.cmake).
lockstep_allow_std_generator(${LOCKSTEP_CORE_DIR}/main/src/shard_digest.cpp)
