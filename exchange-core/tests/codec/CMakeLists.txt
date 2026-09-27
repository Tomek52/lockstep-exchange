lockstep_add_test(lockstep_codec_tests
  LABELS grpc
  SOURCES
    order_entry_codec_test.cpp
  LINK lockstep::codec)
