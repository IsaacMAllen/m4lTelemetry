# bz.telemetry
# Reusable telemetry framework external for bugbytz Max for Live devices.
#
# This target builds the [bz.telemetry] external as well as the
# `telemetry_core` translation unit which other externals in this repo
# can link against (or include) to use the same framework directly from
# C++ rather than going through a Max object.
#
# Platform support
#   macOS   : NSURLSession HTTP client + POSIX signal crash hooks (.mm files)
#   Windows : WinHTTP client + SetUnhandledExceptionFilter crash hooks (.cpp files)

cmake_minimum_required(VERSION 3.18)

# Declare OBJC/OBJCXX only on Apple; CMake will fail to configure on Windows
# if those language IDs are listed and no Objective-C compiler is found.
if(APPLE)
    project(bz.telemetry LANGUAGES C CXX OBJC OBJCXX)
else()
    project(bz.telemetry LANGUAGES C CXX)
endif()

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
elseif(WIN32)
    list(APPEND BZTEL_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/telemetry_http_win.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/telemetry_crash_win.cpp"
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
elseif(WIN32)
    # Unicode Windows API, no legacy min/max macros, leaner windows.h
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    if(MSVC)
        # Compile source as UTF-8 so string literals in our headers are
        # well-defined; also suppress the secure-CRT "deprecation" warnings
        # for standard POSIX names we intentionally use.
        target_compile_options(${PROJECT_NAME} PRIVATE
            /utf-8
            /wd4996   # 'function': was declared deprecated (secure CRT)
        )
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        winhttp     # WinHttpOpen / WinHttpConnect / ...
        rpcrt4      # UuidCreate
        shell32     # SHGetKnownFolderPath
        ole32       # CoTaskMemFree
        version     # GetFileVersionInfoW / VerQueryValueW
    )
endif()
