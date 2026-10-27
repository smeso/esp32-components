# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_build_get_property(_idf_target IDF_TARGET)
if(_idf_target STREQUAL "linux")
  # The esp_psram component is not usable for the linux target; the PSRAM
  # calls made by the allocator and the mocks are redirected to the mocks
  # component at link time, so only the compat headers are needed to compile.
  set(_psram_req linux_compat)
else()
  set(_psram_req esp_psram)
endif()

idf_component_register(INCLUDE_DIRS "include" PRIV_REQUIRES ${_psram_req})
