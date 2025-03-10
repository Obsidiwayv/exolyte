// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/llvm-profdata/llvm-profdata.h>
#include <zircon/assert.h>

#include <ktl/byte.h>
#include <ktl/move.h>
#include <ktl/span.h>
#include <object/vm_object_dispatcher.h>
#include <phys/handoff.h>
#include <vm/vm_object_paged.h>

#include "private.h"

#include <ktl/enforce.h>

Handle* MakePhysVmo(const PhysVmo& phys_vmo) {
  ktl::span contents = phys_vmo.data.get();
  if (contents.empty()) {
    return nullptr;
  }

  // Create a VMO to hold the whole dump.
  fbl::RefPtr<VmObjectPaged> vmo;
  uint64_t aligned_size;
  zx_status_t status = VmObject::RoundSize(contents.size_bytes(), &aligned_size);
  ZX_ASSERT(status == ZX_OK);
  status = VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, 0, aligned_size, &vmo);
  ZX_ASSERT(status == ZX_OK);

  status = vmo->Write(contents.data(), 0, contents.size_bytes());
  ZX_ASSERT(status == ZX_OK);

  zx_rights_t rights;
  KernelHandle<VmObjectDispatcher> handle;
  status =
      VmObjectDispatcher::Create(ktl::move(vmo), contents.size_bytes(),
                                 VmObjectDispatcher::InitialMutability::kMutable, &handle, &rights);
  ZX_ASSERT(status == ZX_OK);
  status = handle.dispatcher()->set_name(phys_vmo.name.data(), phys_vmo.name.size());
  DEBUG_ASSERT(status == ZX_OK);
  return Handle::Make(ktl::move(handle), rights & ~ZX_RIGHT_WRITE).release();
}
