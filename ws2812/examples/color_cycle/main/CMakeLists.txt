# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRCS
  "color_cycle.cpp"
  INCLUDE_DIRS
  "."
  REQUIRES
  ws2812
  esp_driver_gpio
  freertos)
