cmake_minimum_required(VERSION 3.16)
project(lumen_driver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple)

add_library(driver_lumen SHARED
    src/driver_main.cpp
    src/device_provider.cpp
    src/hmd_device.cpp
    src/pointer_device.cpp
    src/touch_device.cpp
    src/direct_mode.cpp
    src/helper_link.cpp
    src/shared_fd.cpp
    src/pose_receiver.cpp
)
target_include_directories(driver_lumen PRIVATE third_party/openvr)
target_compile_options(driver_lumen PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(driver_lumen PRIVATE Threads::Threads)

# SteamVR looks for driver_<name>.so with no "lib" prefix.
set_target_properties(driver_lumen PROPERTIES PREFIX "")

# Assemble the driver folder SteamVR loads: <root>/driver.vrdrivermanifest,
# <root>/resources/..., <root>/bin/linux64/driver_lumen.so
set(LUMEN_OUT ${CMAKE_BINARY_DIR}/lumen)
add_custom_command(TARGET driver_lumen POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/lumen ${LUMEN_OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LUMEN_OUT}/bin/linux64
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:driver_lumen> ${LUMEN_OUT}/bin/linux64/
)

# Separate process that imports, reads and encodes the frames (see src/direct_mode.h
# for why this can't live inside vrserver).
find_package(CUDAToolkit REQUIRED)
add_executable(lumen-capture helper/main.cpp helper/video_encoder.cpp helper/cuda_frames.cpp
    helper/udp_streamer.cpp helper/audio_streamer.cpp helper/frame_reader.cpp)
target_include_directories(lumen-capture PRIVATE src)
target_compile_options(lumen-capture PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(lumen-capture PRIVATE Vulkan::Vulkan ZLIB::ZLIB PkgConfig::FFMPEG CUDA::cuda_driver CUDA::nvrtc
    PkgConfig::PULSE)
