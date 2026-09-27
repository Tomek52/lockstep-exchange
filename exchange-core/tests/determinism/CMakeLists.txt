# Replay determinism property tests (ADR-0004). No gRPC: runs under TSan.
lockstep_add_test(lockstep_determinism_tests
  LABELS determinism
  SOURCES
    replay_determinism_test.cpp
  LINK lockstep::app lockstep::journal)
