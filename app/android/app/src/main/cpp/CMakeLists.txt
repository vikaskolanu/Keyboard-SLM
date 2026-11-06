cmake_minimum_required(VERSION 3.22.1)

project("keyboard_slm" CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../engine")

add_library(
    keyboard_slm
    SHARED
    ${ENGINE_DIR}/keyboard_slm_engine.cpp
    ${ENGINE_DIR}/keyboard_slm_c_api.cpp
    keyboard_slm_jni.cpp
)

target_include_directories(
    keyboard_slm
    PRIVATE
    ${ENGINE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    keyboard_slm
    PRIVATE
    log
)
