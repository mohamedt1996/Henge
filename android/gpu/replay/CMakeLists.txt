# SPDX-License-Identifier: GPL-2.0-or-later
# libhengegpu.so, the app's renderer (android/gpu/replay). Built by Gradle's externalNativeBuild.
cmake_minimum_required(VERSION 3.22)
project(hengegpu C)
set(CMAKE_C_STANDARD 11)
add_library(hengegpu SHARED guest.c objects.c pixels.c screen_vk.c shaders.c draw.c replay.c jni.c)
target_compile_options(hengegpu PRIVATE -O2 -Wall -Wno-unused-parameter -D_GNU_SOURCE)
target_link_libraries(hengegpu vulkan android log m)

# Graphics: reims (android/gpu/reims): reims-vgpu, the paravirtual GPU's device on metal2vulkan, built
# for Android by android/gpu/reims/build.sh into android/gpu/reims/out. Without it the app has no reims.
set(REIMS_OUT ${CMAKE_CURRENT_SOURCE_DIR}/../reims/out)
if(EXISTS ${REIMS_OUT}/lib/libreims_vgpu.a)
    target_sources(hengegpu PRIVATE ../reims/host.c)
    target_include_directories(hengegpu PRIVATE ${REIMS_OUT}/include)
    target_compile_definitions(hengegpu PRIVATE HENGE_REIMS=1)
    target_link_libraries(hengegpu ${REIMS_OUT}/lib/libreims_vgpu.a dl)
    message(STATUS "hengegpu: with reims-vgpu")
endif()
