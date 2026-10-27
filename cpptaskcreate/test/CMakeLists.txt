# SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
  SRC_DIRS
  "."
  INCLUDE_DIRS
  "."
  REQUIRES
  doctest
  cpptaskcreate
  mocks
  esp_system
  freertos)

# task_entry() ends with abort() on both of its error paths (normal return
# and uncaught exception). The mocks component --wrap redirects every
# abort() in the firmware to __wrap_abort(), which consults the hook
# installed by test_task_entry(): the hook suspends the aborting task
# (vTaskSuspend(NULL)), so the two error paths are exercised without ending
# the test process. vTaskDelete(NULL) would be unsafe from inside the abort
# machinery on the linux port, and a hook that deletes the parked task after
# the fact is equally unnecessary: the context is released before abort() and
# its thread local storage deletion callback is unregistered at the same
# point. -u forces the __wrap_abort definition out of the mocks archive
# (see the --whole-archive block in utils/test_app/CMakeLists.txt).
target_link_options(
  ${COMPONENT_LIB}
  INTERFACE
  "-Wl,-u,__wrap_abort")
