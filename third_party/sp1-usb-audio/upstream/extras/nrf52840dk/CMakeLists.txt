# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1_usb_audio_sample)

# This sample has no watchdog feed, no power-off and no LEDs. Flashed on an
# SP-1 it would reset every 5 s (the bootloader's watchdog) with no way to turn
# the unit off. See README.md, "Running on an SP-1".
if(BOARD STREQUAL "sp1")
  message(FATAL_ERROR "sample: not for the sp1 board as it stands -- the SP-1 firmware is ../../sp1/")
endif()

# sample_usbd: Zephyr's device_next helper that builds the USB device from the
# classes enabled in devicetree and Kconfig.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# The shared USB audio code: the glue and the packet regulator.
set(USB_AUDIO ${CMAKE_CURRENT_SOURCE_DIR}/../../usb-audio)
target_sources(app PRIVATE src/main.c ${USB_AUDIO}/usb_audio.c ${USB_AUDIO}/uacring.c)
target_include_directories(app PRIVATE ${USB_AUDIO})

# The ring is single-producer single-consumer across a thread and the USB
# stack (or, with the fast path, the USBD interrupt): full barriers on target.
set_source_files_properties(${USB_AUDIO}/uacring.c PROPERTIES
  COMPILE_DEFINITIONS "UACRING_BARRIER=barrier_dmem_fence_full"
  COMPILE_OPTIONS "-include;zephyr/sys/barrier.h")

# Optional: the ISO IN fast path, in place of Zephyr's nRF USBD driver.
# Build with -DEXTRA_CONF_FILE=isofast.conf.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../module/nrf-usbd-isofast/CMakeLists.txt)
