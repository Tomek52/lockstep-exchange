# Header-only concurrency primitives. Knows nothing about the domain: queues
# are generic over the element type (ADR-0002, ADR-0011).
find_package(Threads REQUIRED)

add_library(lockstep_concurrency INTERFACE)
add_library(lockstep::concurrency ALIAS lockstep_concurrency)

target_include_directories(lockstep_concurrency INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lockstep_concurrency INTERFACE Threads::Threads)
target_compile_features(lockstep_concurrency INTERFACE cxx_std_23)

lockstep_restrict_links(lockstep_concurrency ALLOW Threads::Threads)
