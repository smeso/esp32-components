# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_build_get_property(_idf_target IDF_TARGET)
if(_idf_target STREQUAL "linux")
  # The esp_psram component is not usable for the linux target; the PSRAM
  # calls are redirected to the mocks at link time, so only the compat
  # headers are needed to compile.
  set(_psram_req linux_compat)
else()
  set(_psram_req esp_psram)
endif()

idf_component_register(
  SRC_DIRS
  "."
  INCLUDE_DIRS
  "."
  REQUIRES
  doctest
  spiram_allocation
  ${_psram_req}
  mocks
  freertos)

# Mock heap_caps_aligned_alloc()/heap_caps_free() (provided by the shared
# mocks component) so the allocator tests can run on targets without a SPI RAM
# heap. The mocks component's --wrap redirects every reference to the
# __wrap_* implementations in its mock_heap.cpp.
target_link_options(
  ${COMPONENT_LIB}
  INTERFACE
  "-Wl,-u,__wrap_heap_caps_aligned_alloc"
  "-Wl,-u,__wrap_heap_caps_free")
