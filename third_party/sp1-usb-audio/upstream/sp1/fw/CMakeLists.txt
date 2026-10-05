# Copyright (c) 2026 Ryan Gilmore
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1_usb_audio_example)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

foreach(f src/usb_audio.c src/usb_audio.h src/uacring.c src/uacring.h
          src/led.c src/led.h src/controls.c src/controls.h
          src/buttons.c src/buttons.h src/sp1_board.h)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
    message(FATAL_ERROR "${f} missing: build with sp1/prep.sh, which stages it")
  endif()
endforeach()

target_sources(app PRIVATE
  src/main.c src/audio.c src/power.c src/gesture.c src/wdt.c
  src/usb_audio.c src/uacring.c
  src/led.c src/controls.c src/buttons.c)

set_source_files_properties(src/uacring.c PROPERTIES
  COMPILE_DEFINITIONS "UACRING_BARRIER=barrier_dmem_fence_full"
  COMPILE_OPTIONS "-include;zephyr/sys/barrier.h")

include(${CMAKE_CURRENT_SOURCE_DIR}/isofast/CMakeLists.txt)

target_include_directories(app PRIVATE src)
