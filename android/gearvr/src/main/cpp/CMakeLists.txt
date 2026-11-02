cmake_minimum_required(VERSION 3.22.1)
project(lumen_gearvr C)

if(NOT OVR_SDK_DIR)
    message(FATAL_ERROR "OVR_SDK_DIR not set (ovr.sdk.dir in android/local.properties)")
endif()

add_library(vrapi SHARED IMPORTED)
set_target_properties(vrapi PROPERTIES
    IMPORTED_LOCATION ${OVR_SDK_DIR}/VrApi/Libs/Android/${ANDROID_ABI}/Release/libvrapi.so)

add_library(lumen_gearvr SHARED lumen_gearvr.c)
target_include_directories(lumen_gearvr PRIVATE ${OVR_SDK_DIR}/VrApi/Include)
target_compile_options(lumen_gearvr PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(lumen_gearvr vrapi android jnigraphics log EGL GLESv3)
