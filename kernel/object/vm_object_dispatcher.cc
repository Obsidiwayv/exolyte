// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "object/vm_object_dispatcher.h"

#include <assert.h>
#include <inttypes.h>
#include <lib/boot-options/boot-options.h>
#include <lib/counters.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/rights.h>

#include <fbl/alloc_checker.h>
#include <ktl/algorithm.h>
#include <ktl/atomic.h>
#include <ktl/optional.h>
#include <vm/page_source.h>
#include <vm/vm_aspace.h>
#include <vm/vm_object.h>
#include <vm/vm_object_paged.h>

#include <ktl/enforce.h>

#define LOCAL_TRACE 0

KCOUNTER(dispatcher_vmo_create_count, "dispatcher.vmo.create")
KCOUNTER(dispatcher_vmo_destroy_count, "dispatcher.vmo.destroy")

zx::result<VmObjectDispatcher::CreateStats> VmObjectDispatcher::parse_create_syscall_flags(
    uint32_t flags, size_t size) {
  CreateStats res = {0, size};

  if (flags & ZX_VMO_RESIZABLE) {
    if (flags & ZX_VMO_UNBOUNDED) {
      return zx::error(ZX_ERR_INVALID_ARGS);
    }
    res.flags |= VmObjectPaged::kResizable;
    flags &= ~ZX_VMO_RESIZABLE;
  }
  if (flags & ZX_VMO_DISCARDABLE) {
    res.flags |= VmObjectPaged::kDiscardable;
    flags &= ~ZX_VMO_DISCARDABLE;
  }
  if (flags & ZX_VMO_UNBOUNDED) {
    flags &= ~ZX_VMO_UNBOUNDED;
    res.size = VmObjectPaged::max_size();
  } else {
    if (zx_status_t status = VmObject::RoundSize(size, &res.size); status != ZX_OK) {
      return zx::error(status);
    }
  }

  if (flags) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  return zx::ok(res);
}

zx_status_t VmObjectDispatcher::CreateWithCsm(fbl::RefPtr<VmObject> vmo,
                                              fbl::RefPtr<ContentSizeManager> content_size_manager,
                                              zx_koid_t pager_koid,
                                              InitialMutability initial_mutability,
                                              KernelHandle<VmObjectDispatcher>* handle,
                                              zx_rights_t* rights) {
  fbl::AllocChecker ac;
  KernelHandle new_handle(fbl::AdoptRef(new (&ac) VmObjectDispatcher(
      ktl::move(vmo), ktl::move(content_size_manager), pager_koid, initial_mutability)));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  new_handle.dispatcher()->vmo()->set_user_id(new_handle.dispatcher()->get_koid());
  *rights =
      default_rights() | (new_handle.dispatcher()->vmo()->is_resizable() ? ZX_RIGHT_RESIZE : 0);
  *handle = ktl::move(new_handle);
  return ZX_OK;
}

zx_status_t VmObjectDispatcher::Create(fbl::RefPtr<VmObject> vmo, uint64_t content_size,
                                       zx_koid_t pager_koid, InitialMutability initial_mutability,
                                       KernelHandle<VmObjectDispatcher>* handle,
                                       zx_rights_t* rights) {
  fbl::RefPtr<ContentSizeManager> content_size_manager;
  zx_status_t status = ContentSizeManager::Create(content_size, &content_size_manager);
  if (status != ZX_OK) {
    return status;
  }
  return CreateWithCsm(ktl::move(vmo), ktl::move(content_size_manager), pager_koid,
                       initial_mutability, handle, rights);
}

VmObjectDispatcher::VmObjectDispatcher(fbl::RefPtr<VmObject> vmo,
                                       fbl::RefPtr<ContentSizeManager> content_size_manager,
                                       zx_koid_t pager_koid, InitialMutability initial_mutability)
    : SoloDispatcher(ZX_VMO_ZERO_CHILDREN),
      vmo_(ktl::move(vmo)),
      content_size_mgr_(ktl::move(content_size_manager)),
      pager_koid_(pager_koid),
      initial_mutability_(initial_mutability) {
  kcounter_add(dispatcher_vmo_create_count, 1);
  vmo_->SetChildObserver(this);
}

VmObjectDispatcher::~VmObjectDispatcher() {
  kcounter_add(dispatcher_vmo_destroy_count, 1);
  // Intentionally leave vmo_->user_id() set to our koid even though we're
  // dying and the koid will no longer map to a Dispatcher. koids are never
  // recycled, and it could be a useful breadcrumb.
}

void VmObjectDispatcher::OnZeroChild() {
  Guard<CriticalMutex> guard{get_lock()};
  // Double check the number of children now that we are holding the dispatcher lock and so are
  // serialized with our calls to CreateChild. This allows us to atomically observer that there are
  // indeed zero children, and then set the signal. The double check is needed since OnZeroChild is
  // called without the VMO lock held, and so there is a race where before we could acquire the
  // dispatcher lock a new child got created.
  if (vmo_->num_children() == 0) {
    UpdateStateLocked(0, ZX_VMO_ZERO_CHILDREN);
  }
}

zx_status_t VmObjectDispatcher::get_name(char (&out_name)[ZX_MAX_NAME_LEN]) const {
  canary_.Assert();
  vmo_->get_name(out_name, ZX_MAX_NAME_LEN);
  return ZX_OK;
}

zx_status_t VmObjectDispatcher::set_name(const char* name, size_t len) {
  canary_.Assert();
  return vmo_->set_name(name, len);
}

void VmObjectDispatcher::on_zero_handles() {
  // Clear when handle count reaches zero rather in the destructor because we're retaining a
  // VmObject that might call back into |this| via VmObjectChildObserver when it's destroyed.
  vmo_->SetChildObserver(nullptr);
}

zx_status_t VmObjectDispatcher::Read(user_out_ptr<char> user_data, size_t length, uint64_t offset,
                                     size_t* out_actual) {
  canary_.Assert();

  return vmo_->ReadUser(user_data, offset, length, VmObjectReadWriteOptions::None, out_actual);
}

zx_status_t VmObjectDispatcher::ReadVector(user_out_iovec_t user_data, size_t length,
                                           uint64_t offset, size_t* out_actual) {
  canary_.Assert();

  return vmo_->ReadUserVector(user_data, offset, length, out_actual);
}

zx_status_t VmObjectDispatcher::WriteVector(
    user_in_iovec_t user_data, size_t length, uint64_t offset, size_t* out_actual,
    VmObject::OnWriteBytesTransferredCallback on_bytes_transferred) {
  canary_.Assert();

  return vmo_->WriteUserVector(user_data, offset, length, out_actual, on_bytes_transferred);
}

zx_status_t VmObjectDispatcher::Write(
    user_in_ptr<const char> user_data, size_t length, uint64_t offset, size_t* out_actual,
    VmObject::OnWriteBytesTransferredCallback on_bytes_transferred) {
  canary_.Assert();

  return vmo_->WriteUser(user_data, offset, length, VmObjectReadWriteOptions::None, out_actual,
                         on_bytes_transferred);
}

zx_status_t VmObjectDispatcher::SetSize(uint64_t size) {
  canary_.Assert();

  ContentSizeManager::Operation op;
  Guard<Mutex> guard{content_size_mgr_->lock()};

  content_size_mgr_->BeginSetContentSizeLocked(size, &op, &guard);

  uint64_t size_aligned = ROUNDUP(size, PAGE_SIZE);
  // Check for overflow when rounding up.
  if (size_aligned < size) {
    op.AssertParentLockHeld();
    op.CancelLocked();
    return ZX_ERR_OUT_OF_RANGE;
  }

  zx_status_t status = vmo_->Resize(size_aligned);
  if (status != ZX_OK) {
    op.AssertParentLockHeld();
    op.CancelLocked();
    return status;
  }

  uint64_t remaining = size_aligned - size;
  if (remaining > 0) {
    // TODO(https://fxbug.dev/42053728): Determine whether failure to ZeroRange here should undo
    // this operation.
    //
    // Dropping the lock here is fine, as an `Operation` only needs to be locked when initializing,
    // committing, or cancelling.
    guard.CallUnlocked([&] { vmo_->ZeroRange(size, remaining); });
  }

  op.AssertParentLockHeld();
  op.CommitLocked();
  return status;
}

zx_status_t VmObjectDispatcher::GetSize(uint64_t* size) {
  canary_.Assert();

  *size = vmo_->size();

  return ZX_OK;
}

zx_info_vmo_t VmoToInfoEntry(const VmObject* vmo, VmoOwnership ownership,
                             zx_rights_t handle_rights) {
  zx_info_vmo_t entry = {};
  entry.koid = vmo->user_id();
  vmo->get_name(entry.name, sizeof(entry.name));
  entry.size_bytes = vmo->size();
  entry.parent_koid = vmo->parent_user_id();
  entry.num_children = vmo->num_children();
  entry.num_mappings = vmo->num_mappings();
  entry.share_count = vmo->share_count();
  entry.flags = (vmo->is_paged() ? ZX_INFO_VMO_TYPE_PAGED : ZX_INFO_VMO_TYPE_PHYSICAL) |
                (vmo->is_resizable() ? ZX_INFO_VMO_RESIZABLE : 0) |
                (vmo->is_discardable() ? ZX_INFO_VMO_DISCARDABLE : 0) |
                (vmo->is_user_pager_backed() ? ZX_INFO_VMO_PAGER_BACKED : 0) |
                (vmo->is_contiguous() ? ZX_INFO_VMO_CONTIGUOUS : 0);
  // As an implementation detail, both ends of an IOBuffer keep a child reference to a shared parent
  // which is dropped. Since references aren't normally attributed memory otherwise, we specifically
  // request their attribution counts.
  VmObject::AttributionCounts counts = ownership == VmoOwnership::kIoBuffer
                                           ? vmo->GetAttributedMemoryInReferenceOwner()
                                           : vmo->GetAttributedMemory();
  entry.committed_bytes = counts.uncompressed_bytes;
  entry.populated_bytes = counts.compressed_bytes + counts.uncompressed_bytes;
  entry.cache_policy = vmo->GetMappingCachePolicy();
  switch (ownership) {
    case VmoOwnership::kHandle:
      entry.flags |= ZX_INFO_VMO_VIA_HANDLE;
      entry.handle_rights = handle_rights;
      break;
    case VmoOwnership::kMapping:
      entry.flags |= ZX_INFO_VMO_VIA_MAPPING;
      break;
    case VmoOwnership::kIoBuffer:
      entry.flags |= ZX_INFO_VMO_VIA_IOB_HANDLE;
      entry.handle_rights = handle_rights;
      break;
  }
  if (vmo->child_type() == VmObject::ChildType::kCowClone) {
    entry.flags |= ZX_INFO_VMO_IS_COW_CLONE;
  }
  entry.metadata_bytes = vmo->HeapAllocationBytes();
  // Only events that change committed pages are different kinds of reclamation.
  entry.committed_change_events = vmo->ReclamationEventCount();
  return entry;
}

zx_info_vmo_t VmObjectDispatcher::GetVmoInfo(zx_rights_t rights) {
  zx_info_vmo_t info = VmoToInfoEntry(vmo().get(), VmoOwnership::kHandle, rights);
  if (initial_mutability_ == InitialMutability::kImmutable) {
    info.flags |= ZX_INFO_VMO_IMMUTABLE;
  }
  return info;
}

zx_status_t VmObjectDispatcher::SetContentSize(uint64_t content_size) {
  canary_.Assert();

  ContentSizeManager::Operation op;
  Guard<Mutex> guard{content_size_mgr_->lock()};
  content_size_mgr_->BeginSetContentSizeLocked(content_size, &op, &guard);

  uint64_t vmo_size = vmo_->size();
  if (content_size < vmo_size) {
    // TODO(https://fxbug.dev/42053728): Determine whether failure to ZeroRange here should undo
    // this operation.
    //
    // Dropping the lock here is fine, as an `Operation` only needs to be locked when initializing,
    // committing, or cancelling.
    guard.CallUnlocked([&] { vmo_->ZeroRange(content_size, vmo_size - content_size); });
  }

  op.AssertParentLockHeld();
  op.CommitLocked();
  return ZX_OK;
}

uint64_t VmObjectDispatcher::GetContentSize() const {
  canary_.Assert();

  return content_size_mgr_->GetContentSize();
}

zx_status_t VmObjectDispatcher::ExpandIfNecessary(uint64_t requested_vmo_size, bool can_resize_vmo,
                                                  uint64_t* out_actual) {
  canary_.Assert();

  uint64_t current_vmo_size = vmo_->size();
  *out_actual = current_vmo_size;

  uint64_t required_vmo_size = ROUNDUP(requested_vmo_size, PAGE_SIZE);
  // Overflow when rounding up.
  if (required_vmo_size < requested_vmo_size) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  if (required_vmo_size > current_vmo_size) {
    if (!can_resize_vmo) {
      return ZX_ERR_NOT_SUPPORTED;
    }
    zx_status_t status = vmo_->Resize(required_vmo_size);
    if (status != ZX_OK) {
      // Resizing failed but the rest of the current VMO size can be used.
      return status;
    }

    *out_actual = required_vmo_size;
  }

  return ZX_OK;
}

zx_status_t VmObjectDispatcher::RangeOp(uint32_t op, uint64_t offset, uint64_t size,
                                        user_inout_ptr<void> buffer, size_t buffer_size,
                                        zx_rights_t rights) {
  canary_.Assert();

  LTRACEF("op %u offset %#" PRIx64 " size %#" PRIx64 " buffer %p buffer_size %zu rights %#x\n", op,
          offset, size, buffer.get(), buffer_size, rights);

  switch (op) {
    case ZX_VMO_OP_COMMIT: {
      if ((rights & ZX_RIGHT_WRITE) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      // TODO: handle partial commits
      auto status = vmo_->CommitRange(offset, size);
      return status;
    }
    case ZX_VMO_OP_DECOMMIT: {
      if ((rights & ZX_RIGHT_WRITE) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      // TODO: handle partial decommits
      auto status = vmo_->DecommitRange(offset, size);
      return status;
    }
    case ZX_VMO_OP_LOCK: {
      if ((rights & (ZX_RIGHT_READ | ZX_RIGHT_WRITE)) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }

      zx_vmo_lock_state_t lock_state = {};
      zx_status_t status = vmo_->LockRange(offset, size, &lock_state);
      if (status != ZX_OK) {
        return status;
      }
      // If an error is encountered from this point on, the lock operation MUST be reverted
      // before returning.

      if (buffer_size < sizeof(zx_vmo_lock_state_t)) {
        // Undo the lock before returning an error.
        vmo_->UnlockRange(offset, size);
        return ZX_ERR_INVALID_ARGS;
      }

      auto lock_state_out = buffer.reinterpret<zx_vmo_lock_state_t>();
      if ((status = lock_state_out.copy_to_user(lock_state)) != ZX_OK) {
        // Undo the lock before returning an error.
        vmo_->UnlockRange(offset, size);
        return status;
      }

      return status;
    }
    case ZX_VMO_OP_TRY_LOCK:
      if ((rights & (ZX_RIGHT_READ | ZX_RIGHT_WRITE)) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      return vmo_->TryLockRange(offset, size);
    case ZX_VMO_OP_UNLOCK:
      if ((rights & (ZX_RIGHT_READ | ZX_RIGHT_WRITE)) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      return vmo_->UnlockRange(offset, size);
    case ZX_VMO_OP_CACHE_SYNC:
      if ((rights & ZX_RIGHT_READ) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      return vmo_->CacheOp(offset, size, VmObject::CacheOpType::Sync);
    case ZX_VMO_OP_CACHE_INVALIDATE:
      if (!gBootOptions->enable_debugging_syscalls) {
        return ZX_ERR_NOT_SUPPORTED;
      }
      // A straight invalidate op requires the write right since
      // it may drop dirty cache lines, thus modifying the contents
      // of the VMO.
      if ((rights & ZX_RIGHT_WRITE) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      return vmo_->CacheOp(offset, size, VmObject::CacheOpType::Invalidate);
    case ZX_VMO_OP_CACHE_CLEAN:
      if ((rights & ZX_RIGHT_READ) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      return vmo_->CacheOp(offset, size, VmObject::CacheOpType::Clean);
    case ZX_VMO_OP_CACHE_CLEAN_INVALIDATE:
      if ((rights & ZX_RIGHT_READ) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      return vmo_->CacheOp(offset, size, VmObject::CacheOpType::CleanInvalidate);
    case ZX_VMO_OP_ZERO:
      if ((rights & ZX_RIGHT_WRITE) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      return vmo_->ZeroRange(offset, size);
    case ZX_VMO_OP_ALWAYS_NEED:
      return vmo_->HintRange(offset, size, VmObject::EvictionHint::AlwaysNeed);
    case ZX_VMO_OP_DONT_NEED:
      return vmo_->HintRange(offset, size, VmObject::EvictionHint::DontNeed);
    case ZX_VMO_OP_PREFETCH:
      if ((rights & ZX_RIGHT_READ) == 0) {
        return ZX_ERR_ACCESS_DENIED;
      }
      return vmo_->PrefetchRange(offset, size);
    default:
      return ZX_ERR_INVALID_ARGS;
  }
}

zx_status_t VmObjectDispatcher::SetMappingCachePolicy(uint32_t cache_policy) {
  return vmo_->SetMappingCachePolicy(cache_policy);
}

zx_status_t VmObjectDispatcher::CreateChildInternal(uint32_t options, uint64_t offset,
                                                    uint64_t size, bool copy_name,
                                                    fbl::RefPtr<VmObject>* child_vmo) {
  // Clones are not supported for discardable VMOs.
  if (vmo_->is_discardable()) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (options & ZX_VMO_CHILD_SLICE) {
    // No other flags are valid for slices.
    options &= ~ZX_VMO_CHILD_SLICE;
    if (options) {
      return ZX_ERR_INVALID_ARGS;
    }
    return vmo_->CreateChildSlice(offset, size, copy_name, child_vmo);
  }

  Resizability resizable = Resizability::NonResizable;
  if (options & ZX_VMO_CHILD_REFERENCE) {
    options &= ~ZX_VMO_CHILD_REFERENCE;
    if (options & ZX_VMO_CHILD_RESIZABLE) {
      resizable = Resizability::Resizable;
      options &= ~ZX_VMO_CHILD_RESIZABLE;
    }
    if (options) {
      return ZX_ERR_INVALID_ARGS;
    }
    return vmo_->CreateChildReference(resizable, offset, size, copy_name, nullptr, child_vmo);
  }

  // Check for mutually-exclusive child type flags.
  CloneType type;
  if (options & ZX_VMO_CHILD_SNAPSHOT) {
    options &= ~ZX_VMO_CHILD_SNAPSHOT;
    type = CloneType::Snapshot;
  } else if (options & ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE) {
    options &= ~ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE;
    type = CloneType::SnapshotAtLeastOnWrite;
  } else if (options & ZX_VMO_CHILD_SNAPSHOT_MODIFIED) {
    options &= ~ZX_VMO_CHILD_SNAPSHOT_MODIFIED;
    type = CloneType::SnapshotModified;
  } else {
    return ZX_ERR_INVALID_ARGS;
  }

  if (options & ZX_VMO_CHILD_RESIZABLE) {
    resizable = Resizability::Resizable;
    options &= ~ZX_VMO_CHILD_RESIZABLE;
  }

  if (options)
    return ZX_ERR_INVALID_ARGS;

  return vmo_->CreateClone(resizable, type, offset, size, copy_name, child_vmo);
}

zx_status_t VmObjectDispatcher::CreateChild(uint32_t options, uint64_t offset, uint64_t size,
                                            bool copy_name, fbl::RefPtr<VmObject>* child_vmo) {
  canary_.Assert();

  LTRACEF("options 0x%x offset %#" PRIx64 " size %#" PRIx64 "\n", options, offset, size);

  // To synchronize the zero child signal the dispatcher lock is used over the create child path to
  // ensure we can atomically know that a child exists, and clear the zero child signal.
  Guard<CriticalMutex> guard{get_lock()};
  zx_status_t status = CreateChildInternal(options, offset, size, copy_name, child_vmo);
  DEBUG_ASSERT((status == ZX_OK) == (*child_vmo != nullptr));
  if (status == ZX_OK) {
    // We know definitively that a child exists, as we have a refptr to it, and so we can clear the
    // zero children signal.
    UpdateStateLocked(ZX_VMO_ZERO_CHILDREN, 0);
  }
  return status;
}
