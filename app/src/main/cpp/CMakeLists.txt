cmake_minimum_required(VERSION 3.22.1)

project(pico_alvr LANGUAGES C CXX)

set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)

# ALVR client_core build output directory
# The header (alvr_client_core.h) and .so files are generated here by cargo xtask
set(ALVR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../ALVR")
set(ALVR_BUILD_DIR "${ALVR_ROOT}/build/alvr_client_core")

# Main native library
add_library(pico_alvr SHARED
    alvr_pico_main.cpp
)

# Include paths: local dir for our header, ALVR build dir for generated header
target_include_directories(pico_alvr
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${ALVR_BUILD_DIR}
)

# Link against ALVR client_core shared library
# The .so is built by `cargo xtask build-client-lib` into build/alvr_client_core/<abi>/
target_link_libraries(pico_alvr
    ${ALVR_BUILD_DIR}/${ANDROID_ABI}/libalvr_client_core.so
    EGL
    GLESv3
    log
    android
)
