lockstep_add_test(lockstep_app_tests
  LABELS unit
  SOURCES
    engine_pipeline_test.cpp
    publisher_test.cpp
    router_test.cpp
  LINK lockstep::app lockstep::journal)
