/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mock_heap.hpp"

#include <esp_heap_caps.h>
#include <esp_psram.h>

// The real functions behind the --wrap redirects below. The linker provides
// the __real_* symbols when --wrap is active; they must be declared here
// because C++ does not allow implicit function declarations.
extern "C" {
void *__real_heap_caps_aligned_alloc(std::size_t alignment,
                                     std::size_t size,
                                     std::uint32_t caps);
void __real_heap_caps_free(void *ptr);
bool __real_esp_ptr_external_ram(const void *p);
}

namespace mock_heap {

namespace {

bool fail = false;

}   // namespace

bool
spiram_available()
{
#if CONFIG_SPIRAM
    return esp_psram_is_initialized();
#else
    return false;
#endif
}

void *
aligned_alloc(std::size_t alignment, std::size_t size, std::uint32_t caps)
{
    if (fail)
        return nullptr;
    if (spiram_available())
        // Forward to the real SPI RAM heap, exactly like the allocator under
        // test would do on real hardware. The QEMU test targets have no SPI
        // RAM heap, so this branch is only reachable on real hardware.
        return __real_heap_caps_aligned_alloc(alignment, size, caps);
    // No SPI RAM heap: fall back to the regular internal heap (this is what
    // malloc() resolves to in ESP-IDF), so the allocator under test gets
    // valid, correctly aligned memory instead of failing.
    return __real_heap_caps_aligned_alloc(
            alignment, size, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
}

void
free(void *ptr)
{
    __real_heap_caps_free(ptr);
}

void
fail_allocations(bool fail_)
{
    fail = fail_;
}

}   // namespace mock_heap

// ---------------------------------------------------------------------------
// Mocked ESP-IDF heap entry points.
//
// The test component is linked into the firmware before the real heap
// libraries (test components are linked with --whole-archive, see
// tools/cmake/project.cmake), so the --wrap options in this component's
// CMakeLists.txt redirect every reference to heap_caps_aligned_alloc() and
// heap_caps_free() to the __wrap_* implementations below. They detect at
// compile and runtime whether SPI RAM is available and forward to the real
// functions when it is, so the allocator under test works unchanged on real
// hardware; otherwise they fall back to internal RAM (what malloc/free
// resolve to), letting the spiram_allocation and sound_driven tests run even
// where no MALLOC_CAP_SPIRAM heap exists (e.g. QEMU without PSRAM emulation).
//
// Calls always end up in the real __real_* functions: calling plain
// malloc()/free() here would recurse, because esp_libc implements them on top
// of heap_caps_malloc_default()/heap_caps_free(), whose references are also
// redirected by --wrap.
// ---------------------------------------------------------------------------

extern "C" {

void *
__wrap_heap_caps_aligned_alloc(std::size_t alignment, std::size_t size, std::uint32_t caps)
{
    return mock_heap::aligned_alloc(alignment, size, caps);
}

void
__wrap_heap_caps_free(void *ptr)
{
    mock_heap::free(ptr);
}

// The real esp_ptr_external_ram() lives in esp_hw_support, which is not
// usable for the linux target. There is no SPI RAM there, so the mock always
// reports false; the spiram_allocation tests then take their no-PSRAM path.
// On device builds with SPI RAM the real function is forwarded to, exactly
// like the heap mocks do, so the placement checks in the tests still pass.
bool
__wrap_esp_ptr_external_ram(const void *p)
{
#if CONFIG_SPIRAM
    if (mock_heap::spiram_available())
        return __real_esp_ptr_external_ram(p);
#endif
    return false;
}

}   // extern "C"
