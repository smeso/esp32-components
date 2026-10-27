/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

#include <doctest/doctest.h>

#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_psram.h>

#include "mock_heap.hpp"
#include "spiram_allocation.hpp"

namespace {

// True when the firmware runs on a target with a working SPI RAM heap. The
// ESP32 and ESP32-S3 QEMU machines emulate PSRAM (ESP32 only when started
// with -m), while the ESP32-C3 has no PSRAM at all and the test app does not
// enable SPIRAM. The mock in mock_heap.cpp mirrors this detection and falls
// back to internal RAM when no MALLOC_CAP_SPIRAM heap exists, so the
// allocator itself can always be exercised; only the vector tests below
// require real SPI RAM to verify placement.
bool
spiram_available()
{
#if CONFIG_SPIRAM
    return esp_psram_is_initialized();
#else
    return false;
#endif
}

}   // namespace

TEST_CASE("spiram allocator is a standard-compliant allocator")
{
    static_assert(std::is_default_constructible_v<SpiramAllocator<int>>);
    static_assert(std::is_copy_constructible_v<SpiramAllocator<int>>);
    static_assert(std::is_copy_assignable_v<SpiramAllocator<int>>);
    static_assert(std::is_nothrow_move_constructible_v<SpiramAllocator<int>>);
    static_assert(std::is_nothrow_move_assignable_v<SpiramAllocator<int>>);
    static_assert(std::is_same_v<SpiramAllocator<int>::value_type, int>);
}

TEST_CASE("allocator conversions and equality")
{
    // Converting constructor: allocators of different value types are
    // interchangeable.
    static_assert(std::is_constructible_v<SpiramAllocator<int>, SpiramAllocator<long>>);
    static_assert(std::is_constructible_v<SpiramAllocator<long>, SpiramAllocator<int>>);
    static_assert(
            std::is_same_v<decltype(SpiramAllocator<int>{} == SpiramAllocator<long>{}), bool>);

    CHECK(SpiramAllocator<int>{} == SpiramAllocator<long>{});
    CHECK(!(SpiramAllocator<int>{} != SpiramAllocator<long>{}));
}

TEST_CASE("spiram_vector is a std::vector over SpiramAllocator")
{
    static_assert(std::is_same_v<spiram_vector<int>, std::vector<int, SpiramAllocator<int>>>);
    static_assert(
            std::is_same_v<spiram_vector<float>, std::vector<float, SpiramAllocator<float>>>);
}

TEST_CASE("allocate returns 16-byte aligned memory")
{
    SpiramAllocator<std::uint64_t> alloc;
    auto *p = alloc.allocate(4);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
    if (spiram_available()) {
        CHECK(esp_ptr_external_ram(p));
    } else {
        // Without a SPI RAM heap the mock falls back to internal RAM.
        CHECK(!esp_ptr_external_ram(p));
    }
    alloc.deallocate(p, 4);
}

TEST_CASE("allocate respects the alignment of over-aligned types")
{
    struct alignas(32) Overaligned
    {
        std::uint64_t v[4];
    };
    static_assert(alignof(Overaligned) == 32);
    SpiramAllocator<Overaligned> alloc;
    auto *p = alloc.allocate(2);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 32 == 0);
    new (p) Overaligned{ { 1, 2, 3, 4 } };
    CHECK(p[0].v[0] == 1);
    CHECK(p[0].v[3] == 4);
    p[0].~Overaligned();
    alloc.deallocate(p, 2);
}

TEST_CASE("allocate(0) returns a valid pointer")
{
    SpiramAllocator<int> alloc;
    auto *p = alloc.allocate(0);
    REQUIRE(p != nullptr);
    alloc.deallocate(p, 0);
}

TEST_CASE("mock_heap detects SPI RAM availability")
{
    CHECK(mock_heap::spiram_available() == spiram_available());
}

TEST_CASE("mock_heap failure injection makes the allocator throw")
{
    SpiramAllocator<int> alloc;
    mock_heap::fail_allocations(true);
    CHECK_THROWS_AS((void) alloc.allocate(4), std::bad_alloc);
    mock_heap::fail_allocations(false);
    auto *p = alloc.allocate(4);
    REQUIRE(p != nullptr);
    alloc.deallocate(p, 4);
}

TEST_CASE("mock_heap forwards to the SPI RAM heap when available")
{
    if (!spiram_available()) {
        MESSAGE("SPI RAM not available on this target; skipping");
        return;
    }
    void *p = mock_heap::aligned_alloc(16, 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
    CHECK(esp_ptr_external_ram(p));
    mock_heap::free(p);
}

TEST_CASE("allocation failure throws std::bad_alloc")
{
    // A size that overflows when multiplied by sizeof(T) must throw before
    // touching the heap.
    SpiramAllocator<int> alloc;
    CHECK_THROWS_AS((void) alloc.allocate(std::size_t(-1) / sizeof(int) + 1), std::bad_alloc);
    // The largest count the multiplication alone would accept: the alignment
    // padding added by the round-up overflows std::size_t too, so the request
    // must be rejected instead of being rounded down to (almost) zero bytes.
    CHECK_THROWS_AS(
            (void) alloc.allocate(std::numeric_limits<std::size_t>::max() / sizeof(int)),
            std::bad_alloc);
    CHECK_THROWS_AS((void) SpiramAllocator<std::uint64_t>{}.allocate(
                            std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t)),
                    std::bad_alloc);
}

TEST_CASE("spiram_vector stores its data in SPI RAM")
{
    if (!spiram_available()) {
        MESSAGE("SPI RAM not available on this target; skipping");
        return;
    }
    spiram_vector<int> v;
    v.reserve(1024);
    CHECK(v.capacity() >= 1024);
    for (int i = 0; i < 1024; ++i)
        v.push_back(i);
    CHECK(v.size() == 1024);
    CHECK(v.front() == 0);
    CHECK(v.back() == 1023);
    for (int i = 0; i < 1024; ++i)
        CHECK(v[static_cast<size_t>(i)] == i);
    CHECK(esp_ptr_external_ram(v.data()));
}

TEST_CASE("spiram_vector can hold floats")
{
    if (!spiram_available()) {
        MESSAGE("SPI RAM not available on this target; skipping");
        return;
    }
    spiram_vector<float> v(128, 1.5F);
    CHECK(v.size() == 128);
    CHECK(v.front() == 1.5F);
    CHECK(v.back() == 1.5F);
    CHECK(esp_ptr_external_ram(v.data()));
}
