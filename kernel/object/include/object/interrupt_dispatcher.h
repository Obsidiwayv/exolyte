// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_INTERRUPT_DISPATCHER_H_
#define ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_INTERRUPT_DISPATCHER_H_

#include <lib/zircon-internal/thread_annotations.h>
#include <sys/types.h>
#include <zircon/rights.h>
#include <zircon/types.h>

#include <kernel/event.h>
#include <kernel/idle_power_thread.h>
#include <kernel/spinlock.h>
#include <object/dispatcher.h>
#include <object/port_dispatcher.h>

enum class InterruptState {
  WAITING = 0,
  DESTROYED = 1,
  TRIGGERED = 2,
  NEEDACK = 3,
  IDLE = 4,
};

// Note that unlike most Dispatcher subclasses, this one is further
// subclassed, and so cannot be final.
class InterruptDispatcher
    : public SoloDispatcher<InterruptDispatcher, ZX_DEFAULT_INTERRUPT_RIGHTS> {
 public:
  InterruptDispatcher& operator=(const InterruptDispatcher&) = delete;
  zx_obj_type_t get_type() const final { return ZX_OBJ_TYPE_INTERRUPT; }
  uint32_t get_flags() const { return flags_; }

  zx_status_t WaitForInterrupt(zx_time_t* out_timestamp);
  zx_status_t Trigger(zx_time_t timestamp);
  zx_status_t Ack();
  zx_status_t Destroy();
  void InterruptHandler();
  zx_status_t Bind(fbl::RefPtr<PortDispatcher> port_dispatcher, uint64_t key);
  zx_status_t Unbind(fbl::RefPtr<PortDispatcher> port_dispatcher);

  void on_zero_handles() final;

 protected:
  virtual void MaskInterrupt() = 0;
  virtual void UnmaskInterrupt() = 0;
  virtual void DeactivateInterrupt() = 0;
  virtual void UnregisterInterruptHandler() = 0;

  InterruptDispatcher();
  void Signal() { event_.Signal(); }
  zx_status_t set_flags(uint32_t flags);
  bool SendPacketLocked(zx_time_t timestamp) TA_REQ(spinlock_);

  // Bits for Interrupt.flags
  // The interrupt is virtual.
  static constexpr uint32_t INTERRUPT_VIRTUAL = (1u << 0);
  // The interrupt should be unmasked before waiting on the event.
  static constexpr uint32_t INTERRUPT_UNMASK_PREWAIT = (1u << 1);
  // The same as |INTERRUPT_UNMASK_PREWAIT| except release the dispatcher
  // spinlock before waiting.
  static constexpr uint32_t INTERRUPT_UNMASK_PREWAIT_UNLOCKED = (1u << 2);
  // The interrupt should be masked following waiting.
  static constexpr uint32_t INTERRUPT_MASK_POSTWAIT = (1u << 4);
  // The interrupt may wake the system from suspend.
  static constexpr uint32_t INTERRUPT_WAKE_VECTOR = (1u << 5);
  // Allow kernel tests to call Ack() without binding to a port.
  static constexpr uint32_t INTERRUPT_ALLOW_ACK_WITHOUT_PORT_FOR_TEST = (1u << 6);

 private:
  AutounsignalEvent event_;
  // Interrupt Flags
  uint32_t flags_;

  zx_time_t timestamp_ TA_GUARDED(spinlock_);
  // Current state of the interrupt object
  InterruptState state_ TA_GUARDED(spinlock_);
  PortInterruptPacket port_packet_ TA_GUARDED(spinlock_) = {};
  fbl::RefPtr<PortDispatcher> port_dispatcher_ TA_GUARDED(spinlock_);
  IdlePowerThread::WakeEvent wake_event_ TA_GUARDED(spinlock_);

  // Controls the access to Interrupt properties
  DECLARE_SPINLOCK(InterruptDispatcher) spinlock_;
};

#endif  // ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_INTERRUPT_DISPATCHER_H_
