cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 23)

# The linux target cannot build the esp_gcov runtime: its esp_trace/app_trace
# dependencies are not supported by the FreeRTOS Linux simulator. The linux
# coverage build (plumbing/run-linux-tests-coverage.sh) instruments the code
# with --coverage instead and dumps the data with the host libgcov, so the
# esp_gcov component is only added for the chip targets.
if(DEFINED IDF_TARGET)
  set(_idf_target "${IDF_TARGET}")
else()
  set(_idf_target "$ENV{IDF_TARGET}")
endif()

# Coverage is enabled with -DCOVERAGE_ENABLED=1 (and COVERAGE_ENABLED=1 in the
# environment, which is what the early component-requirements expansion can
# see). The esp_gcov component (used by plumbing/run-qemu-tests-coverage.sh) requires
# ESP-IDF 6.0 or newer (see its idf_component.yml). It is added to the build
# only when coverage is enabled so that plain test runs stay independent of it.
if((COVERAGE_ENABLED OR DEFINED ENV{COVERAGE_ENABLED})
   AND NOT EXISTS "$ENV{IDF_PATH}/components/esp_trace/Kconfig")
  message(FATAL_ERROR "test_app coverage requires ESP-IDF 6.0 or newer")
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../.."
                         "${CMAKE_CURRENT_LIST_DIR}/../mocks"
                         "${CMAKE_CURRENT_LIST_DIR}/../linux_compat"
                         "${CMAKE_CURRENT_LIST_DIR}/../multidevice_test")

if((COVERAGE_ENABLED OR DEFINED ENV{COVERAGE_ENABLED})
   AND NOT _idf_target STREQUAL "linux")
  list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../esp_gcov")
endif()

if(NOT "$ENV{IDF_TOOLCHAIN}" STREQUAL "clang")
  # Too many false positives
  add_compile_options(-Wno-analyzer-use-of-uninitialized-value
                      -Wno-analyzer-null-dereference
                      -Wno-analyzer-malloc-leak
                      -Wno-analyzer-out-of-bounds
                      -Wno-analyzer-null-argument)
endif()

# Components whose tests are linked into the firmware. The ESP-IDF build system
# needs TEST_COMPONENTS (the components whose tests are linked into the
# firmware) while TESTS_ALL only enables the test-component machinery for the
# components already in the build.
#
# By default every component that ships a test/ subdirectory is selected. To run
# only a subset, pass the list on the command line (space- or
# semicolon-separated), e.g.:
#
# idf.py -DTEST_COMPONENTS=es8311 build
#
# An empty value restores the default of testing everything, except when a
# multidevice suite is selected: that firmware only contains the rig tests, so
# force the multidevice_test component into the minimal build (its sources are
# the ones selected by MULTIDEVICE_SUITE) instead of linking every component
# test.
set(TEST_COMPONENTS
    ""
    CACHE STRING "Components to test (empty = all components with a test/ dir)")
if(NOT TEST_COMPONENTS)
  if(MULTIDEVICE_SUITE)
    set(TEST_COMPONENTS multidevice_test)
  else()
    file(GLOB _components "${CMAKE_CURRENT_LIST_DIR}/../../*")
    foreach(_component_dir ${_components})
      if(IS_DIRECTORY "${_component_dir}"
         AND EXISTS "${_component_dir}/test"
         AND EXISTS "${_component_dir}/CMakeLists.txt")
        get_filename_component(_component_name "${_component_dir}" NAME)
        if(NOT "${_component_name}" STREQUAL "esp-dsp"
           AND NOT "${_component_name}" STREQUAL "esp-mqtt"
           AND NOT "${_component_name}" STREQUAL "esp_gcov")
          list(APPEND TEST_COMPONENTS "${_component_name}")
        endif()
      endif()
    endforeach()
    unset(_components)
    unset(_component_dir)
    unset(_component_name)
  endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
idf_build_set_property(EXTRA_COMPONENT_EXCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")
idf_build_replace_option_from_property(COMPILE_OPTIONS "-Wno-error=extra" "")
# idf_build_set_property(COMPILE_OPTIONS "-fsanitize=undefined,address,leak" "-fno-sanitize=shift-base" APPEND)

project(test_app VERSION 1.0.0)

# The vendored esp-dsp C sources trip many -fanalyzer false positives (the
# analyzer cannot prove that malloc()ed pointers are non-NULL etc.) once the
# test app links the component (e.g. via sound_driven). The analyzer is only
# meant for this repository's own code, so turn it off for the vendored
# component.
if(NOT "$ENV{IDF_TOOLCHAIN}" STREQUAL "clang" AND TARGET __idf_esp-dsp)
  target_compile_options(__idf_esp-dsp PRIVATE -fno-analyzer)
endif()

# Instrument the components under test (and their test code) so that gcov
# coverage data is recorded. The esp_gcov component provides the runtime and the
# UART transport used to dump the resulting .gcda files. Targets are real
# (static) libraries at this point because project() has already run.
if(COVERAGE_ENABLED OR DEFINED ENV{COVERAGE_ENABLED})
  # TEST_COMPONENTS arrives as a plain string (e.g. "a b"), which a CMake
  # list context would treat as a single element: split it first.
  set(_test_components "${TEST_COMPONENTS}")
  string(REPLACE " " ";" _test_components "${_test_components}")
  foreach(_component IN LISTS _test_components)
    foreach(_target "__idf_${_component}" "__${_component}_test")
      if(TARGET "${_target}")
        get_target_property(_type "${_target}" TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
          target_compile_options("${_target}" PRIVATE --coverage)
        endif()
      endif()
    endforeach()
  endforeach()
  unset(_test_components)
  unset(_component)
  unset(_target)
  unset(_type)

  # The Linux simulator links the instrumented objects against the host
  # libgcov (--coverage implies it) and test_runner.cpp flushes the data with
  # __gcov_dump() after the suites. The chip targets get the runtime from the
  # esp_gcov component instead.
  if(IDF_TARGET STREQUAL "linux")
    target_link_options(test_app.elf PRIVATE --coverage)
  endif()
endif()

# Optional sanitizer builds, selected with SANITIZERS (1-4) by
# plumbing/run-{qemu,device,linux}-tests.sh:
#   1 - UBSan in a Debug build
#   2 - UBSan in a regular build
#   3 - ThreadSanitizer in a Debug build (linux target only)
#   4 - ThreadSanitizer in a regular build (linux target only)
# The linux UBSan builds (1 and 2) also enable AddressSanitizer and
# LeakSanitizer; the linux builds link against the host sanitizer runtimes.
# The chip targets have no compiler-rt for the address/thread/leak sanitizers.
# GCC emits a UBSan descriptor for every check site as writable .data (the
# esp_system handlers take a non-const pointer), and a test translation unit
# with heavy template expansion generates over a hundred KiB of them. That
# fills the internal DRAM static region, so the pre-scheduler allocations fail
# in vApplicationGetIdleTaskMemory() before app_main() runs. Use
# -fsanitize-undefined-trap-on-error instead: it turns each check into a trap
# instruction with no descriptor and no runtime, so nothing is put in .data.
# Every check is still detected (the chip aborts either way) and clang
# supports the same flag. The C++ vptr check stays disabled, as it has always
# been on the chip targets, to keep this fix orthogonal to the check set.
set(_sanitizers "$ENV{SANITIZERS}")
if(NOT _sanitizers AND DEFINED SANITIZERS)
  set(_sanitizers "${SANITIZERS}")
endif()
if(_sanitizers)
  if(NOT _sanitizers MATCHES "^[1-4]$")
    message(FATAL_ERROR "SANITIZERS must be 1, 2, 3 or 4, got '${_sanitizers}'")
  endif()

  if(_sanitizers STREQUAL "1" OR _sanitizers STREQUAL "2")
    set(_san_mode "ubsan")
  else()
    set(_san_mode "tsan")
  endif()

  if(IDF_TARGET STREQUAL "linux")
    if(_san_mode STREQUAL "ubsan")
      set(_san_flags "-fsanitize=address,undefined,leak"
                     "-fno-sanitize-recover=all")
    else()
      set(_san_flags "-fsanitize=thread")
    endif()
  else()
    if(_san_mode STREQUAL "tsan")
      message(FATAL_ERROR "SANITIZERS=${_sanitizers} (thread sanitizer) is only supported on the linux target")
    endif()
    set(_san_flags "-fsanitize=undefined"
                   "-fsanitize-undefined-trap-on-error"
                   "-fno-sanitize=vptr"
                   "-fno-sanitize=shift-base")
  endif()

  # Instrument only the components under test (and their test code), like the
  # coverage block above; the rest of the firmware stays uninstrumented.
  set(_test_components "${TEST_COMPONENTS}")
  string(REPLACE " " ";" _test_components "${_test_components}")
  foreach(_component IN LISTS _test_components)
    foreach(_target "__idf_${_component}" "__${_component}_test")
      if(TARGET "${_target}")
        get_target_property(_type "${_target}" TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
          target_compile_options("${_target}" PRIVATE ${_san_flags})
        endif()
      endif()
    endforeach()
  endforeach()
  unset(_test_components)
  unset(_component)
  unset(_target)
  unset(_type)

  # The linux simulator links against the host sanitizer runtime. The chip
  # builds need no link flags: -fsanitize-undefined-trap-on-error lowers every
  # check to an inline trap, so no UBSan handler symbols are referenced.
  if(IDF_TARGET STREQUAL "linux")
    target_link_options(test_app.elf PRIVATE ${_san_flags})
  endif()
  unset(_san_flags)
  unset(_san_mode)
endif()
unset(_sanitizers)
