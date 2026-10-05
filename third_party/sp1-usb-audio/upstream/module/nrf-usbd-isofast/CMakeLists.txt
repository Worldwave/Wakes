# Out-of-tree build of the patched nRF USBD driver. Include from an application
# CMakeLists.txt after find_package(Zephyr):
#   include(<path>/nrf-usbd-isofast/CMakeLists.txt)
# The driver needs Zephyr's private UDC and nrf_usbd_common headers.
if(CONFIG_UDC_NRF_ISOFAST)
  target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/udc_nrf.c)
  target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${ZEPHYR_BASE}/drivers/usb/udc
    ${ZEPHYR_BASE}/drivers/usb/common/nrf_usbd_common)
endif()
