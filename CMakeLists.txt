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
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "Enable instruction-context hooks" FORCE)
add_subdirectory(external/CommonLibSSE-NG)
include(external/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)
find_package(toml11 CONFIG REQUIRED)

configure_file(cmake/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/Version.h @ONLY)

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "Skyrim Load Progress"
    AUTHOR "ahzaab"
    VERSION ${PROJECT_VERSION}
    USE_ADDRESS_LIBRARY
    SOURCES
        include/CellTransitioner.h
        include/FormResolver.h
        include/IdsAndOffsets.h
        include/LoadingProgress.h
        include/PCH.h
        include/ProgressMeter.h
        include/Settings.h
        src/CellTransitioner.cpp
        src/FormResolver.cpp
        src/LoadingProgress.cpp
        src/PCH.cpp
        src/ProgressMeter.cpp
        src/Settings.cpp
        src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/include)

# CommonLib already compiles HDE64 for trampoline patch validation. Expose only its private header to
# this target so semantic hook discovery can walk real instruction boundaries without compiling a
# second decoder or adding another copy of the dependency.
if(SKSE_SUPPORT_PATCH_SAFETY)
    include(FetchContent)
    FetchContent_GetProperties(hde64 SOURCE_DIR HDE64_SOURCE_DIR)
    if(NOT HDE64_SOURCE_DIR)
        message(FATAL_ERROR "CommonLib's HDE64 source directory was not populated")
    endif()
    target_include_directories(${PROJECT_NAME} PRIVATE ${HDE64_SOURCE_DIR}/src/hde)
endif()

target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE toml11::toml11)
