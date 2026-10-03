lockstep_add_test(lockstep_journal_tests
  LABELS unit
  SOURCES
    format_test.cpp
    byte_io_test.cpp
    crc32c_test.cpp
    record_codec_test.cpp
    file_journal_writer_test.cpp
  LINK lockstep::journal)
