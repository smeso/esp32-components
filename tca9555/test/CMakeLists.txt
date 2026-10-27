# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRC_DIRS
  "."
  INCLUDE_DIRS
  "."
  REQUIRES
  doctest
  tca9555
  mocks
  task_queue
  esp_system
  freertos)
