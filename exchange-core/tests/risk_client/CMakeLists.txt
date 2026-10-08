lockstep_add_test(lockstep_risk_client_tests
  LABELS grpc
  SOURCES
    risk_client_test.cpp
  LINK lockstep::adapter_risk lockstep::codec lockstep::proto)
