// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// fastmem1: the AOT runtime's private 4 GiB guest window and its fault handler
// (see aot_runtime.h, AotFastMem.base and the `_fmw` helpers).
//
// Deliberately free of Dolphin dependencies (only aot_runtime.h and the C/Mach
// runtime): the ROM-free test Tools/aot/test-fastmem.py compiles this file
// together with generated code and its own aot_*_slow stubs. AotRuntime.cpp
// wraps it for Dolphin (logging, AOTSTAT).
//
// Fault handling runs ON THE FAULTING THREAD: a process-wide SIGBUS/SIGSEGV
// handler (sigaction), not Dolphin's Apple Mach exception port, whose handler
// runs on a separate exception thread (the slow helpers must run on the CPU
// thread: CoreTiming/IsCPUThread, EFB peeks) and costs ~0.6 ms per resumed
// fault in the VM (5 us for the signal). Core.cpp therefore does not install
// the Mach port for AOT sessions: a thread port that declines every fault
// would cost ~150 us before the signal is delivered.

#pragma once

#include <stdint.h>

#include "Core/PowerPC/AOT/aot_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

// One decoded window access (the eight `_fmw` instruction forms).
typedef struct AotFastmemAccess
{
  uint32_t size;   // 1, 2, 4 or 8 bytes
  uint32_t store;  // 0 = load, 1 = store
  uint32_t rt;     // data register (31 = zr)
  uint32_t rn;     // base register
  uint32_t rm;     // index register (the 32-bit guest effective address)
} AotFastmemAccess;

// 1 when `insn` is one of the eight `_fmw` forms (LDRB/LDRH/LDR W/LDR X/STRB/
// STRH/STR W/STR X, register offset, UXTW, no shift), with *out filled.
int aot_fastmem_decode(uint32_t insn, AotFastmemAccess* out);

// Result of aot_fastmem_window_create.
typedef struct AotFastmemInit
{
  uint8_t* base;          // window base, NULL on any failure
  int reserve_kr;         // vm_allocate result (0 = KERN_SUCCESS)
  int protect_kr;         // vm_protect result
  // Per alias: 1 = mapped and verified both ways, 0 = failed, -1 = not mapped
  // (no buffer). Order: MEM1 0x80, MEM1 0xC0, MEM2 0x90, MEM2 0xD0, L1 0xE0.
  int alias[5];
  int handler_installed;  // sigaction succeeded
  int selftest_faults;    // template faults the handler resumed (8 = all)
  int selftest_ok;        // every self-test value round-tripped
  double fault_us;        // mean wall time per self-test fault
  char error[160];        // first failure, empty on success
} AotFastmemInit;

// Reserves the 4 GiB window (PROT_NONE), aliases the guest buffers into it,
// installs the fault handler and runs the eight-template self-test (faults at
// base+0x1000 that never reach emulator state). `s` is what the handler passes
// to the aot_*_slow helpers. On any failure everything is undone and
// out->base is NULL. ram_size/exram_size/l1_size must be page multiples
// (exram/l1 may be NULL/0). Returns out->base != NULL.
int aot_fastmem_window_create(AOTState* s, uint8_t* ram, uint32_t ram_size, uint8_t* exram,
                              uint32_t exram_size, uint8_t* l1, uint32_t l1_size,
                              AotFastmemInit* out);
// fastmem probe (Phase B): the steps of aot_fastmem_window_create, each with
// its kern_return/errno and wall time, then either keeping the window (keep=1:
// the loaded library needs it; the probe IS the init) or tearing it down
// again (keep=0: deallocate + restore the previous SIGBUS/SIGSEGV dispositions,
// so a session whose library never touches the window keeps today's process
// state). Answers on a device: can this process reserve 4 GiB + guard, do the
// vm_remap aliases work, is the signal handler invoked and does it resume.
enum
{
  AOT_FMP_STEP_NONE = 0,
  AOT_FMP_STEP_ARGS = 1,      // guest buffers missing / not page-sized
  AOT_FMP_STEP_RESERVE = 2,   // vm_allocate(4 GiB + guard)
  AOT_FMP_STEP_PROTECT = 3,   // vm_protect(PROT_NONE)
  AOT_FMP_STEP_ALIAS = 4,     // vm_remap + both-way verify (failed_alias says which)
  AOT_FMP_STEP_HANDLER = 5,   // sigaction(SIGBUS/SIGSEGV)
  AOT_FMP_STEP_SELFTEST = 6,  // the eight templates through the fault handler
  AOT_FMP_STEP_UNSUPPORTED = 7
};
typedef struct AotFastmemProbeResult
{
  AotFastmemInit init;  // as aot_fastmem_window_create fills it (base NULL unless kept)
  uintptr_t window_addr;  // where the window was reserved (0 = never), also after teardown
  int keep;             // requested mode: 1 = keep the window, 0 = tear it down
  int failed_step;      // AOT_FMP_STEP_* of the first failure, 0 = window came up
  // kern_return (reserve/protect/alias), errno (handler), faults resumed
  // (selftest). Alias failures that are not a kern_return: -2 = remapped at
  // the wrong address or without read/write, -3 = both-way verify mismatch.
  int failed_code;
  int failed_alias;  // index into init.alias when failed_step == ALIAS, else -1
  int alias_kr[5];   // vm_remap result per alias (0 also when not mapped)
  int handler_errno;
  double reserve_us, protect_us, alias_us[5], handler_us, selftest_us;
  // Teardown (keep=0, or cleanup after a failure): -1 = not done (window
  // kept), 1 = window deallocated and both previous dispositions back in
  // place, 0 = something did not come back (dealloc_kr / handler_restored).
  int teardown;
  int dealloc_kr;
  int handler_restored;
  double teardown_us, total_us;
} AotFastmemProbeResult;

// Runs the probe (see AotFastmemProbeResult). Arguments as for
// aot_fastmem_window_create. Returns 1 when the window came up (and, with
// keep=0, was torn down again).
int aot_fastmem_probe(AOTState* s, uint8_t* ram, uint32_t ram_size, uint8_t* exram,
                      uint32_t exram_size, uint8_t* l1, uint32_t l1_size, int keep,
                      AotFastmemProbeResult* out);
// Step / alias names for logs ("reserve", "alias_MEM2@90", ...).
const char* aot_fastmem_probe_step_name(const AotFastmemProbeResult* r);
const char* aot_fastmem_alias_name(int index);
// The one-line summary WITHOUT the "AOT fastmem probe: " prefix:
//   window=UP|FAIL(<step>,<code>) aliases=<ok>/<mapped> handler=OK|FAIL|-
//   selftest=<n>/8 fault_us=<f> mode=keep|teardown teardown=OK|FAIL|- total_us=<n>
// Returns the length written (snprintf semantics).
int aot_fastmem_probe_format(const AotFastmemProbeResult* r, char* out, unsigned long out_len);

// Unmaps the window, restores the previous signal handlers. Idempotent.
void aot_fastmem_window_destroy(void);
uint8_t* aot_fastmem_window_base(void);

// Faults handled since the window was created, by direction and by EA region.
enum
{
  AOT_FMW_REGION_LOWMEM = 0,  // 0x0xxxxxxx
  AOT_FMW_REGION_MMIO = 1,    // 0xCxxxxxxx / 0xDxxxxxxx except EFB
  AOT_FMW_REGION_EFB = 2,     // 0xC8xxxxxx
  AOT_FMW_REGION_OTHER = 3,
  AOT_FMW_REGIONS = 4
};
typedef struct AotFastmemStats
{
  uint64_t reads[AOT_FMW_REGIONS];
  uint64_t writes[AOT_FMW_REGIONS];
  uint64_t unhandled;  // window faults whose instruction did not decode (crash follows)
} AotFastmemStats;
void aot_fastmem_get_stats(AotFastmemStats* out);
uint64_t aot_fastmem_fault_total(void);

#ifdef __cplusplus
}
#endif
