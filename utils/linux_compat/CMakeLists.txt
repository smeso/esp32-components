# SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

# Header-only stand-in for the esp_driver_i2s, esp_psram and
# esp_hw_support (esp_memory_utils.h) headers, which are not available on the
# FreeRTOS Linux simulator target. The I2S/PSRAM calls made by the codecs and
# the mocks are redirected to the mocks component at link time, so only the
# type and function declarations are needed to compile. Device builds use the
# real components and never see this one.
idf_build_get_property(_idf_target IDF_TARGET)
if(NOT _idf_target STREQUAL "linux")
  return()
endif()

idf_component_register(INCLUDE_DIRS "include")

# esp_hw_support is part of the common requirements even for the linux target,
# so its real esp_memory_utils.h is found before this component's stand-in and
# must compile as-is. That header references the SoC memory-range macros that
# the linux soc port does not define; provide them here (linux target only) so
# the inline esp_ptr_* helpers build. The values mirror a typical single-core
# SoC layout and never match host pointers, so every check reports false, which
# is what the mocks assume. Device builds use the real SoC headers and never
# see these definitions.
target_compile_definitions(${COMPONENT_LIB} INTERFACE
  SOC_IRAM_LOW=0x40000000
  SOC_IRAM_HIGH=0x40080000
  SOC_DRAM_LOW=0x3FFE0000
  SOC_DRAM_HIGH=0x40000000
  SOC_DIRAM_DRAM_LOW=0x3FFE0000
  SOC_DIRAM_DRAM_HIGH=0x40000000
  SOC_DIRAM_IRAM_LOW=0x40000000
  SOC_DIRAM_IRAM_HIGH=0x40080000
  SOC_DMA_LOW=0x3FFE0000
  SOC_DMA_HIGH=0x40080000
  SOC_MEM_INTERNAL_LOW=0x3FFE0000
  SOC_MEM_INTERNAL_HIGH=0x40080000
  SOC_DROM_LOW=0x3C000000
  SOC_DROM_HIGH=0x3C200000
  SOC_IROM_MASK_LOW=0x40000000
  SOC_IROM_MASK_HIGH=0x40080000
  SOC_RMT_MEM_WORDS_PER_CHANNEL=64)
