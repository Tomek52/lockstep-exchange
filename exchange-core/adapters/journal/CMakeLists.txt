# Journal adapter: implements app::Journal. Deliberately free of protobuf and
# gRPC (own binary format, ADR-0012) so the journal and the determinism tests
# can run under ThreadSanitizer with fully instrumented code (ADR-0007).
#
# Header-only in the skeleton; task 008 adds src/file_journal_writer.cpp and
# task 009 adds src/file_journal_reader.cpp, turning this into a STATIC library.
add_library(lockstep_journal INTERFACE)
add_library(lockstep::journal ALIAS lockstep_journal)

target_include_directories(lockstep_journal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lockstep_journal INTERFACE lockstep::app)

lockstep_restrict_links(lockstep_journal ALLOW lockstep::app)
