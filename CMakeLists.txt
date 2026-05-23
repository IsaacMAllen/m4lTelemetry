# bz.telemetry
# Reusable telemetry framework external for bugbytz Max for Live devices.
#
# This target builds the [bz.telemetry] external as well as the
# `telemetry_core` translation unit which other externals in this repo
# can link against (or include) to use the same framework directly from
# C++ rather than going through a Max object.
#
# macOS-only for now (NSURLSession-based HTTP client).  Windows support
# can be added by providing a parallel WinHTTP implementation of the
# functions declared in telemetry_core.hpp.

cmake_minimum_required(VERSION 3.18)
project(bz.telemetry LANGUAGES C CXX OBJC OBJCXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../min-api/script/min-pretarget.cmake")

set(BZTEL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/bz.telemetry.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/telemetry_core.cpp"
)

if(APPLE)
    list(APPEND BZTEL_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/telemetry_http.mm"
        "${CMAKE_CURRENT_LIST_DIR}/telemetry_crash.mm"
    )
endif()

add_library(${PROJECT_NAME} MODULE ${BZTEL_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../min-api/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../min-api/max-sdk-base/c74support"
    "${CMAKE_CURRENT_LIST_DIR}/../../min-api/max-sdk-base/c74support/max-includes"
    "${CMAKE_CURRENT_LIST_DIR}/../../min-lib/include"
)

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15")
    target_compile_options(${PROJECT_NAME} PRIVATE "-mmacosx-version-min=10.15")
    target_link_options(${PROJECT_NAME}    PRIVATE "-mmacosx-version-min=10.15")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../min-api/script/min-posttarget.cmake")

if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework Foundation"
        "-framework AppKit"
        "-framework CoreFoundation"
    )
endif()
