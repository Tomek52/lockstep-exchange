# exchange-core: layered as ports-and-adapters (ADR-0002).
#
#   domain        pure business logic            -> (nothing)
#   concurrency   lock-free queues, idle policy  -> Threads
#   app           shard runtime, publisher, ports-> domain, concurrency
#   adapters/*    journal, codec, grpc, risk     -> app (+ gRPC/protobuf where needed)
#   main          composition root               -> everything
#
# Dependency direction is enforced by lockstep_restrict_links() at configure
# time and by include-scanning architecture tests at test time.

add_subdirectory(domain)

if(LOCKSTEP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
