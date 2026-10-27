/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Header-only C++ wrapper around xTaskCreate() that accepts any callable.
 *
 * cppTaskCreate() is a drop-in replacement for xTaskCreate(): the callable is
 * moved onto the heap and invoked by a plain trampoline that runs as the
 * FreeRTOS task. The trampoline catches every exception the callable may
 * throw, logs it and aborts: control never escapes back into the (already
 * nonexistent) caller's frame, which is the only safe thing to do when a
 * FreeRTOS task body ends.
 *
 * @note C++ exceptions must be enabled in the project configuration
 * (CONFIG_COMPILER_CXX_EXCEPTIONS): the trampoline needs a working try/catch
 * to contain the callable's exceptions. The preprocessor check below makes a
 * missing option a build error instead of a cryptic one raised from inside
 * the standard library.
 */

#pragma once

#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

// abi::__forced_unwind, referenced by the forced-unwind clause of
// forward_forced_unwind(). The clause is only compiled for the Linux simulator
// (see the CONFIG_IDF_TARGET_LINUX branches), but the name must still resolve
// on every target, so the header of the C++ ABI is included unconditionally.
#include <cxxabi.h>

// sdkconfig.h provides the CONFIG_* macros this header branches on:
// CONFIG_IDF_TARGET_LINUX, CONFIG_COMPILER_CXX_EXCEPTIONS,
// CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS,
// CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS and
// CONFIG_CPPTASKCREATE_TLS_INDEX. It is included explicitly so that the
// header is self-contained: without it a translation unit that does not reach
// sdkconfig.h transitively would silently evaluate every undefined CONFIG_*
// to 0 in the #if branches below (and, e.g., lose the Linux forced-unwind
// forwarding at run time).
#include <sdkconfig.h>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
// vTaskSetThreadLocalStoragePointerAndDelCallback() and its
// TlsDeleteCallbackFunction_t type, used to release the callable's context
// when a task ends with vTaskDelete(). The declaration exists only when
// CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS is set: the check below makes the
// option a hard requirement on every non-Linux target.
#include <freertos/idf_additions.h>

// The task trampoline is built around try/catch: without exceptions the
// compiler would reject it deep inside the standard library with a cryptic
// message. Fail here instead, naming the option.
#if !CONFIG_COMPILER_CXX_EXCEPTIONS
#error "cpptaskcreate requires C++ exceptions (CONFIG_COMPILER_CXX_EXCEPTIONS=y): the task trampoline relies on try/catch to contain the callable's exceptions"
#endif

// On every target but the Linux simulator cppTaskCreate() hands each task's
// callable to the kernel through a thread local storage deletion callback, so
// that a task which ends without unwinding its C++ stack (vTaskDelete() on
// real hardware) still releases it. The callback API only exists when
// CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS is enabled; the Linux simulator
// does not use it (a self-deleting task is ended by a forced unwind that
// destroys the callable's context directly) and the option is therefore free
// to be off there, but it is a hard requirement everywhere else.
#if !CONFIG_IDF_TARGET_LINUX && !CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS
#error "CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS must be enabled on non-Linux targets: cppTaskCreate() relies on the thread local storage deletion callback to free a task's callable when the task is deleted"
#endif

namespace cpptaskcreate {

inline constexpr const char *TAG = "cpptaskcreate";

#if !CONFIG_IDF_TARGET_LINUX
// Thread local storage pointer holding the callable's heap context, so that
// FreeRTOS can free it when the task is deleted: see tls_fn_delete() below.
// The index is a Kconfig option (CPPTASKCREATE_TLS_INDEX) because it is a
// global FreeRTOS resource shared with the rest of the application: index 0
// is reserved by ESP-IDF for the pthreads API, and a component that picks the
// same index would overwrite cppTaskCreate()'s pointer or its deletion
// callback without any diagnostic. Set a different index if the default one
// collides.
inline constexpr BaseType_t tls_fn_index = CONFIG_CPPTASKCREATE_TLS_INDEX;

// The context lives at tls_fn_index, which the Kconfig option keeps away from
// 0 (reserved by ESP-IDF for the pthreads API, read by the C++ exception
// machinery), so at least two thread local storage pointers are required.
static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS > tls_fn_index,
              "CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS must be larger "
              "than the thread local storage pointer index used by "
              "cppTaskCreate(): index 0 is reserved by ESP-IDF for the "
              "pthreads API (used by the C++ exception machinery), so the "
              "callable's context needs one more slot (see "
              "CPPTASKCREATE_TLS_INDEX)");

// FreeRTOS deletion callback registered for tls_fn_index: releases the heap
// context stored by cppTaskCreate() when the task that owns it is deleted.
// The kernel calls it from prvDeleteTCB(), i.e. on the stack of the task that
// deletes the owner (or of the idle task when the owner deleted itself), with
// the scheduler lock held, so it must be short and non-blocking and must not
// throw. Deleting the std::function is nevertheless user code: it runs the
// destructors of everything the user's callable captured (see the contract at
// cppTaskCreate() below). extern "C" matches TlsDeleteCallbackFunction_t,
// which the kernel declares inside an extern "C" block.
extern "C" {
inline void
tls_fn_delete(int, void *ptr) noexcept
{
    delete static_cast<std::function<void()> *>(ptr);   // GCOVR_EXCL_BR_LINE: noexcept
}
}   // extern "C"
#endif

#if CONFIG_IDF_TARGET_LINUX
/*
 * The Linux port tears down a task that deletes itself with pthread_exit()
 * from inside vTaskDelete(): glibc implements the thread exit as a forced
 * unwind of type abi::__forced_unwind. Unlike a regular C++ exception it must
 * be rethrown: a catch-all clause anywhere on the unwound stack swallows it
 * and libstdc++ then aborts the whole process with "FATAL: exception not
 * rethrown" instead of letting the thread die. Forward it to glibc's
 * thread-exit frame and let anything else fall through to the caller's abort.
 *
 * The clause is kept in its own function so that the null sanitizer
 * suppression it needs stays local to it: UBSan reports the catch below as a
 * "reference binding to null pointer of type 'struct __forced_unwind'" (a
 * forced unwind carries no C++ exception object, so the reference it binds is
 * null even though catching it is valid, GCC PR 100415). The attribute is
 * spelled the GNU way because [[gnu::no_sanitize]] is not understood by
 * clang.
 */
// GCOVR_EXCL_START: EH cleanup edges of the forced-unwind clause are never
// recorded by gcov, although the tests below execute both outcomes.
__attribute__((no_sanitize("null"))) inline void
forward_forced_unwind()
{
    try {
        throw;
    } catch (abi::__forced_unwind &) {
        throw;
    } catch (...) {
        /* Not the forced unwind: fall through to the caller's abort. */
    }
}
// GCOVR_EXCL_STOP
#endif

/*
 * Entry point handed to FreeRTOS. Runs the user callable and guarantees that
 * control never escapes back into the (nonexistent) caller's frame:
 *  - normal return -> error + abort
 *  - uncaught throw -> error + abort
 *
 * The parameter is the address of a heap-allocated std::function<void()>
 * holding the user's callable (the indirection keeps the trampoline signature
 * independent of the callable's type, so one instantiation serves every
 * task). An empty context can never reach this function: cppTaskCreate()
 * rejects empty callables before creating the task.
 *
 * The context is owned by a unique_ptr, so it is released along every path
 * that unwinds the trampoline's frame: the two abort paths below and, on the
 * Linux simulator, the forced unwind that ends a task calling vTaskDelete().
 *
 * On real hardware a task that calls vTaskDelete(NULL) is torn down without
 * unwinding its C++ stack, so the unique_ptr never runs there: cppTaskCreate()
 * also registers the context as a thread local storage pointer with a deletion
 * callback (before it returns the task handle) and FreeRTOS frees it while
 * deleting the task, no matter how the task ends.
 *
 * extern "C" matches TaskFunction_t, which the kernel declares inside an
 * extern "C" block.
 */
extern "C" {
[[noreturn]] inline void
task_entry(void *pv)
{
    // GCOVR_EXCL_BR_START: gcov records a throw edge for the unique_ptr
    // construction, although it cannot throw (the constructor is noexcept).
    // The normal fallthrough branch of the line is the one the tests exercise.
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()> *>(pv));
    // GCOVR_EXCL_BR_STOP

    // GCOVR_EXCL_START: EH cleanup edges of the try/catch are never recorded
    // by gcov on xtensa (the unwinder resumes into the landing pad without
    // updating the counters), although the tests below execute every path.
    try {
        (*fn)();
        ESP_LOGE(TAG, "task '%s' returned, aborting", pcTaskGetName(nullptr));
    } catch (const std::exception &e) {
        ESP_LOGE(TAG,
                 "task '%s' threw an uncaught exception: %s",
                 pcTaskGetName(nullptr),
                 e.what() != nullptr ? e.what() : "(no message)");
    } catch (...) {
#if CONFIG_IDF_TARGET_LINUX
        // Chip targets do not use pthreads and never take this path. The
        // preprocessor (not if constexpr) is required here: task_entry() is
        // not a template, so the discarded statement of an if constexpr would
        // still be parsed and forward_forced_unwind() would have to exist on
        // every target.
        forward_forced_unwind();
#endif
        ESP_LOGE(TAG, "task '%s' threw an uncaught exception", pcTaskGetName(nullptr));
    }
    // GCOVR_EXCL_STOP

    /* Function never returns: abort() never comes back, and even if the task
     * were deleted from a hook instead, vTaskDelete() would prevent falling
     * off the end of a FreeRTOS task. The kernel's deletion callback is
     * unregistered before the context is released, so that a task parked at
     * the abort (e.g. by an abort() hook that suspends it) cannot later make
     * FreeRTOS's TCB cleanup delete the context a second time. A debugger
     * inspecting the task at the abort point still sees no leak, because the
     * context is released right here. */
#if !CONFIG_IDF_TARGET_LINUX
    // GCOVR_EXCL_BR_START: gcov records a throw edge for this call because the
    // unique_ptr above has to be destroyed if it throws, although the call is
    // plain C and cannot throw; the fallthrough branch is the one the tests
    // exercise (the same reason the unique_ptr construction is excluded).
    vTaskSetThreadLocalStoragePointerAndDelCallback(nullptr, tls_fn_index, nullptr, nullptr);
    // GCOVR_EXCL_BR_STOP
#endif
    fn.reset();   // GCOVR_EXCL_BR_LINE: delete is noexcept, no real branch here
    std::abort();
}   // GCOVR_EXCL_LINE: std::abort() is [[noreturn]], so this brace is unreachable;
    // gcov reports it as an exception-only line because of fn's destructor
}   // extern "C"

}   // namespace cpptaskcreate

/**
 * Drop-in replacement for xTaskCreate() that accepts any C++ callable
 * (function, lambda, bind expression, functor, ...).
 *
 * The callable must outlive the caller's stack frame, so it is copied or
 * moved onto the heap. Its ownership is then handed to the kernel before this
 * function returns: the context is registered at a thread local storage
 * pointer index (see CPPTASKCREATE_TLS_INDEX) with a deletion callback, so
 * FreeRTOS releases it whenever the task is deleted - including a task that
 * is deleted before it ever runs, which is why the registration happens here
 * and not from inside the task. (Registering here means the creating task
 * writes the new task's thread local storage pointer; the new task cannot
 * have deleted itself in the meantime on a single-core system, because the
 * idle task that reclaims a deleted task's TCB cannot run while the creator,
 * which does not block before registering, is ready. On an SMP system a task
 * that manages to run and self-delete before the creator registers is the one
 * remaining race; it can only leak, never corrupt, the context.)
 *
 * A callable whose destructor blocks (vTaskDelay(), mutex/queue waits,
 * ESP_LOG*) or throws is not supported: FreeRTOS runs the deletion callback
 * from prvDeleteTCB(), on the stack of the task that deletes the owner (or of
 * the idle task when the owner deleted itself), with the scheduler lock held.
 * The same constraint applies to the destructors of everything the callable
 * captured. The callback itself is noexcept, so a throwing destructor
 * terminates the process rather than propagating through the kernel.
 *
 * An empty callable (an empty std::function, a null function pointer, ...) is
 * rejected before the task is created and pdFAIL is returned: invoking it
 * would throw std::bad_function_call, which the trampoline would turn into an
 * abort of the whole application.
 *
 * When the task creation fails, the callable is deleted again and the function
 * returns an error (like xTaskCreate()). By-reference captures still dangle
 * exactly as they would in plain C if the referenced objects do not outlive
 * the task, so capture by value (or through an owned shared_ptr) whenever the
 * referenced object's lifetime is not guaranteed to span the task's.
 *
 * @param name task name (as in xTaskCreate())
 * @param stack_depth task stack depth (the ulStackDepth argument of
 *        xTaskCreate(): in units of StackType_t, e.g. bytes on the xtensa and
 *        riscv ports; IDF defines configSTACK_DEPTH_TYPE as uint32_t)
 * @param priority task priority (as in xTaskCreate())
 * @param handle optional output for the created task's handle (may be
 *        nullptr, as in xTaskCreate())
 * @param fn the callable to run in the new task
 * @return pdPASS on success; pdFAIL when the callable is empty; otherwise the
 *         error reported by xTaskCreate()
 *         (e.g. errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY), with the callable
 *         freed again
 * @throws std::bad_alloc when the heap allocation of the callable's context
 *         fails, and any exception thrown by the callable's own copy/move
 *         constructor (both happen inside the same new-expression)
 */
template<typename Callable>
[[nodiscard]] BaseType_t
cppTaskCreate(const char *name,
              configSTACK_DEPTH_TYPE stack_depth,
              UBaseType_t priority,
              TaskHandle_t *handle,
              Callable &&fn)
{
    static_assert(std::is_invocable_v<Callable &>,
                  "the callable must be invocable with no arguments");
    static_assert(std::is_copy_constructible_v<std::decay_t<Callable>>,
                  "the callable must be copy-constructible: it travels "
                  "through std::function<void()>, which requires a copyable "
                  "target (capture move-only state through an owning smart "
                  "pointer so that the closure itself stays copyable)");

    // The allocation below can only fail with std::bad_alloc, whose throw
    // edge the unwinder never records in gcov; the xTaskCreate() failure
    // branch below is exercised deterministically by the "ctc_oom" test.
    // GCOVR_EXCL_BR_LINE: the only recorded branch is the std::bad_alloc
    // throw edge of operator new, which no test can trigger on the chip
    // (heap exhaustion is not controllable), and which the unwinder never
    // counts even when the constructor does throw.
    auto *ctx = new std::function<void()>(std::forward<Callable>(fn));   // GCOVR_EXCL_BR_LINE

    // An empty callable passes the is_invocable check above but cannot be
    // invoked: reporting the error here keeps task_entry()'s abort for the
    // "callable is broken" case only. Both outcomes are exercised by the
    // tests ("ctc_empty" takes the branch, every other create does not).
    if (!*ctx) {
        delete ctx;   // GCOVR_EXCL_BR_LINE: delete is noexcept, no real branch here
        return pdFAIL;
    }

    TaskHandle_t created = nullptr;
    const BaseType_t ret =
            xTaskCreate(cpptaskcreate::task_entry, name, stack_depth, ctx, priority, &created);
    // GCOVR_EXCL_BR_LINE: both outcomes are covered by the tests (success in
    // every task test, failure via the mock xTaskCreate in "ctc_oom"), but
    // gcovr reports the branch as untaken when the header template is
    // instantiated from several test translation units at once.
    if (ret != pdPASS) {   // GCOVR_EXCL_BR_LINE
        // The task was never created: nobody else will free the callable.
        delete ctx;   // GCOVR_EXCL_BR_LINE: delete is noexcept, no real branch here
        return ret;
    }

    if (handle != nullptr) {
        *handle = created;
    }

#if !CONFIG_IDF_TARGET_LINUX
    // Hand the context to the kernel before the caller gets the handle, so
    // that deleting the task before it first runs frees it too. The write is
    // idempotent with respect to the task's own state: nothing else touches
    // tls_fn_index for this task.
    vTaskSetThreadLocalStoragePointerAndDelCallback(
            created, cpptaskcreate::tls_fn_index, ctx, &cpptaskcreate::tls_fn_delete);
#endif

    return ret;
}
