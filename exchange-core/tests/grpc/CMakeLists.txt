lockstep_add_test(lockstep_grpc_tests
  LABELS grpc
  SOURCES
    order_entry_service_test.cpp
  LINK lockstep::adapter_grpc)
