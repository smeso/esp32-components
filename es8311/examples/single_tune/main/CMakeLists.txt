# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRCS
  "single_tune.cpp"
  INCLUDE_DIRS
  "."
  REQUIRES
  es8311
  esp_driver_gpio
  esp_driver_i2c
  esp_driver_i2s
  freertos)
