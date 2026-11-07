cmake_minimum_required(VERSION 3.20)
project(fh3_airride VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The FH3 Mod Platform checkout (only include/fh3mod.h is needed to build a mod).
set(FH3_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/platform" CACHE PATH "FH3 Mod Platform checkout")
if(NOT EXISTS "${FH3_PLATFORM_DIR}/include/fh3mod.h")
    message(FATAL_ERROR "fh3mod.h not found in ${FH3_PLATFORM_DIR}/include; add the platform as ./platform or pass -DFH3_PLATFORM_DIR=...")
endif()
file(STRINGS "${FH3_PLATFORM_DIR}/CMakeLists.txt" _pv REGEX "^project\\(fh3_mod_platform VERSION")
string(REGEX REPLACE ".*VERSION ([0-9.]+).*" "\\1" FH3_PLATFORM_VERSION "${_pv}")

add_library(airride SHARED src/airride.cpp)
target_include_directories(airride PRIVATE "${FH3_PLATFORM_DIR}/include")
target_compile_definitions(airride PRIVATE
    AIRRIDE_VERSION="${PROJECT_VERSION}"
    FH3MOD_PLATFORM_VERSION="${FH3_PLATFORM_VERSION}"
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(NOT MSVC)
    # -fms-extensions: __try/__except around reads of game memory.
    target_compile_options(airride PRIVATE -Wall -Wextra -fms-extensions "$<$<CONFIG:Release>:-O2>")
    target_link_options(airride PRIVATE -static "$<$<CONFIG:Release>:-s>")
endif()
set_target_properties(airride PROPERTIES OUTPUT_NAME Airride PREFIX "")
