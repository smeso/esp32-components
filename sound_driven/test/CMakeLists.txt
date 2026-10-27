# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRC_DIRS
  "."
  INCLUDE_DIRS
  "."
  REQUIRES
  doctest
  sound_driven
  fft4r_tables
  mocks
  spiram_allocation
  esp-dsp
  esp_system
  freertos)
