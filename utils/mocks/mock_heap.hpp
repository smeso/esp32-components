/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace mock_heap {

// True when a real SPI RAM heap is available at runtime (compile-time
// CONFIG_SPIRAM plus esp_psram_is_initialized()).
bool spiram_available();

// Allocate from the SPI RAM heap when it is available, otherwise fall back to
// the regular internal heap. Returns a pointer aligned to `alignment` or
// nullptr (also when fail_allocations() is active).
void *aligned_alloc(std::size_t alignment, std::size_t size, std::uint32_t caps);

// Release memory previously returned by aligned_alloc() (or by the allocator
// under test when it ran on real SPI RAM). Always forwards to the real
// heap_caps_free().
void free(void *ptr);

// Failure injection: make aligned_alloc() return nullptr.
void fail_allocations(bool fail);

}   // namespace mock_heap
