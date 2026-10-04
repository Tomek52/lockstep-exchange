# Config adapter: parses the JSON exchange configuration into domain reference
# data (ADR-0019). Header-only JSON (nlohmann), so there is no runtime library
# to ship; it depends only on the domain, never on the app or any other
# adapter (ADR-0002).
find_package(nlohmann_json 3.11 CONFIG REQUIRED)

add_library(lockstep_config STATIC
  src/exchange_config.cpp)
add_library(lockstep::config ALIAS lockstep_config)

target_include_directories(lockstep_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lockstep_config
  PUBLIC lockstep::domain
  PRIVATE nlohmann_json::nlohmann_json lockstep::compile_options)

# domain is the only project dependency; nlohmann_json is header-only and
# compile_options carries only flags, not code (ADR-0002, ADR-0019).
lockstep_restrict_links(lockstep_config
  ALLOW lockstep::domain nlohmann_json::nlohmann_json lockstep::compile_options)
