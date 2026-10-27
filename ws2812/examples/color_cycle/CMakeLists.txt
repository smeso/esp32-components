# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

# The example lives inside the ws2812 component, so point the component
# manager at the component directory itself instead of at this project's
# (nonexistent) components/ directory. The ws2812 component itself depends
# on esp_driver_rmt and esp_driver_gpio, so no other component directory has
# to be on the path.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(color_cycle)
