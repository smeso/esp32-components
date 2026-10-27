# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_build_get_property(_idf_target IDF_TARGET)
if(_idf_target STREQUAL "linux")
  # The esp_driver_i2c/esp_driver_i2s components are not usable for the
  # linux target; the i2c/i2s calls are mocked at link time, so only the
  # compat headers are needed to compile.
  set(_driver_reqs linux_compat)
else()
  set(_driver_reqs esp_driver_i2c esp_driver_i2s)
endif()

idf_component_register(INCLUDE_DIRS "include" PRIV_REQUIRES ${_driver_reqs})
