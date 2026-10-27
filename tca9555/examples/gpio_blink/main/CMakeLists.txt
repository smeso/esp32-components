# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRCS
  "gpio_blink.cpp"
  INCLUDE_DIRS
  "."
  REQUIRES
  tca9555
  esp_driver_gpio
  esp_driver_i2c
  freertos)
