# SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

# Header-only doctest library. It is exposed as an interface component so that
# test components can pull it in with REQUIRES doctest instead of relying on
# include-path tricks. The header lives at components/doctest/doctest.h, so the
# parent directory is exposed as the include path to allow
# #include <doctest/doctest.h>.
idf_component_register(INCLUDE_DIRS ".")

# doctest's default assertion decomposition expands every CHECK() into a large
# amount of inline code. Under --coverage those branches become per-function
# gcov counters in each test translation unit, and the largest suites generate
# ~80 KiB of counter .bss. That shrinks the internal 8-bit DRAM heap on the
# chip/QEMU coverage builds below what the pre-scheduler allocations (apptrace
# buffers, main/IPC task stacks, the idle and timer task memory) and doctest's
# static test-case registration need, so the app aborts in
# vApplicationGetIdleTaskMemory() before app_main() runs and no .gcda data is
# ever dumped. Super-fast asserts call out to doctest instead of inlining the
# decomposition, cutting the counters by ~45 KiB so the coverage build boots.
# The reported coverage is unaffected: the test translation units' own counters
# are excluded from the report (only the components under test are measured),
# and the tests keep evaluating the same expressions. Inheritance is applied
# through this interface component so that every test component picks it up;
# the components under test do not depend on doctest, so their own coverage
# counters stay untouched.
target_compile_definitions(${COMPONENT_LIB} INTERFACE
                           DOCTEST_CONFIG_SUPER_FAST_ASSERTS)
