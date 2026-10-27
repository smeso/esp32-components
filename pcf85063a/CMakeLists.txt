# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_build_get_property(_idf_target IDF_TARGET)
if(_idf_target STREQUAL "linux")
  set(_i2c_req linux_compat)
else()
  set(_i2c_req esp_driver_i2c)
endif()

idf_component_register(INCLUDE_DIRS "include" PRIV_REQUIRES ${_i2c_req})
