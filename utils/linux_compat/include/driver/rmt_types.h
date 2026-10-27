/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal stand-in for the esp_driver_rmt <driver/rmt_types.h> header (plus
 * the hal types it re-exports), used only for the FreeRTOS Linux simulator
 * build. The real component registers no headers for the linux target; the
 * RMT driver calls made by the components under test are redirected to the
 * mocks at link time, so only the types and function declarations are needed
 * to compile.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int rmt_clock_source_t;
#define RMT_CLK_SRC_DEFAULT 0

typedef struct rmt_channel_t *rmt_channel_handle_t;
typedef struct rmt_encoder_t *rmt_encoder_handle_t;

typedef union
{
    struct
    {
        uint16_t duration0 : 15;
        uint16_t level0 : 1;
        uint16_t duration1 : 15;
        uint16_t level1 : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;

typedef struct
{
    int gpio_num;
    rmt_clock_source_t clk_src;
    uint32_t resolution_hz;
    size_t mem_block_symbols;
    size_t trans_queue_depth;
    int intr_priority;
    struct
    {
        uint32_t invert_out : 1;
        uint32_t with_dma : 1;
        uint32_t allow_pd : 1;
        uint32_t init_level : 1;
    } flags;
} rmt_tx_channel_config_t;

typedef struct
{
    int loop_count;
    struct
    {
        uint32_t eot_level : 1;
        uint32_t queue_nonblocking : 1;
    } flags;
} rmt_transmit_config_t;

typedef struct
{
    rmt_symbol_word_t bit0;
    rmt_symbol_word_t bit1;
    struct
    {
        uint32_t msb_first : 1;
    } flags;
} rmt_bytes_encoder_config_t;

#ifdef __cplusplus
}
#endif
