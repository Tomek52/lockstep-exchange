# Cross-cutting helpers for adapters and main (logging). Not for domain/app.
add_library(lockstep_support INTERFACE)
add_library(lockstep::support ALIAS lockstep_support)
target_include_directories(lockstep_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lockstep_support INTERFACE cxx_std_23)
