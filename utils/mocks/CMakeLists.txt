# SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
# SPDX-License-Identifier: Apache-2.0

idf_build_get_property(_idf_target IDF_TARGET)
if(_idf_target STREQUAL "linux")
  # The esp_driver_* and esp_psram components are not usable for the linux
  # target. The mocked driver entry points are linked via --wrap, so only
  # their declarations are needed; linux_compat provides them.
  set(_driver_reqs linux_compat heap)
else()
  set(_driver_reqs esp_driver_gpio esp_driver_i2c esp_driver_i2s
                   esp_driver_rmt esp_psram heap)
endif()

# mock_timers.cpp wraps the FreeRTOS timer API (xTimerCreate /
# xTimerGenericCommand) and delegates to the real kernel functions through
# __real_* symbols, which only exist when the --wrap options below are active.
# It is therefore only compiled for the linux target; device builds use the
# real FreeRTOS timers unmodified.
set(_mock_sources
    mock_bus.cpp
    mock_gpio.cpp
    mock_heap.cpp
    mock_hooks.cpp
    mock_rmt.cpp
    mock_time.cpp)
if(_idf_target STREQUAL "linux")
  list(APPEND _mock_sources mock_timers.cpp)
endif()

idf_component_register(
  SRCS
  ${_mock_sources}
  INCLUDE_DIRS
  "."
  REQUIRES
  ${_driver_reqs})

if(_idf_target STREQUAL "linux")
  # esp_system does not build freertos_hooks.c for the linux target, so the
  # tick-hook API used by the task_queue ISR test is backed by mock_hooks.cpp:
  # esp_register/deregister_freertos_tick_hook are --wrap-ed to the mock, and
  # vApplicationTickHook() (called by the FreeRTOS kernel from
  # xTaskIncrementTick() when CONFIG_FREERTOS_USE_TICK_HOOK is enabled, see
  # run-linux-tests.sh) is --wrap-ed to dispatch to the registered callbacks.
  target_link_options(
    ${COMPONENT_LIB}
    INTERFACE
    "-Wl,--wrap=vApplicationTickHook"
    "-Wl,--wrap=esp_register_freertos_tick_hook"
    "-Wl,--wrap=esp_deregister_freertos_tick_hook"
    "-Wl,--wrap=xPortCheckIfInISR"
    "-Wl,--wrap=xTimerCreate"
    "-Wl,--wrap=xTimerGenericCommand"
    "-Wl,--wrap=xTaskCreatePinnedToCore"
    "-Wl,--wrap=xQueueGenericCreate"
    "-Wl,-u,__wrap_vApplicationTickHook"
    "-Wl,-u,__wrap_esp_register_freertos_tick_hook"
    "-Wl,-u,__wrap_esp_deregister_freertos_tick_hook"
    "-Wl,-u,__wrap_xPortCheckIfInISR"
    "-Wl,-u,__wrap_xTimerCreate"
    "-Wl,-u,__wrap_xTimerGenericCommand"
    "-Wl,-u,__wrap_xTaskCreatePinnedToCore"
    "-Wl,-u,__wrap_xQueueGenericCreate")
endif()

# Mock the ESP-IDF I2C/I2S/GPIO/RMT/heap driver calls, the libc time entry
# points and abort() so tests can exercise drivers that talk to those buses,
# pins or peripherals, that allocate from the SPI RAM heap, that read/set the
# system time, or that reach an abort()-based error path (the __wrap_abort
# hook makes the abort recoverable for tests) without real hardware. The test
# component that pulls this
# component in is linked into the firmware before the real driver libraries
# (see the --whole-archive block in utils/test_app/CMakeLists.txt), so --wrap
# redirects every reference to the __wrap_* implementations in mock_bus.cpp /
# mock_gpio.cpp / mock_heap.cpp / mock_rmt.cpp / mock_time.cpp.
# -u forces the definitions out of the test component's archive.
target_link_options(
  ${COMPONENT_LIB}
  INTERFACE
  "-Wl,--wrap=i2c_master_bus_add_device"
  "-Wl,--wrap=i2c_master_bus_rm_device"
  "-Wl,--wrap=i2c_master_transmit"
  "-Wl,--wrap=i2c_master_transmit_receive"
  "-Wl,--wrap=i2s_channel_write"
  "-Wl,--wrap=i2s_channel_read"
  "-Wl,--wrap=gpio_config"
  "-Wl,--wrap=gpio_get_level"
  "-Wl,--wrap=gpio_install_isr_service"
  "-Wl,--wrap=gpio_isr_handler_add"
  "-Wl,--wrap=gpio_isr_handler_remove"
  "-Wl,--wrap=gpio_reset_pin"
  "-Wl,--wrap=rmt_new_tx_channel"
  "-Wl,--wrap=rmt_new_bytes_encoder"
  "-Wl,--wrap=rmt_enable"
  "-Wl,--wrap=rmt_disable"
  "-Wl,--wrap=rmt_del_encoder"
  "-Wl,--wrap=rmt_del_channel"
  "-Wl,--wrap=rmt_transmit"
  "-Wl,--wrap=rmt_tx_wait_all_done"
  "-Wl,--wrap=heap_caps_aligned_alloc"
  "-Wl,--wrap=heap_caps_free"
  "-Wl,--wrap=esp_ptr_external_ram"
  "-Wl,--wrap=gettimeofday"
  "-Wl,--wrap=settimeofday"
  "-Wl,--wrap=adjtime"
  "-Wl,--wrap=abort"
  "-Wl,-u,__wrap_i2c_master_bus_add_device"
  "-Wl,-u,__wrap_i2c_master_bus_rm_device"
  "-Wl,-u,__wrap_i2c_master_transmit"
  "-Wl,-u,__wrap_i2c_master_transmit_receive"
  "-Wl,-u,__wrap_i2s_channel_write"
  "-Wl,-u,__wrap_i2s_channel_read"
  "-Wl,-u,__wrap_gpio_config"
  "-Wl,-u,__wrap_gpio_get_level"
  "-Wl,-u,__wrap_gpio_install_isr_service"
  "-Wl,-u,__wrap_gpio_isr_handler_add"
  "-Wl,-u,__wrap_gpio_isr_handler_remove"
  "-Wl,-u,__wrap_gpio_reset_pin"
  "-Wl,-u,__wrap_rmt_new_tx_channel"
  "-Wl,-u,__wrap_rmt_new_bytes_encoder"
  "-Wl,-u,__wrap_rmt_enable"
  "-Wl,-u,__wrap_rmt_disable"
  "-Wl,-u,__wrap_rmt_del_encoder"
  "-Wl,-u,__wrap_rmt_del_channel"
  "-Wl,-u,__wrap_rmt_transmit"
  "-Wl,-u,__wrap_rmt_tx_wait_all_done"
  "-Wl,-u,__wrap_heap_caps_aligned_alloc"
  "-Wl,-u,__wrap_heap_caps_free"
  "-Wl,-u,__wrap_esp_ptr_external_ram"
  "-Wl,-u,__wrap_gettimeofday"
  "-Wl,-u,__wrap_settimeofday"
  "-Wl,-u,__wrap_adjtime"
  "-Wl,-u,__wrap_abort")
