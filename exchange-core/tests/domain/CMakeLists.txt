lockstep_add_test(lockstep_domain_tests
  LABELS unit
  SOURCES
    strong_int_test.cpp
    checked_math_test.cpp
    flat_map_test.cpp
    validation_test.cpp
    order_book_test.cpp
    order_book_storage_test.cpp
    shard_engine_test.cpp
    matching_test.cpp
  LINK lockstep::domain)
