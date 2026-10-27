# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

# The example lives inside the tca9555 component, so point the component
# manager at the component directory itself instead of at this project's
# (nonexistent) components/ directory. The tca9555 component itself depends
# on the sibling task_queue component (its header includes task_queue.hpp),
# which in turn depends on the sibling cpptaskcreate component, so those
# directories have to be on the component path as well.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../../task_queue"
    "${CMAKE_CURRENT_LIST_DIR}/../../../cpptaskcreate")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gpio_blink)
