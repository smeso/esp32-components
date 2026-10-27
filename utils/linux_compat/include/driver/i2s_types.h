/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal stand-in for the esp_driver_i2s <driver/i2s_types.h> header, used
 * only for the FreeRTOS Linux simulator build. esp_driver_i2s is not
 * available for the linux target, but the codec drivers under test only need
 * these declarations at compile time: the actual i2s_channel_write()/read()
 * calls are redirected to the mocks in components/utils/mocks, so no real I2S
 * driver is linked. Device builds keep using the real esp_driver_i2s headers.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct i2s_channel_obj_t *i2s_chan_handle_t;

typedef enum
{
    I2S_MCLK_MULTIPLE_128 = 128,
    I2S_MCLK_MULTIPLE_192 = 192,
    I2S_MCLK_MULTIPLE_256 = 256,
    I2S_MCLK_MULTIPLE_384 = 384,
    I2S_MCLK_MULTIPLE_512 = 512,
    I2S_MCLK_MULTIPLE_576 = 576,
    I2S_MCLK_MULTIPLE_768 = 768,
    I2S_MCLK_MULTIPLE_1024 = 1024,
    I2S_MCLK_MULTIPLE_1152 = 1152,
} i2s_mclk_multiple_t;

#ifdef __cplusplus
}
#endif
