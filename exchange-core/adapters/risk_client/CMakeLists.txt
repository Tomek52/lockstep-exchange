# Risk loop adapter: gRPC client of risk-sentinel (ADR-0013).
add_library(lockstep_adapter_risk STATIC src/risk_client.cpp)
add_library(lockstep::adapter_risk ALIAS lockstep_adapter_risk)

target_include_directories(lockstep_adapter_risk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lockstep_adapter_risk
  PUBLIC lockstep::app
  PRIVATE lockstep::codec lockstep::support gRPC::grpc++ lockstep::compile_options)
