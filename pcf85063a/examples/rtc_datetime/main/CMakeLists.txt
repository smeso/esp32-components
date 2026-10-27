# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRCS
  "rtc_datetime.cpp"
  INCLUDE_DIRS
  "."
  REQUIRES
  pcf85063a
  esp_driver_gpio
  esp_driver_i2c
  freertos)
