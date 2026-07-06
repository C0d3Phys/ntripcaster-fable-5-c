cmake_minimum_required(VERSION 3.20)
project(ntripcaster VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_definitions(_GNU_SOURCE)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-unused-function
  )
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug|Release)" FORCE)
endif()
message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler   : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")

if(NOT DEFINED APP_VERSION)
  set(APP_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "App version: ${APP_VERSION}")

find_package(Threads REQUIRED)

add_library(inih_vendor STATIC vendor/inih/ini.c)
target_include_directories(inih_vendor PUBLIC vendor/inih)

add_library(uthash_vendor INTERFACE)
target_include_directories(uthash_vendor INTERFACE vendor/uthash)

add_library(sha256_vendor STATIC vendor/sha256/sha256.c)
target_include_directories(sha256_vendor PUBLIC vendor/sha256)

add_subdirectory(src)

if(EXISTS ${CMAKE_SOURCE_DIR}/test/CMakeLists.txt)
  enable_testing()
  add_subdirectory(test)
endif()
