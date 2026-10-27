# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRC_DIRS
  "."
  INCLUDE_DIRS
  "."
  REQUIRES
  doctest
  ws2812
  spiram_allocation
  mocks
  esp_system
  freertos)
