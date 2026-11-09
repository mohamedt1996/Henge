# SPDX-License-Identifier: GPL-2.0-or-later
# libhengegpu.so, the app's renderer (android/gpu/replay). Built by Gradle's externalNativeBuild.
cmake_minimum_required(VERSION 3.22)
project(hengegpu C)
set(CMAKE_C_STANDARD 11)
add_library(hengegpu SHARED guest.c objects.c pixels.c screen_vk.c shaders.c draw.c replay.c jni.c)
target_compile_options(hengegpu PRIVATE -O2 -Wall -Wno-unused-parameter -D_GNU_SOURCE)
target_link_libraries(hengegpu vulkan android log m)
