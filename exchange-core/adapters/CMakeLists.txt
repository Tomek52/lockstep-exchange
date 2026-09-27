# Adapters translate between the outside world and the application core.
# Each one depends on app (and through it on domain) - never the reverse.
add_subdirectory(journal)
add_subdirectory(codec)
add_subdirectory(grpc)
add_subdirectory(risk_client)
