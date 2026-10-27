# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

# The example lives inside the task_queue component, so point the component
# manager at the component directory itself instead of at this project's
# (nonexistent) components/ directory. task_queue depends on the sibling
# cpptaskcreate component (task_queue.hpp includes cpptaskcreate.hpp), so its
# directory has to be on the component path as well.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../../cpptaskcreate")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(worker_pool)
