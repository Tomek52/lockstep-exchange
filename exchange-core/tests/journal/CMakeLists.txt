lockstep_add_test(lockstep_journal_tests
  LABELS unit
  SOURCES
    format_test.cpp
    byte_io_test.cpp
  LINK lockstep::journal)
