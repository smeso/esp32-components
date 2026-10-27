# SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

# Pull the esp_gcov component (and its esp_trace/app_trace dependencies) into
# the build so that the coverage runtime and its Kconfig options are available.
# COVERAGE_ENABLED is passed both as a cache variable (-D) and as an environment
# variable: the latter is what the early component-requirements expansion (a
# separate cmake -P process) can see.
#
# The Linux simulator target cannot build esp_gcov (its esp_trace/app_trace
# dependencies are not supported there): the linux coverage build
# (plumbing/run-linux-tests-coverage.sh) instruments with --coverage and dumps
# the data with the host libgcov's __gcov_dump() instead.
#
# The UART file I/O override in gcov_rtio_uart.c is used by the QEMU and the
# UART-on-device coverage builds: the plain esp_gcov file I/O (esp_apptrace_* in
# host_file_io.c) sends a command and then blocks waiting for a synchronous
# response, but the UART apptrace transport reads the RX FIFO only once without
# ever waiting for that response, so the plain implementation can never work
# over UART. The override replaces it with the fire-and-forget command stream
# that the coverage collectors (collect_gcov_qemu.py / gcov_boot.py) understand. When
# the apptrace destination is JTAG (CONFIG_APPTRACE_DEST_JTAG, the on-device
# coverage path for targets with a built-in USB-Serial/JTAG controller), the
# plain esp_gcov implementations are used as-is and OpenOCD answers their
# synchronous file I/O over JTAG.
idf_build_get_property(_idf_target IDF_TARGET)
if(COVERAGE_ENABLED OR DEFINED ENV{COVERAGE_ENABLED})
  if(_idf_target STREQUAL "linux")
    set(_priv_requires doctest)
    set(_gcov_srcs "")
  else()
    set(_priv_requires doctest esp_gcov)
    if(CONFIG_APPTRACE_DEST_UART)
      set(_gcov_srcs "gcov_rtio_uart.c")
    else()
      set(_gcov_srcs "")
    endif()
  endif()
else()
  set(_priv_requires doctest)
  set(_gcov_srcs "")
endif()

# Multidevice suites need more than one device, so by default the runner skips
# them all. Set MULTIDEVICE_SUITE to one of multidevice_a..multidevice_d to
# build a firmware that runs exactly that suite (and nothing else). Suites
# multidevice_a and multidevice_b pull in the multidevice_test component, which
# compiles the source file of the selected suite.
set(MULTIDEVICE_SUITE
    ""
    CACHE
      STRING
      "Multidevice doctest suite to run exclusively (multidevice_a..multidevice_d); empty = skip all"
)
set(_multidevice_requires "")
if(MULTIDEVICE_SUITE)
  if(NOT MULTIDEVICE_SUITE MATCHES "^multidevice_[abcd]$")
    message(
      FATAL_ERROR
        "MULTIDEVICE_SUITE must be one of multidevice_a..multidevice_d, got '${MULTIDEVICE_SUITE}'"
    )
  endif()
  list(APPEND _multidevice_requires "multidevice_test")
endif()

idf_component_register(SRCS "test_runner.cpp" ${_gcov_srcs} PRIV_REQUIRES
                       ${_priv_requires} ${_multidevice_requires})

if(MULTIDEVICE_SUITE)
  target_compile_definitions(${COMPONENT_LIB}
                             PRIVATE TEST_SUITE_INCLUDE="${MULTIDEVICE_SUITE}")
else()
  target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
      TEST_SUITE_EXCLUDE="multidevice_a,multidevice_b,multidevice_c,multidevice_d"
  )
endif()

# Compile the test runner with coverage instrumentation so that the test binary
# records coverage data for the linked test components. The Linux coverage
# build also needs the host libgcov's __gcov_dump() (test_runner.cpp) to flush
# the data, since the simulator process never exits on its own.
if(COVERAGE_ENABLED OR DEFINED ENV{COVERAGE_ENABLED})
  set_source_files_properties(test_runner.cpp PROPERTIES COMPILE_FLAGS --coverage)
  if(_idf_target STREQUAL "linux")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LINUX_GCOV_DUMP)
  endif()
endif()

# The UART dump path in gcov_rtio_uart.c replaces the esp_gcov component's JTAG
# implementations (esp_gcov/ is a vendored copy of upstream and must stay
# untouched). libgcov.a only references the gcov_rtio_* symbols, so --wrap
# redirects those references to our UART implementations. -u forces the
# definitions out of the main component's archive, since nothing else pulls them
# in (the wrapped references live in libgcov_rtio.a, linked later). The JTAG
# destination build (CONFIG_APPTRACE_DEST_JTAG) must NOT wrap: there the plain
# esp_gcov implementations are used and OpenOCD's `esp gcov` command drives the
# synchronous file I/O protocol over JTAG.
if(COVERAGE_ENABLED OR DEFINED ENV{COVERAGE_ENABLED})
  if(CONFIG_APPTRACE_DEST_UART)
    target_link_options(
      ${COMPONENT_LIB}
      INTERFACE
      "-Wl,--wrap=gcov_rtio_fopen"
      "-Wl,--wrap=gcov_rtio_fclose"
      "-Wl,--wrap=gcov_rtio_fwrite"
      "-Wl,--wrap=gcov_rtio_fread"
      "-Wl,--wrap=gcov_rtio_fseek"
      "-Wl,--wrap=gcov_rtio_ftell"
      "-Wl,--wrap=gcov_rtio_feof"
      "-Wl,-u,__wrap_gcov_rtio_fopen"
      "-Wl,-u,__wrap_gcov_rtio_fclose"
      "-Wl,-u,__wrap_gcov_rtio_fwrite"
      "-Wl,-u,__wrap_gcov_rtio_fread"
      "-Wl,-u,__wrap_gcov_rtio_fseek"
      "-Wl,-u,__wrap_gcov_rtio_ftell"
      "-Wl,-u,__wrap_gcov_rtio_feof")
  endif()
endif()
