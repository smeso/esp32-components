/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Header-only C++ helpers to allocate memory in SPI RAM (PSRAM).
 *
 * @note Exceptions must be enabled in the project configuration
 * (CONFIG_COMPILER_CXX_EXCEPTIONS): allocation failures are reported by
 * throwing std::bad_alloc. Note also that, when CONFIG_HEAP_ABORT_WHEN_
 * ALLOCATION_FAILS is enabled, the ESP-IDF heap aborts before std::bad_alloc
 * can be thrown.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <vector>

#include <esp_heap_caps.h>

/**
 * A standard-library-compatible allocator that allocates from the SPI RAM
 * heap via heap_caps_aligned_alloc() with MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT.
 *
 * Memory is always aligned to at least 16 bytes, or to alignof(T) when the
 * type requires stricter alignment. Throws std::bad_alloc when no memory is
 * available; a working SPI RAM heap is required at runtime, otherwise every
 * allocation throws.
 *
 * @tparam T the value type to allocate
 */
template<typename T>
struct SpiramAllocator
{
    using value_type = T;

    constexpr SpiramAllocator() = default;

    template<typename U>
    constexpr SpiramAllocator(const SpiramAllocator<U> &) noexcept
    {
    }

    [[nodiscard]] T *allocate(std::size_t n)
    {
        constexpr std::size_t alignment = std::max(std::size_t{ 16 }, alignof(T));
        // Reject counts whose padded byte count does not fit in std::size_t:
        // both the n * sizeof(T) product and the alignment - 1 that the
        // round-up below adds to it must not overflow, otherwise the wrapped
        // size would be requested from the heap.
        if (n > (std::numeric_limits<std::size_t>::max() - (alignment - 1)) / sizeof(T))
            throw std::bad_alloc();
        if (n == 0)
            n = 1;
        // C11 aligned_alloc() (used by the Linux simulator heap) and the ASan
        // aligned_alloc interceptor require the size to be an integral multiple
        // of the alignment. Small requests (e.g. allocate(0): 1 int, 4 bytes,
        // 16-byte alignment) would otherwise abort SANITIZERS runs even though
        // the ESP-IDF heap itself would serve them fine, so round the size up.
        // This is okay because SPIRAM is supposed to be used for large allocations
        // so this change makes no difference for real production uses.
        std::size_t bytes = n * sizeof(T);
        bytes = (bytes + alignment - 1) / alignment * alignment;
        if (auto *p = static_cast<T *>(heap_caps_aligned_alloc(
                    alignment, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
            return p;
        throw std::bad_alloc();
    }

    void deallocate(T *p, std::size_t) noexcept
    {
        heap_caps_free(p);
    }
};

template<typename T, typename U>
constexpr bool
operator==(const SpiramAllocator<T> &, const SpiramAllocator<U> &) noexcept
{
    return true;
}

/**
 * std::vector over SpiramAllocator: a vector whose storage always lives in
 * SPI RAM. Throws std::bad_alloc when the SPI RAM heap cannot serve a
 * request.
 *
 * @tparam T the element type
 */
template<typename T>
using spiram_vector = std::vector<T, SpiramAllocator<T>>;
