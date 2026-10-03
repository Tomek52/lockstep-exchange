# Journal adapter: implements app::Journal. Deliberately free of protobuf and
# gRPC (own binary format, ADR-0012) so the journal and the determinism tests
# can run under ThreadSanitizer with fully instrumented code (ADR-0007).
#
# Task 008 added the record codec and the file writer; task 009 adds
# src/file_journal_reader.cpp.
add_library(lockstep_journal STATIC
  src/record_codec.cpp
  src/file_journal_writer.cpp)
add_library(lockstep::journal ALIAS lockstep_journal)

target_include_directories(lockstep_journal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lockstep_journal
  PUBLIC lockstep::app
  PRIVATE lockstep::compile_options)

# compile_options carries only warning and sanitizer flags, not code; the app
# layer allows it the same way.
lockstep_restrict_links(lockstep_journal ALLOW lockstep::app lockstep::compile_options)
