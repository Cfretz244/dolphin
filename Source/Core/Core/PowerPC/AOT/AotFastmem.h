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
