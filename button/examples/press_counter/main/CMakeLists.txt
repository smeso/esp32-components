# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRCS
  "press_counter.cpp"
  INCLUDE_DIRS
  "."
  REQUIRES
  button
  esp_driver_gpio
  freertos)
