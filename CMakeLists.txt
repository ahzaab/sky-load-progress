cmake_minimum_required(VERSION 3.21)
project(SkyrimLoadProgress VERSION 0.1.0.0 LANGUAGES CXX DESCRIPTION "Skyrim cell loading progress instrumentation")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
if(MSVC)
    add_compile_options("$<$<CONFIG:Release>:/Zi>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG:FULL;/OPT:REF;/OPT:ICF>")
endif()

set(ENABLE_SKYRIM_SE ON CACHE BOOL "Enable Skyrim SE support" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "Enable Skyrim AE support" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "Enable Skyrim VR support" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Build dependency tests" FORCE)
add_subdirectory(external/CommonLibSSE-NG)
include(external/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

configure_file(cmake/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/Version.h @ONLY)

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "Skyrim Load Progress"
    AUTHOR "dgcoo"
    VERSION ${PROJECT_VERSION}
    USE_ADDRESS_LIBRARY
    SOURCES
        include/LoadProgress.h
        include/PCH.h
        src/LoadProgress.cpp
        src/PCH.cpp
        src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/include)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)

