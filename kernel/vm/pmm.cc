// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2014 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "vm/pmm.h"

#include <assert.h>
#include <inttypes.h>
#include <lib/boot-options/boot-options.h>
#include <lib/console.h>
#include <lib/counters.h>
#include <lib/ktrace.h>
#include <platform.h>
#include <pow2.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include <new>

#include <fbl/auto_lock.h>
#include <fbl/intrusive_double_list.h>
#include <kernel/mp.h>
#include <kernel/mutex.h>
#include <kernel/timer.h>
#include <ktl/algorithm.h>
#include <lk/init.h>
#include <vm/physmap.h>
#include <vm/pmm_checker.h>
#include <vm/vm.h>

#include "pmm_arena.h"
#include "pmm_node.h"
#include "vm_priv.h"

#if defined(__x86_64__)
#include <arch/x86/feature.h>
#endif

#include <ktl/enforce.h>

#define LOCAL_TRACE VM_GLOBAL_TRACE(0)

// Number of bytes available in the PMM after kernel init, but before userspace init.
KCOUNTER(boot_memory_bytes, "boot.memory.post_init_free_bytes")

// The (currently) one and only pmm node
static PmmNode pmm_node;

// Singleton
static PhysicalPageBorrowingConfig ppb_config;

// Check that if random should wait is requested that this is a debug build with assertions as it is
// currently assumed that enabling this in a non-debug build would be a mistake that should be
// caught.
static void pmm_init_alloc_random_should_wait(uint level) {
  if (gBootOptions->pmm_alloc_random_should_wait) {
    ASSERT(DEBUG_ASSERT_IMPLEMENTED);
    printf("pmm: alloc-random-should-wait enabled\n");
    pmm_node.SeedRandomShouldWait();
  }
}
LK_INIT_HOOK(pmm_init_alloc_random_should_wait, &pmm_init_alloc_random_should_wait,
             LK_INIT_LEVEL_LAST)

static void pmm_fill_free_pages(uint level) { pmm_node.FillFreePagesAndArm(); }
LK_INIT_HOOK(pmm_fill, &pmm_fill_free_pages, LK_INIT_LEVEL_VM)

vm_page_t* paddr_to_vm_page(paddr_t addr) { return pmm_node.PaddrToPage(addr); }

zx_status_t pmm_add_arena(const pmm_arena_info_t* info) { return pmm_node.AddArena(info); }

size_t pmm_num_arenas() { return pmm_node.NumArenas(); }

zx_status_t pmm_get_arena_info(size_t count, uint64_t i, pmm_arena_info_t* buffer,
                               size_t buffer_size) {
  return pmm_node.GetArenaInfo(count, i, buffer, buffer_size);
}

zx_status_t pmm_alloc_page(uint alloc_flags, paddr_t* pa) {
  VM_KTRACE_DURATION(3, "pmm_alloc_page", ("count", 1), ("alloc_flags", alloc_flags));
  return pmm_node.AllocPage(alloc_flags, nullptr, pa);
}

zx_status_t pmm_alloc_page(uint alloc_flags, vm_page_t** page) {
  VM_KTRACE_DURATION(3, "pmm_alloc_page", ("count", 1), ("alloc_flags", alloc_flags));
  return pmm_node.AllocPage(alloc_flags, page, nullptr);
}

zx_status_t pmm_alloc_page(uint alloc_flags, vm_page_t** page, paddr_t* pa) {
  VM_KTRACE_DURATION(3, "pmm_alloc_page", ("count", 1), ("alloc_flags", alloc_flags));
  return pmm_node.AllocPage(alloc_flags, page, pa);
}

zx_status_t pmm_alloc_pages(size_t count, uint alloc_flags, list_node* list) {
  VM_KTRACE_DURATION(3, "pmm_alloc_pages", ("count", count), ("alloc_flags", alloc_flags));
  return pmm_node.AllocPages(count, alloc_flags, list);
}

zx_status_t pmm_alloc_range(paddr_t address, size_t count, list_node* list) {
  VM_KTRACE_DURATION(3, "pmm_alloc_range", ("address", ktrace::Pointer{address}), ("count", count));
  return pmm_node.AllocRange(address, count, list);
}

zx_status_t pmm_alloc_contiguous(size_t count, uint alloc_flags, uint8_t alignment_log2,
                                 paddr_t* pa, list_node* list) {
  VM_KTRACE_DURATION(3, "pmm_alloc_contiguous", ("count", count), ("alloc_flags", alloc_flags));
  // if we're called with a single page, just fall through to the regular allocation routine
  if (unlikely(count == 1 && alignment_log2 <= PAGE_SIZE_SHIFT)) {
    vm_page_t* page;
    zx_status_t status = pmm_node.AllocPage(alloc_flags, &page, pa);
    if (status != ZX_OK) {
      return status;
    }
    list_add_tail(list, &page->queue_node);
    return ZX_OK;
  }

  return pmm_node.AllocContiguous(count, alloc_flags, alignment_log2, pa, list);
}

void pmm_begin_loan(list_node* page_list) {
  VM_KTRACE_DURATION(3, "pmm_begin_loan");
  pmm_node.BeginLoan(page_list);
}

void pmm_cancel_loan(paddr_t address, size_t count) {
  VM_KTRACE_DURATION(3, "pmm_cancel_loan", ("address", ktrace::Pointer{address}), ("count", count));
  pmm_node.CancelLoan(address, count);
}

void pmm_end_loan(paddr_t address, size_t count, list_node* page_list) {
  VM_KTRACE_DURATION(3, "pmm_end_loan", ("address", ktrace::Pointer{address}), ("count", count));
  pmm_node.EndLoan(address, count, page_list);
}

void pmm_delete_lender(paddr_t address, size_t count) {
  VM_KTRACE_DURATION(3, "pmm_delete_lender", ("address", ktrace::Pointer{address}),
                     ("count", count));
  pmm_node.DeleteLender(address, count);
}

void pmm_free(list_node* list) {
  VM_KTRACE_DURATION(3, "pmm_free");
  pmm_node.FreeList(list);
}

void pmm_free_page(vm_page* page) {
  VM_KTRACE_DURATION(3, "pmm_free_page");
  pmm_node.FreePage(page);
}

uint64_t pmm_count_free_pages() { return pmm_node.CountFreePages(); }

uint64_t pmm_count_loaned_free_pages() { return pmm_node.CountLoanedFreePages(); }

uint64_t pmm_count_loaned_used_pages() { return pmm_node.CountLoanedNotFreePages(); }

uint64_t pmm_count_loaned_pages() { return pmm_node.CountLoanedPages(); }

uint64_t pmm_count_loan_cancelled_pages() { return pmm_node.CountLoanCancelledPages(); }

uint64_t pmm_count_total_bytes() { return pmm_node.CountTotalBytes(); }

PageQueues* pmm_page_queues() { return pmm_node.GetPageQueues(); }

Evictor* pmm_evictor() { return pmm_node.GetEvictor(); }

VmCompression* pmm_page_compression() { return pmm_node.GetPageCompression(); }

zx_status_t pmm_set_page_compression(fbl::RefPtr<VmCompression> compression) {
  return pmm_node.SetPageCompression(ktl::move(compression));
}

PhysicalPageBorrowingConfig* pmm_physical_page_borrowing_config() {
  // singleton
  return &ppb_config;
}

bool pmm_set_free_memory_signal(uint64_t free_lower_bound, uint64_t free_upper_bound,
                                uint64_t delay_allocations_pages, Event* event) {
  return pmm_node.SetFreeMemorySignal(free_lower_bound, free_upper_bound, delay_allocations_pages,
                                      event);
}

zx_status_t pmm_wait_till_should_retry_single_alloc(const Deadline& deadline) {
  return pmm_node.WaitTillShouldRetrySingleAlloc(deadline);
}

void pmm_stop_returning_should_wait() { pmm_node.StopReturningShouldWait(); }

void pmm_checker_check_all_free_pages() { pmm_node.CheckAllFreePages(); }

#if __has_feature(address_sanitizer)
void pmm_asan_poison_all_free_pages() { pmm_node.PoisonAllFreePages(); }
#endif

int64_t pmm_get_alloc_failed_count() { return PmmNode::get_alloc_failed_count(); }

bool pmm_has_alloc_failed_no_mem() { return PmmNode::has_alloc_failed_no_mem(); }

static void pmm_checker_enable(size_t fill_size, CheckFailAction action) {
  // Enable filling of pages going forward.
  if (!pmm_node.EnableFreePageFilling(fill_size, action)) {
    printf("Checker already configured, requested fill size and action ignored.\n");
  }

  // From this point on, pages will be filled when they are freed.  However, the free list may still
  // have a lot of unfilled pages so make a pass over them and fill them all.
  pmm_node.FillFreePagesAndArm();

  // All free pages have now been filled with |fill_size| and the checker is armed.
}

static bool pmm_checker_is_enabled() { return pmm_node.Checker()->IsArmed(); }

static void pmm_checker_print_status() { pmm_node.Checker()->PrintStatus(stdout); }

void pmm_checker_init_from_cmdline() {
  bool enabled = false;
  switch (gBootOptions->pmm_checker_enabled) {
    case CheckerEnable::kTrue:
      enabled = true;
      break;
    case CheckerEnable::kFalse:
      enabled = false;
      break;
    case CheckerEnable::kAuto:
#if defined(__x86_64__)
      if (x86_has_hypervisor()) {
        printf("PMM: Checker enabled set to auto and hypervisor detected, disabling\n");
        enabled = false;
      } else {
        printf("PMM: Checker enabled set to auto and hypervisor not detected, enabling\n");
        enabled = true;
      }
#else
      printf(
          "PMM: Checker enabled set to auto on platform without hypervisor detection, disabling\n");
      enabled = false;
#endif
      break;
  }
  if (enabled) {
    size_t fill_size = gBootOptions->pmm_checker_fill_size;
    if (!PmmChecker::IsValidFillSize(fill_size)) {
      printf("PMM: value from %s is invalid (%lu), using PAGE_SIZE instead\n",
             kPmmCheckerFillSizeName.data(), fill_size);
      fill_size = PAGE_SIZE;
    }

    pmm_node.EnableFreePageFilling(fill_size, gBootOptions->pmm_checker_action);
  }
}

static void pmm_dump_timer(Timer* t, zx_time_t now, void*) {
  zx_time_t deadline = zx_time_add_duration(now, ZX_SEC(1));
  t->SetOneshot(deadline, &pmm_dump_timer, nullptr);
  pmm_node.DumpFree();
}

LK_INIT_HOOK(
    pmm_boot_memory,
    [](unsigned int /*level*/) {
      // Track the amount of free memory available in the PMM after kernel init, but before
      // userspace starts.
      //
      // We record this in a kcounter to be tracked by build infrastructure over time.
      dprintf(INFO, "Free memory after kernel init: %" PRIu64 " bytes.\n",
              pmm_node.CountFreePages() * PAGE_SIZE);
      boot_memory_bytes.Set(pmm_node.CountFreePages() * PAGE_SIZE);
    },
    LK_INIT_LEVEL_USER - 1)

static Timer dump_free_mem_timer;

static int cmd_usage(const char* cmd_name, bool is_panic) {
  printf("usage:\n");
  printf("%s dump                                     : dump pmm info \n", cmd_name);
  if (!is_panic) {
    printf("%s free                                     : periodically dump free mem count\n",
           cmd_name);
    printf("%s drop_user_pt                             : drop all user hardware page tables\n",
           cmd_name);
    printf("%s checker status                           : prints the status of the pmm checker\n",
           cmd_name);
    printf(
        "%s checker enable [<size>] [oops|panic]     : enables the pmm checker with optional "
        "fill size and optional action\n",
        cmd_name);
    printf(
        "%s checker check                            : forces a check of all free pages in the "
        "pmm\n",
        cmd_name);
  }
  return ZX_ERR_INTERNAL;
}

static int cmd_pmm(int argc, const cmd_args* argv, uint32_t flags) {
  const bool is_panic = flags & CMD_FLAG_PANIC;
  const char* name = argv[0].str;

  if (argc < 2) {
    printf("not enough arguments\n");
    return cmd_usage(name, is_panic);
  }

  if (!strcmp(argv[1].str, "dump")) {
    pmm_node.Dump(is_panic);
  } else if (is_panic) {
    // No other operations will work during a panic.
    printf("Only the \"arenas\" command is available during a panic.\n");
    return cmd_usage(name, is_panic);
  } else if (!strcmp(argv[1].str, "free")) {
    static bool show_mem = false;

    if (!show_mem) {
      printf("pmm free: issue the same command to stop.\n");
      zx_time_t deadline = zx_time_add_duration(current_time(), ZX_SEC(1));
      const TimerSlack slack{ZX_MSEC(20), TIMER_SLACK_CENTER};
      const Deadline slackDeadline(deadline, slack);
      dump_free_mem_timer.Set(slackDeadline, &pmm_dump_timer, nullptr);
      show_mem = true;
    } else {
      dump_free_mem_timer.Cancel();
      show_mem = false;
    }
  } else if (!strcmp(argv[1].str, "drop_user_pt")) {
    VmAspace::DropAllUserPageTables();
  } else if (!strcmp(argv[1].str, "checker")) {
    if (argc < 3 || argc > 5) {
      return cmd_usage(name, is_panic);
    }
    if (!strcmp(argv[2].str, "status")) {
      pmm_checker_print_status();
    } else if (!strcmp(argv[2].str, "enable")) {
      size_t fill_size = PAGE_SIZE;
      CheckFailAction action = PmmChecker::kDefaultAction;
      if (argc >= 4) {
        fill_size = argv[3].u;
        if (!PmmChecker::IsValidFillSize(fill_size)) {
          printf(
              "error: fill size must be a multiple of 8 and be between 8 and PAGE_SIZE, "
              "inclusive\n");
          return ZX_ERR_INTERNAL;
        }
      }
      if (argc == 5) {
        BootOptions opts;
        if (opts.Parse(argv[4].str, &BootOptions::pmm_checker_action)) {
          action = opts.pmm_checker_action;
        } else {
          printf("error: invalid action\n");
          return ZX_ERR_INTERNAL;
        }
      }
      pmm_checker_enable(fill_size, action);
      // No need to print status as enabling automatically prints status.
    } else if (!strcmp(argv[2].str, "check")) {
      if (!pmm_checker_is_enabled()) {
        printf("error: pmm checker is not enabled\n");
        return ZX_ERR_INTERNAL;
      }
      printf("checking all free pages...\n");
      pmm_checker_check_all_free_pages();
      printf("done\n");
    } else {
      return cmd_usage(name, is_panic);
    }
  } else {
    printf("unknown command\n");

    return cmd_usage(name, is_panic);
  }

  return ZX_OK;
}

void pmm_print_physical_page_borrowing_stats() {
  uint64_t free_pages = pmm_count_free_pages();
  uint64_t loaned_free_pages = pmm_count_loaned_free_pages();
  uint64_t loaned_pages = pmm_count_loaned_pages();
  uint64_t loan_cancelled_pages = pmm_count_loan_cancelled_pages();
  uint64_t total_bytes = pmm_count_total_bytes();
  uint64_t used_loaned_pages = pmm_count_loaned_used_pages();
  printf(
      "PPB stats:\n"
      "  free pages: %" PRIu64 " free MiB: %" PRIu64
      "\n"
      "  loaned free pages: %" PRIu64 " loaned free MiB: %" PRIu64
      "\n"
      "  loaned pages: %" PRIu64 " loaned MiB: %" PRIu64
      "\n"
      "  used loaned pages: %" PRIu64 " used loaned MiB: %" PRIu64
      "\n"
      "  loan cancelled pages: %" PRIu64 " loan cancelled MIB: %" PRIu64
      "\n"
      "  total physical pages: %" PRIu64 " total MiB: %" PRIu64 "\n",
      free_pages, free_pages * PAGE_SIZE / MB, loaned_free_pages,
      loaned_free_pages * PAGE_SIZE / MB, loaned_pages, loaned_pages * PAGE_SIZE / MB,
      used_loaned_pages, used_loaned_pages * PAGE_SIZE / MB, loan_cancelled_pages,
      loan_cancelled_pages * PAGE_SIZE / MB, total_bytes / PAGE_SIZE, total_bytes / MB);
}

void pmm_report_alloc_failure() { pmm_node.ReportAllocFailure(); }

STATIC_COMMAND_START
STATIC_COMMAND_MASKED("pmm", "physical memory manager", &cmd_pmm, CMD_AVAIL_ALWAYS)
STATIC_COMMAND_END(pmm)
