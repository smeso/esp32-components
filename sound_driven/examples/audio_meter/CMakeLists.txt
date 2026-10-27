# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

# The example lives inside the sound_driven component, so point the component
# manager at the component directory itself instead of at this project's
# (nonexistent) components/ directory. The sound_driven component itself
# depends on the sibling spiram_allocation component (its header includes
# spiram_allocation.hpp), on the sibling fft4r_tables component (its header
# includes fft4r_tables.hpp) and on the vendored esp-dsp component (its
# headers include dsps_fft4r.h), so those directories have to be on the
# component path as well. The example's own code does not use any of them
# directly.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../../spiram_allocation"
    "${CMAKE_CURRENT_LIST_DIR}/../../../fft4r_tables"
    "${CMAKE_CURRENT_LIST_DIR}/../../../esp-dsp")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(audio_meter)
