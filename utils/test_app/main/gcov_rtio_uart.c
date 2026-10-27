/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// UART (fire-and-forget) file I/O implementation for the gcov runtime, used
// when the test app dumps .gcda data over the apptrace UART (UART1 in the
// coverage build) both on real hardware and under QEMU. Kept out of esp_gcov/
// (which mirrors upstream esp_gcov) and wired in via linker --wrap so these
// symbols win over esp_gcov's JTAG implementations.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_trace.h"
#include "esp_log.h"

static const char *TAG = "esp_gcov_rtio";

#if CONFIG_APPTRACE_DEST_UART

/* Host file I/O command IDs. Must match the host-side implementation (OpenOCD
 * esp_gcov.c / IDF's host_file_io.c). */
#define ESP_APPTRACE_FILE_CMD_FOPEN 0x0
#define ESP_APPTRACE_FILE_CMD_FCLOSE 0x1
#define ESP_APPTRACE_FILE_CMD_FWRITE 0x2
#define ESP_APPTRACE_FILE_CMD_FREAD 0x3
#define ESP_APPTRACE_FILE_CMD_FSEEK 0x4
#define ESP_APPTRACE_FILE_CMD_FTELL 0x5
#define ESP_APPTRACE_FILE_CMD_STOP 0x6
#define ESP_APPTRACE_FILE_CMD_FEOF 0x7

/* With the UART transport the host cannot deliver responses synchronously
 * (OpenOCD uses JTAG and writes responses back into trace memory). The host
 * side only needs the command stream: it creates/updates the .gcda files as
 * the FCMD messages arrive. Sending without waiting for a response is also
 * what keeps the flow working under QEMU, where the emulated UART RX FIFO
 * cannot be relied upon to deliver a response within the poll window. */
static esp_err_t
esp_gcov_file_cmd_send(uint8_t cmd, const uint8_t *args, uint32_t args_len)
{
    esp_err_t ret = esp_apptrace_write(&cmd, 1, ESP_APPTRACE_TMO_INFINITE);
    if (ret != ESP_OK) {
        return ret;
    }
    if (args_len > 0) {
        ret = esp_apptrace_write(args, args_len, ESP_APPTRACE_TMO_INFINITE);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return esp_apptrace_flush(ESP_APPTRACE_TMO_INFINITE);
}

void *
__wrap_gcov_rtio_fopen(const char *path, const char *mode)
{
    ESP_EARLY_LOGV(TAG, "%s '%s' '%s'", __FUNCTION__, path, mode);
    size_t path_len = strlen(path) + 1;
    size_t mode_len = strlen(mode) + 1;
    uint8_t *args = malloc(path_len + mode_len);
    if (args == NULL) {
        return NULL;
    }
    memcpy(args, path, path_len);
    memcpy(args + path_len, mode, mode_len);
    esp_err_t ret =
            esp_gcov_file_cmd_send(ESP_APPTRACE_FILE_CMD_FOPEN, args, path_len + mode_len);
    free(args);
    if (ret != ESP_OK) {
        return NULL;
    }
    /* The host tracks files by the order in which they are opened. */
    return (void *) (uintptr_t) 1;
}

int
__wrap_gcov_rtio_fclose(void *stream)
{
    ESP_EARLY_LOGV(TAG, "%s", __FUNCTION__);
    uint32_t fd = (uint32_t) (uintptr_t) stream;
    esp_err_t ret = esp_gcov_file_cmd_send(
            ESP_APPTRACE_FILE_CMD_FCLOSE, (const uint8_t *) &fd, sizeof(fd));
    if (ret != ESP_OK) {
        return EOF;
    }
    return 0;
}

size_t
__wrap_gcov_rtio_fread(void *ptr, size_t size, size_t nmemb, void *stream)
{
    ESP_EARLY_LOGV(TAG, "%s read %u", __FUNCTION__, size * nmemb);
    uint32_t fd = (uint32_t) (uintptr_t) stream;
    uint32_t args[2] = { fd, size * nmemb };
    esp_err_t ret = esp_gcov_file_cmd_send(
            ESP_APPTRACE_FILE_CMD_FREAD, (const uint8_t *) args, sizeof(args));
    if (ret != ESP_OK) {
        return 0;
    }
    /* The host has no existing data to merge, report EOF/empty file. */
    return 0;
}

size_t
__wrap_gcov_rtio_fwrite(const void *ptr, size_t size, size_t nmemb, void *stream)
{
    ESP_EARLY_LOGV(TAG, "%s", __FUNCTION__);
    uint32_t fd = (uint32_t) (uintptr_t) stream;
    uint32_t len = size * nmemb;
    uint8_t *args = malloc(sizeof(fd) + sizeof(len) + len);
    if (args == NULL) {
        return 0;
    }
    memcpy(args, &fd, sizeof(fd));
    memcpy(args + sizeof(fd), &len, sizeof(len));
    memcpy(args + sizeof(fd) + sizeof(len), ptr, len);
    esp_err_t ret = esp_gcov_file_cmd_send(
            ESP_APPTRACE_FILE_CMD_FWRITE, args, sizeof(fd) + sizeof(len) + len);
    free(args);
    if (ret != ESP_OK) {
        return 0;
    }
    return nmemb;
}

int
__wrap_gcov_rtio_fseek(void *stream, long offset, int whence)
{
    ESP_EARLY_LOGV(TAG, "%s(%p %ld %d)", __FUNCTION__, stream, offset, whence);
    uint32_t fd = (uint32_t) (uintptr_t) stream;
    int32_t args[3] = { fd, offset, whence };
    esp_err_t ret = esp_gcov_file_cmd_send(
            ESP_APPTRACE_FILE_CMD_FSEEK, (const uint8_t *) args, sizeof(args));
    if (ret != ESP_OK) {
        return -1;
    }
    return 0;
}

long
__wrap_gcov_rtio_ftell(void *stream)
{
    ESP_EARLY_LOGV(TAG, "%s(%p)", __FUNCTION__, stream);
    uint32_t fd = (uint32_t) (uintptr_t) stream;
    esp_err_t ret = esp_gcov_file_cmd_send(
            ESP_APPTRACE_FILE_CMD_FTELL, (const uint8_t *) &fd, sizeof(fd));
    if (ret != ESP_OK) {
        return -1;
    }
    return 0;
}

int
__wrap_gcov_rtio_feof(void *stream)
{
    ESP_EARLY_LOGV(TAG, "%s(%p)", __FUNCTION__, stream);
    uint32_t fd = (uint32_t) (uintptr_t) stream;
    esp_err_t ret = esp_gcov_file_cmd_send(
            ESP_APPTRACE_FILE_CMD_FEOF, (const uint8_t *) &fd, sizeof(fd));
    if (ret != ESP_OK) {
        return EOF;
    }
    return 0;
}

#endif /* CONFIG_APPTRACE_DEST_UART */
