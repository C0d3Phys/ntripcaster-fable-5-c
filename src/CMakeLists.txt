set(CORE_SOURCES
  core/io_engine.c
  core/ring_buffer.c
  core/broker.c
  core/mountpoint.c
  core/logger.c
  gnss/rtcm3_frame.c
  gnss/rtcm3_station.c
  gnss/rtcm3_msm.c
  gnss/rtcm3_eph.c
  gnss/rtcm3_decode.c
  protocol/ntrip.c
  protocol/ntrip_common.c
  protocol/ntrip_v1.c
  protocol/ntrip_v2.c
  protocol/sourcetable.c
  protocol/auth.c
)

add_library(ntripcaster_core STATIC ${CORE_SOURCES})
target_include_directories(ntripcaster_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ntripcaster_core
  PUBLIC
    inih_vendor
    uthash_vendor
    Threads::Threads
  PRIVATE
    m
)

add_executable(ntripcaster main.c)
target_link_libraries(ntripcaster PRIVATE ntripcaster_core)
target_include_directories(ntripcaster PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ntripcaster PRIVATE APP_VERSION="${APP_VERSION}")
