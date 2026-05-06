cmake_minimum_required(VERSION 3.16)
project(filtrox_xmp_core_refactor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ZLIB REQUIRED)

add_library(xmp_core
    src/xmp_patch.cpp
)

target_include_directories(xmp_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(xmp_core PUBLIC ZLIB::ZLIB)

add_executable(filtrox_xmp
    src/main.cpp
)

target_link_libraries(filtrox_xmp PRIVATE xmp_core)

add_executable(xmp_core_tests
    tests/xmp_core_tests.cpp
)

target_link_libraries(xmp_core_tests PRIVATE xmp_core)

enable_testing()
add_test(NAME xmp_core_tests COMMAND xmp_core_tests)
set_tests_properties(xmp_core_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
