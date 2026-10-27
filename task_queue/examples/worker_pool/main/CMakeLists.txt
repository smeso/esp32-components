# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRCS
  "worker_pool.cpp"
  INCLUDE_DIRS
  "."
  REQUIRES
  task_queue
  freertos)
