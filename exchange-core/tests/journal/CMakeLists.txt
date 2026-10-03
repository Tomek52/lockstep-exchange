lockstep_add_test(lockstep_journal_tests
  LABELS unit
  SOURCES
    format_test.cpp
    byte_io_test.cpp
    crc32c_test.cpp
    record_codec_test.cpp
    file_journal_writer_test.cpp
    file_journal_reader_test.cpp
  LINK lockstep::journal)

# file_journal_reader_test.cpp runs the journal-dump tool on a journal it wrote.
# Only a build-order dependency: the test executable does not link the tool.
add_dependencies(lockstep_journal_tests journal-dump)
target_compile_definitions(lockstep_journal_tests PRIVATE
  LOCKSTEP_JOURNAL_DUMP_PATH="$<TARGET_FILE:journal-dump>")
