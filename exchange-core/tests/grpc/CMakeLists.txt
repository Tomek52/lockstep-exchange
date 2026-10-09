lockstep_add_test(lockstep_grpc_tests
  LABELS grpc
  SOURCES
    market_data_service_test.cpp
    order_entry_service_test.cpp
  LINK lockstep::adapter_grpc lockstep::journal)
