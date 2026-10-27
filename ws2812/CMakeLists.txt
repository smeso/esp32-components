# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_build_get_property(_idf_target IDF_TARGET)
if(_idf_target STREQUAL "linux")
  set(_driver_reqs linux_compat)
else()
  set(_driver_reqs esp_driver_rmt esp_driver_gpio)
endif()

idf_component_register(INCLUDE_DIRS "include" REQUIRES ${_driver_reqs})
