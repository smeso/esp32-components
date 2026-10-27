# SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRCS
  "counter_task.cpp"
  INCLUDE_DIRS
  "."
  REQUIRES
  cpptaskcreate
  freertos)
