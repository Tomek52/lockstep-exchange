lockstep_add_test(lockstep_concurrency_tests
  LABELS unit
  SOURCES
    queue_contract_test.cpp
    idle_strategy_test.cpp
  LINK lockstep::concurrency)
