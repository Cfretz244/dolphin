// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/CPUCoreBase.h"

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;
}

class Interpreter;
struct AOTState;

using AOTBlockFunc = void (*)(AOTState*);

// AOT CPU core backend: executes pre-compiled C functions for translated PPC blocks.
// Falls back to the interpreter for untranslated addresses.
class AOTCore : public CPUCoreBase
{
public:
  explicit AOTCore(Core::System& system);
  ~AOTCore() override;

  void Init() override;
  void Shutdown() override;
  void Run() override;
  void SingleStep() override;
  // Savestate loads clear caches and can wholesale replace the loaded-module
  // landscape — force a module queue rescan.
  void ClearCache() override;
  const char* GetName() const override { return "AOT"; }

  // Set block sizes for diff mode (called by DiffCommand before boot)
  static void SetDiffBlockSizes(std::unordered_map<u32, u32> sizes);
  static std::unordered_map<u32, u32> s_diff_block_sizes;

  // Shutdown flag — set by signal handler, checked by diff loop
  static std::atomic<bool> s_shutdown_requested;

private:
  void RunDiff();

  // Snapshot of CPU register state for comparison
  struct PPCSnapshot
  {
    u32 pc, npc;
    u32 gpr[32];
    u64 ps[32][2];
    u64 cr_fields[8];
    u32 msr, fpscr;
    u32 exceptions;
    s32 downcount;
    u8 xer_ca, xer_so_ov;
    u32 spr_lr, spr_ctr, spr_xer;
  };

  void CaptureSnapshot(PPCSnapshot& snap);
  void RestoreSnapshot(const PPCSnapshot& snap);
  bool CompareSnapshots(const PPCSnapshot& a, const PPCSnapshot& b, u32 block_pc, FILE* log);
  void LogDivergence(u32 block_pc, u32 num_instr, const PPCSnapshot& pre,
                     const PPCSnapshot& aot_result, const PPCSnapshot& interp_result, FILE* log);
  bool BlockReadsTimebase(u32 block_addr, u32 num_instructions);
  bool BlockAccessesMMIO(const PPCSnapshot& pre, u32 block_addr, u32 num_instructions);
  int RunInterpreterBlock(Interpreter& interp, u32 block_addr, u32 num_instructions,
                          bool ignore_exceptions = false, u32 stop_pc = 0);
  void RunInterpreterDispatch(Interpreter& interp);

  Core::System& m_system;
  PowerPC::PowerPCState& m_ppc_state;

  using DispatchFunc = void (*)(AOTState*);
  using LookupFunc = AOTBlockFunc (*)(uint32_t);
  DispatchFunc m_dispatch = nullptr;
  DispatchFunc m_interp_dispatch = nullptr;
  LookupFunc m_lookup_block = nullptr;

  // Block boundary map: ppc_addr -> num_instructions (loaded from CFG DB for diff mode)
  std::unordered_map<u32, u32> m_block_sizes;

  // Track how many times each block has passed comparison (for skipping validated blocks)
  std::unordered_map<u32, u32> m_block_pass_count;

  // RAM shadow buffer for diff mode (24 MB)
  u8* m_ram_shadow = nullptr;
  u8* m_ram_shadow_aot = nullptr;  // For RAM comparison

  // Full emulator state buffer for diff mode (saves all HW registers too)
  std::vector<u8> m_state_buffer;
};
