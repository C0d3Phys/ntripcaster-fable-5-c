add_library(ntrip_test_common STATIC ntrip_tool_common.c)
target_include_directories(ntrip_test_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ntrip_source_relay ntrip_source_relay.c)
target_link_libraries(ntrip_source_relay PRIVATE ntrip_test_common)

add_executable(ntrip_rover_client ntrip_rover_client.c)
target_link_libraries(ntrip_rover_client PRIVATE ntrip_test_common ntripcaster_core)

add_test(NAME ntrip_tools_help_relay COMMAND ntrip_source_relay --help)
add_test(NAME ntrip_tools_help_rover COMMAND ntrip_rover_client --help)
