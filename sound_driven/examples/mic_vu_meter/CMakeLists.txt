# SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

# The example lives inside the sound_driven component, so point the component
# manager at the component directory itself instead of at this project's
# (nonexistent) components/ directory. The example directly uses the es7210,
# ws2812 and leds_effects components too, and sound_driven itself depends on
# the sibling spiram_allocation component (its header includes
# spiram_allocation.hpp), on the sibling fft4r_tables component (its header
# includes fft4r_tables.hpp) and on the vendored esp-dsp component (its
# headers include dsps_fft4r.h), so those directories have to be on the
# component path as well. leds_effects in turn depends on the sibling
# cpptaskcreate component, so that directory is needed too.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../../es7210"
    "${CMAKE_CURRENT_LIST_DIR}/../../../ws2812"
    "${CMAKE_CURRENT_LIST_DIR}/../../../leds_effects"
    "${CMAKE_CURRENT_LIST_DIR}/../../../cpptaskcreate"
    "${CMAKE_CURRENT_LIST_DIR}/../../../spiram_allocation"
    "${CMAKE_CURRENT_LIST_DIR}/../../../fft4r_tables"
    "${CMAKE_CURRENT_LIST_DIR}/../../../esp-dsp")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mic_vu_meter)
