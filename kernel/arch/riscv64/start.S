// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <asm.h>
#include <arch/riscv64/mmu.h>
#include <arch/riscv64.h>
#include <arch/defines.h>
#include <arch/kernel_aspace.h>
#include <zircon/tls.h>
#include <arch/code-patches/case-id-asm.h>
#include <lib/code-patching/asm.h>
#include <lib/arch/asm.h>

// This code is purely position-independent and generates no relocations
// that need boot-time fixup.

// Note within this file, the difference between lla and la is subtle when loading the address
// of a symbol.
// - lla will use PC-relative instructions and thus will compute the physical address of the
// label since the code is running in physical space.
// - la will emit a literal pool load from a hidden symbol and thus will load the proper
// virtual address of the label, since the kernel has already been relocated by physboot
// by the time we get here.
.text
FUNCTION(_start)
.globl PhysbootHandoff
PhysbootHandoff = _start
    // collect the starting time stamp
    rdtime a2

    // This serves as a verification that code-patching was performed before
    // the kernel was booted; if unpatched, we would trap here and halt.
    .code_patching.start CASE_ID_SELF_TEST
    unimp
    .code_patching.end

    // save a0 (physboot handoff paddr) for later passing to lk_main
    mv      s0, a0

    // save the time stamp we recorded earlier
    lla     t0, kernel_entry_ticks
    sd      a2, (t0)

    // save the physical address the kernel is loaded at
    lla     a0, __executable_start
    lla     t0, kernel_base_phys
    sd      a0, (t0)

    // set the default stack
    lla     sp, boot_cpu_kstack_end

#if __has_feature(shadow_call_stack)
    lla     shadow_call_sp, boot_shadow_call_stack
#endif

    // call into the riscv64 boot mapping code to give it a chance to initialize its page tables
    la      a0, __executable_start // virtual
    lla     a1, __executable_start // physical
    sub     a0, a0, a1 // compute the delta between the virtual and physical address the kernel is at
    call    riscv64_boot_map_init

    // void riscv64_boot_map(pte_t* kernel_table0, vaddr_t vaddr, paddr_t paddr, size_t len, pte_t flags);

    // Set up an identity map for bootstrapping the mmu now and later when secondary cores are initialized.
    // Global pages are explicitly not used here since it's within the user address space range and
    // we dont want any stale TLB entries when context switching to user space later.
    lla     a0, riscv64_kernel_bootstrap_translation_table
    li      a1, 0
    li      a2, 0
    li      a3, ARCH_PHYSMAP_SIZE
    li      a4, RISCV64_PTE_A | RISCV64_PTE_D | RISCV64_PTE_PERM_MASK | RISCV64_PTE_V
    call    riscv64_boot_map

    // map a large run of physical memory at the base of the kernel's address space
    lla     a0, riscv64_kernel_bootstrap_translation_table
    li      a1, KERNEL_ASPACE_BASE
    li      a2, 0
    li      a3, ARCH_PHYSMAP_SIZE
    li      a4, RISCV64_PTE_G | RISCV64_PTE_A | RISCV64_PTE_D | RISCV64_PTE_R | RISCV64_PTE_W | RISCV64_PTE_V
    call    riscv64_boot_map

    // map the kernel to a fixed address
    // note: mapping the kernel here with full rwx, this will get locked down later in vm initialization
    lla     a0, riscv64_kernel_bootstrap_translation_table
    la      a1, __executable_start
    lla     a2, __executable_start
    lla     a3, _end
    sub     a3, a3, a2
    li      a4, RISCV64_PTE_G | RISCV64_PTE_A | RISCV64_PTE_D | RISCV64_PTE_PERM_MASK | RISCV64_PTE_V
    call    riscv64_boot_map

    // ensure it's written out
    fence   w,w

    // set the satp register and enable the mmu
    // ASID 0, riscv64_kernel_translation_table address
    lla     t0, riscv64_kernel_bootstrap_translation_table
    srli    t1, t0, PAGE_SIZE_SHIFT
    li      t2, (RISCV64_SATP_MODE_SV39 << RISCV64_SATP_MODE_SHIFT)
    or      t1, t1, t2
    csrw    satp, t1

    // global tlb fence
    sfence.vma  zero, zero

    // mmu is initialized and we're running out of an identity physical map

    // compute the delta between the old physical and newer high addresses
    lla     t0, _start
    la      t1, _start
    sub     t0, t1, t0

    // fix up the stack pointer
    add     sp, sp, t0

    // fix up the shadow call pointer
#if __has_feature(shadow_call_stack)
    add     shadow_call_sp, shadow_call_sp, t0
#endif

    // jump to high memory
    lla     t1, .Lmmu_on_vaddr
    add     t1, t1, t0
    jr      t1

.Lmmu_on_vaddr:
    // Run the boot cpu init routine with the boot hart id.
    // This will do basic initialization of the cpu such as initializing
    // the main control registers and loading the exception vector table.
    // Also loads the per cpu register.
    mv      a0, s0
    call    riscv64_boot_cpu_init

    // save the time stamp just before entering C
    rdtime  a0
    lla     a1, kernel_virtual_entry_ticks
    sd      a0, (a1)

    // recover the zbi pointer
    mv      a0, s0

    // Call main
    call    lk_main

    // should never return here
    j       .
END_FUNCTION(_start)

// Entry point for secondary cpus when started from SBI
FUNCTION(riscv64_secondary_entry_asm)
    // Enable the MMU with the ASID 0, prefilled by _start
    lla     t0, riscv64_kernel_bootstrap_translation_table
    srli    t1, t0, PAGE_SIZE_SHIFT
    li      t2, (RISCV64_SATP_MODE_SV39 << RISCV64_SATP_MODE_SHIFT)
    or      t1, t1, t2
    csrw    satp, t1

    // global tlb fence
    sfence.vma  zero, zero

    // Compute the relocation offset
    la      t1, __executable_start
    lla     t0, __executable_start
    sub     t0, t1, t0

    // Jump to high memory
    lla     t1, .Lmmu_on_vaddr_secondary
    add     t1, t1, t0
    jr      t1

.Lmmu_on_vaddr_secondary:
    // SBI is kind enough to give us a private parameter in a1, we fill it with
    // the stack pointer for this hart
    mv      sp, a1

    // Read  the cpu number off of the stack
    ld      a1, -8(sp)

    // Set up the shadow call stack
#if __has_feature(shadow_call_stack)
    ld      shadow_call_sp, -16(sp)
#endif

    // The identity mapping is still there, we can jump to C.
    // The hart id is already in a0.
    call    riscv64_secondary_entry

    // should never return here
    j       .
END_FUNCTION(riscv64_secondary_entry_asm)


.bss
LOCAL_DATA(boot_cpu_kstack)
    .skip ARCH_DEFAULT_STACK_SIZE
    .balign 16
LOCAL_DATA(boot_cpu_kstack_end)
END_DATA(boot_cpu_kstack)
LOCAL_DATA(boot_shadow_call_stack)
    .skip PAGE_SIZE
END_DATA(boot_shadow_call_stack)

// This symbol is used by gdb python to know the base of the kernel module
.global KERNEL_BASE_ADDRESS
KERNEL_BASE_ADDRESS = KERNEL_BASE
