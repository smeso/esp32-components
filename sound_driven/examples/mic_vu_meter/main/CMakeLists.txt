# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRCS
  "mic_vu_meter.cpp"
  INCLUDE_DIRS
  "."
  REQUIRES
  sound_driven
  es7210
  ws2812
  leds_effects
  esp_driver_gpio
  esp_driver_i2c
  esp_driver_i2s
  freertos)
