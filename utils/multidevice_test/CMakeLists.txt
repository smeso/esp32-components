# SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

# Two-board rig tests. The component is only linked into the firmware when
# MULTIDEVICE_SUITE selects one of the multidevice doctest suites, and only the
# source file of that suite is compiled so each board gets its own firmware:
#   -DMULTIDEVICE_SUITE=multidevice_a   -> leader   (multidevice_a.cpp)
#   -DMULTIDEVICE_SUITE=multidevice_b   -> follower (multidevice_b.cpp)
set(MULTIDEVICE_SUITE
    ""
    CACHE STRING "Multidevice doctest suite to build (multidevice_a or multidevice_b)")

set(_srcs "")
if(MULTIDEVICE_SUITE)
  if(NOT MULTIDEVICE_SUITE MATCHES "^multidevice_[ab]$")
    message(FATAL_ERROR "Multidevice tests only implement multidevice_a and multidevice_b, got '${MULTIDEVICE_SUITE}'")
  endif()
  set(_srcs "${MULTIDEVICE_SUITE}.cpp")
endif()

idf_component_register(SRCS ${_srcs} INCLUDE_DIRS "include" REQUIRES ws2812
                       PRIV_REQUIRES doctest esp_driver_uart esp_driver_gpio
                                     esp_driver_spi esp_driver_i2s esp_driver_rmt
                                     esp_driver_ledc esp_driver_i2c)

if(MULTIDEVICE_SUITE STREQUAL "multidevice_a")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE MD_ROLE_LEADER)
elseif(MULTIDEVICE_SUITE STREQUAL "multidevice_b")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE MD_ROLE_FOLLOWER)
endif()
