/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstring>
#include <esp_log.h>

#include <doctest/doctest_config.h>

// On the two-board rig the selected suite's component provides this hook; it
// drives the on-board WS2812 status LED (0 = booting, 1 = running, 2 = ok,
// 3 = failed). Only declared here to avoid depending on the component's
// include directory (the regular single-device build does not link it).
#ifdef TEST_SUITE_INCLUDE
extern "C" void multidevice_set_status(int status);
#endif

#ifdef CONFIG_ESP_GCOV_ENABLE
#include <esp_app_trace.h>
#include <soc/uart_pins.h>

// esp_gcov.h is a C header without extern "C" guards; declare the function
// directly so the C++ caller links against the C symbol.
extern "C" void esp_gcov_dump(void);

// Select the apptrace UART when the coverage build dumps over UART
// (CONFIG_APPTRACE_DEST_UART). Two configurations share this override:
//   - The QEMU build (sdkconfig.defaults.coverage.uart) keeps the console on
//     UART0 and puts the apptrace channel on UART1 with GPIO 4/5 (the
//     apptrace Kconfig defaults, GPIO 12/13, are the SPI flash HD/WP pins on
//     ESP32-C3/C2 and get reserved before the apptrace initializes). Taking
//     the values from Kconfig keeps the override in sync with the sdkconfig.
//   - The single-UART device build (sdkconfig.defaults.coverage.uart.device)
//     disables the console (CONFIG_ESP_CONSOLE_NONE) so the apptrace channel
//     can take the main UART (UART0), the only one wired to the host; use its
//     default pins.
// When the destination is JTAG (the on-device coverage path for built-in
// USB-Serial/JTAG controllers) this override is not compiled and the weak
// default (APPTRACE_JTAG_CONFIG_DEFAULT) is used instead. Must be C linkage
// to override the weak C symbol in app_trace.c.
#if CONFIG_APPTRACE_DEST_UART
extern "C" esp_apptrace_config_t
esp_apptrace_get_user_params(void)
{
    esp_apptrace_config_t config = {};
    config.dest = ESP_APPTRACE_DEST_UART;
#if CONFIG_ESP_CONSOLE_NONE
    config.dest_cfg.uart.uart_num = 0;
    config.dest_cfg.uart.tx_pin_num = U0TXD_GPIO_NUM;
    config.dest_cfg.uart.rx_pin_num = U0RXD_GPIO_NUM;
#else
    config.dest_cfg.uart.uart_num = CONFIG_APPTRACE_DEST_UART_NUM;
    config.dest_cfg.uart.tx_pin_num = CONFIG_APPTRACE_UART_TX_GPIO;
    config.dest_cfg.uart.rx_pin_num = CONFIG_APPTRACE_UART_RX_GPIO;
#endif
    config.dest_cfg.uart.baud_rate = CONFIG_APPTRACE_UART_BAUDRATE;
    config.dest_cfg.uart.tx_buff_size = CONFIG_APPTRACE_UART_TX_BUFF_SIZE;
    config.dest_cfg.uart.tx_msg_size = CONFIG_APPTRACE_UART_TX_MSG_SIZE;
    config.flush_tmo = CONFIG_APPTRACE_ONPANIC_HOST_FLUSH_TMO;
    config.flush_thresh = CONFIG_APPTRACE_POSTMORTEM_FLUSH_THRESH;
    return config;
}

// In the single-UART device build the console is disabled, so the status
// markers that the host-side collector (collect_gcov_uart.py) looks for are
// sent through the apptrace channel itself; the collector sees them in the
// same byte stream as the dump.
#if CONFIG_ESP_CONSOLE_NONE
static void
apptrace_puts(const char *str)
{
    esp_apptrace_write(str, strlen(str), ESP_APPTRACE_TMO_INFINITE);
    esp_apptrace_flush(ESP_APPTRACE_TMO_INFINITE);
}
#endif   // CONFIG_ESP_CONSOLE_NONE
#endif   // CONFIG_APPTRACE_DEST_UART
#elif LINUX_GCOV_DUMP
// The FreeRTOS Linux simulator keeps the scheduler running after app_main()
// returns, so the process never exits and libgcov's atexit handler never runs.
// The linux coverage build (plumbing/run-linux-tests-coverage.sh) flushes the
// .gcda data explicitly after the doctest suites finish.
extern "C" void __gcov_dump(void);
#endif

static constexpr const char *TAG = "test_app";

extern "C" void
app_main(void)
{
    // On the two-board rig the on-board WS2812 reports the run outcome: blue
    // while booting, yellow while the suite runs, green on success and red on
    // failure. The regular (single-device) test app has no such LED.
#ifdef TEST_SUITE_INCLUDE
    multidevice_set_status(0);   // blue: booting
#endif
    ESP_LOGI(TAG, "running doctest suites");
    doctest::Context ctx;
    ctx.setOption("no-breaks", true);
    ctx.setOption("success", false);
    ctx.setOption("order-by", "file");
    ctx.setOption("no-colors", true);
    // By default all multidevice suites are skipped (the regular test harness
    // runs on a single device). When MULTIDEVICE_SUITE is set at configure
    // time, exactly that suite is built in and run exclusively.
#ifdef TEST_SUITE_INCLUDE
    ctx.setOption("test-suite", TEST_SUITE_INCLUDE);
    multidevice_set_status(1);   // yellow: running
#else
    ctx.setOption("test-suite-exclude", TEST_SUITE_EXCLUDE);
#endif
    [[maybe_unused]] const int doctest_result = ctx.run();
#ifdef TEST_SUITE_INCLUDE
    multidevice_set_status(doctest_result == 0 ? 2 : 3);   // green: ok / red: failed
#endif

#ifdef CONFIG_ESP_GCOV_ENABLE
    // On the JTAG coverage path the app must be blocked in esp_gcov_dump()
    // before OpenOCD attaches: `esp gcov dump` only waits 3s for the dump
    // data, so it must not be issued while the doctest suites are still
    // running. The console (primary /dev/null, secondary USB-Serial/JTAG in
    // the JTAG build) carries this marker so plumbing/collect_gcov_jtag.py knows when the app
    // is ready.
#if CONFIG_ESP_CONSOLE_NONE
    // The single-UART device build has no console, so report the doctest
    // result and the dump start through the apptrace channel; the collector
    // picks them up from the same bytes as the dump.
    apptrace_puts(doctest_result == 0 ? "[doctest] Status: SUCCESS!" :
                                        "[doctest] Status: FAILURE!");
    apptrace_puts("coverage dump ready");
#else
    ESP_LOGI(TAG, "coverage dump ready");
#endif
    // Dump coverage data collected by the tests over the JTAG apptrace
    // channel (or the UART channel in the UART/QEMU builds). The host-side
    // test harness picks up the .gcda files.
    esp_gcov_dump();
    ESP_LOGI(TAG, "coverage dumped");
#elif LINUX_GCOV_DUMP
    // Flush the host coverage data collected by the tests. The .gcda files are
    // written next to the .gcno notes in the build tree, where gcovr picks
    // them up (see plumbing/run-linux-tests-coverage.sh).
    ESP_LOGI(TAG, "coverage dump ready");
    __gcov_dump();
    ESP_LOGI(TAG, "coverage dumped");
#endif
}
