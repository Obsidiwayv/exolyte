// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "object/handle.h"

#include <lib/counters.h>
#include <pow2.h>

#include <fbl/conditional_select_nospec.h>
#include <object/dispatcher.h>

namespace {

// The number of outstanding (live) handles in the arena.
constexpr size_t kMaxHandleCount = 256 * 1024u;

// Warning level: When the number of handles exceeds this value, we start to emit
// warnings to the kernel's debug log.
constexpr size_t kHighHandleCount = (kMaxHandleCount * 7) / 8;

KCOUNTER(handle_count_made, "handles.made")
KCOUNTER(handle_count_duped, "handles.duped")
KCOUNTER(handle_count_live, "handles.live")
KCOUNTER(handle_count_alloc_failed, "handles.alloc.failed")

// Masks for building a Handle's base_value, which ProcessDispatcher
// uses to create zx_handle_t values.
//
// base_value bit fields:
//   [31..(32 - kHandleReservedBits)]                     : Must be zero
//   [(31 - kHandleReservedBits)..kHandleGenerationShift] : Generation number
//                                                          Masked by kHandleGenerationMask
//   [kHandleGenerationShift-1..0]                        : Index into handle_arena
//                                                          Masked by kHandleIndexMask
constexpr uint32_t kHandleIndexMask = kMaxHandleCount - 1;
static_assert((kHandleIndexMask & kMaxHandleCount) == 0, "kMaxHandleCount must be a power of 2");

constexpr uint32_t kHandleReservedBitsMask = ((1 << kHandleReservedBits) - 1)
                                             << (32 - kHandleReservedBits);
constexpr uint32_t kHandleGenerationMask = ~kHandleIndexMask & ~kHandleReservedBitsMask;
constexpr uint32_t kHandleGenerationShift = log2_uint_floor(kMaxHandleCount);
static_assert(((3 << (kHandleGenerationShift - 1)) & kHandleGenerationMask) ==
                  1 << kHandleGenerationShift,
              "Shift is wrong");
static_assert((kHandleGenerationMask >> kHandleGenerationShift) >= 255,
              "Not enough room for a useful generation count");

static_assert((kHandleReservedBitsMask & kHandleGenerationMask) == 0, "Handle Mask Overlap!");
static_assert((kHandleReservedBitsMask & kHandleIndexMask) == 0, "Handle Mask Overlap!");
static_assert((kHandleGenerationMask & kHandleIndexMask) == 0, "Handle Mask Overlap!");
static_assert((kHandleReservedBitsMask | kHandleGenerationMask | kHandleIndexMask) == 0xffffffffu,
              "Handle masks do not cover all bits!");

// |index| is the literal index into the table. |old_value| is the
// |index mixed with the per-handle-lifetime state.
uint32_t NewHandleValue(uint32_t index, uint32_t old_value) {
  DEBUG_ASSERT((index & ~kHandleIndexMask) == 0);

  uint32_t old_gen = 0;
  if (old_value != 0) {
    // This slot has been used before.
    DEBUG_ASSERT((old_value & kHandleIndexMask) == index);
    old_gen = (old_value & kHandleGenerationMask) >> kHandleGenerationShift;
  }
  uint32_t new_gen = (((old_gen + 1) << kHandleGenerationShift) & kHandleGenerationMask);
  return (index | new_gen);
}

uint32_t HandleValueToIndex(uint32_t value) { return value & kHandleIndexMask; }

}  // namespace

HandleTableArena gHandleTableArena;

void Handle::Init() { gHandleTableArena.arena_.Init("handles", kMaxHandleCount); }

void Handle::set_handle_table_id(zx_koid_t pid) {
  handle_table_id_.store(pid, ktl::memory_order_relaxed);
  dispatcher_->set_owner(pid);
}

// Returns a new |base_value| based on the value stored in the free
// arena slot pointed to by |addr|. The new value will be different
// from the last |base_value| used by this slot.
uint32_t HandleTableArena::GetNewBaseValue(void* addr) {
  // Get the index of this slot within the arena.
  uint32_t handle_index = HandleToIndex(reinterpret_cast<Handle*>(addr));

  // Check the free memory for a stashed base_value.
  uint32_t v = reinterpret_cast<Handle*>(addr)->base_value_;

  return NewHandleValue(handle_index, v);
}

// Allocate space for a Handle from the arena, but don't instantiate the
// object.  |base_value| gets the value for Handle::base_value_.  |what|
// says whether this is allocation or duplication, for the error message.
void* HandleTableArena::Alloc(const fbl::RefPtr<Dispatcher>& dispatcher, const char* what,
                              uint32_t* base_value) {
  // Attempt to allocate a handle.
  void* addr = arena_.Alloc();
  size_t outstanding_handles = arena_.DiagnosticCount();
  if (unlikely(addr == nullptr)) {
    kcounter_add(handle_count_alloc_failed, 1);
    printf("WARNING: Could not allocate %s handle (%zu outstanding)\n", what, outstanding_handles);
    return nullptr;
  }

  // Emit a warning if too many handles have been created and we haven't recently logged
  if (unlikely(outstanding_handles > kHighHandleCount) && handle_count_high_log_.Ready()) {
    printf("WARNING: High handle count: %zu / %zu handles\n", outstanding_handles,
           kHighHandleCount);
  }

  dispatcher->increment_handle_count();
  // checking the handle_table_id_ and dispatcher_ is really about trying to catch cases where this
  // Handle might somehow already be in use.
  DEBUG_ASSERT(reinterpret_cast<Handle*>(addr)->handle_table_id_ == ZX_KOID_INVALID);
  DEBUG_ASSERT(reinterpret_cast<Handle*>(addr)->dispatcher_ == nullptr);
  *base_value = GetNewBaseValue(addr);
  return addr;
}

HandleOwner Handle::Make(fbl::RefPtr<Dispatcher> dispatcher, zx_rights_t rights) {
  uint32_t base_value;
  void* addr = gHandleTableArena.Alloc(dispatcher, "new", &base_value);
  if (unlikely(!addr))
    return nullptr;
  kcounter_add(handle_count_made, 1);
  kcounter_add(handle_count_live, 1);
  return HandleOwner(new (addr) Handle(ktl::move(dispatcher), rights, base_value));
}

HandleOwner Handle::Make(KernelHandle<Dispatcher> kernel_handle, zx_rights_t rights) {
  uint32_t base_value;
  void* addr = gHandleTableArena.Alloc(kernel_handle.dispatcher(), "new", &base_value);
  if (unlikely(!addr))
    return nullptr;
  kcounter_add(handle_count_made, 1);
  kcounter_add(handle_count_live, 1);
  return HandleOwner(new (addr) Handle(kernel_handle.release(), rights, base_value));
}

// Called only by Make.
Handle::Handle(fbl::RefPtr<Dispatcher> dispatcher, zx_rights_t rights, uint32_t base_value)
    : handle_table_id_(ZX_KOID_INVALID),
      dispatcher_(ktl::move(dispatcher)),
      rights_(rights),
      base_value_(base_value) {}

HandleOwner Handle::Dup(Handle* source, zx_rights_t rights) {
  uint32_t base_value;
  void* addr = gHandleTableArena.Alloc(source->dispatcher(), "duplicate", &base_value);
  if (unlikely(!addr))
    return nullptr;
  kcounter_add(handle_count_duped, 1);
  kcounter_add(handle_count_live, 1);
  return HandleOwner(new (addr) Handle(source, rights, base_value));
}

// Called only by Dup.
Handle::Handle(Handle* rhs, zx_rights_t rights, uint32_t base_value)
    // Although this handle is intended to become owned by rhs->handle_table_id_ at the point of
    // creation it is stack owned and may be destroyed without actually being assigned to the
    // handle table. If this happens the assert in TearDown would get triggered.
    : handle_table_id_(ZX_KOID_INVALID),
      dispatcher_(rhs->dispatcher_),
      rights_(rights),
      base_value_(base_value) {}

void HandleTableArena::Delete(Handle* handle) {
  fbl::RefPtr<Dispatcher> dispatcher(ktl::move(handle->dispatcher_));
  [[maybe_unused]] uint32_t old_base_value = handle->base_value_;
  [[maybe_unused]] const uint32_t* base_value = &handle->base_value_;
  // There may be stale pointers to this slot and they will look at handle_table_id. We expect
  // handle_table_id to already have been cleared by the process dispatcher before the handle got to
  // this point.
  DEBUG_ASSERT(handle->handle_table_id() == ZX_KOID_INVALID);

  if (dispatcher->is_waitable()) {
    dispatcher->Cancel(handle);
  }
  // The destructor should not do anything interesting but call it for completeness.
  handle->~Handle();
  // Make sure the base value was not altered by the destructor.
  DEBUG_ASSERT(*base_value == old_base_value);

  bool zero_handles = dispatcher->decrement_handle_count();
  arena_.Free(handle);

  if (zero_handles) {
    dispatcher->on_zero_handles();
  }

  // If |disp| is the last reference (which is likely) then the dispatcher object
  // gets destroyed at the exit of this function.
  kcounter_add(handle_count_live, -1);
}

Handle* Handle::FromU32(uint32_t value) {
  uint32_t index = HandleValueToIndex(value);
  uintptr_t handle_addr = IndexToHandle(index);
  if (unlikely(!gHandleTableArena.arena_.Committed(reinterpret_cast<void*>(handle_addr))))
    return nullptr;
  handle_addr = gHandleTableArena.arena_.Confine(handle_addr);
  auto handle = reinterpret_cast<Handle*>(handle_addr);
  return reinterpret_cast<Handle*>(
      fbl::conditional_select_nospec_eq(handle->base_value(), value, handle_addr, 0));
}

uint32_t Handle::Count(const fbl::RefPtr<const Dispatcher>& dispatcher) {
  return dispatcher->current_handle_count();
}

uint32_t Handle::Count(const Dispatcher& dispatcher) { return dispatcher.current_handle_count(); }

size_t Handle::diagnostics::OutstandingHandles() {
  return gHandleTableArena.arena_.DiagnosticCount();
}

void Handle::diagnostics::DumpTableInfo() { gHandleTableArena.arena_.Dump(); }

uintptr_t Handle::IndexToHandle(uint32_t index) {
  return reinterpret_cast<uintptr_t>(gHandleTableArena.arena_.Base()) + index * sizeof(Handle);
}

uint32_t HandleTableArena::HandleToIndex(Handle* handle) {
  return static_cast<uint32_t>(handle - reinterpret_cast<Handle*>(arena_.Base()));
}

int64_t HandleTableArena::get_alloc_failed_count() {
  return handle_count_alloc_failed.SumAcrossAllCpus();
}
