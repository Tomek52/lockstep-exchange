# Replay determinism property tests (ADR-0004). No gRPC: runs under TSan.
lockstep_add_test(lockstep_determinism_tests
  LABELS determinism
  SOURCES
    generator.cpp
    replay_determinism_test.cpp
    file_journal_replay_test.cpp
    digest_stability_test.cpp
    crash_recovery_test.cpp
    restart_determinism_test.cpp
  LINK lockstep::app lockstep::journal)

# These iterate a std::generator (GCC 14 false positive, see CompilerOptions.cmake).
lockstep_allow_std_generator(file_journal_replay_test.cpp)
lockstep_allow_std_generator(digest_stability_test.cpp)
lockstep_allow_std_generator(crash_recovery_test.cpp)
lockstep_allow_std_generator(restart_determinism_test.cpp)
