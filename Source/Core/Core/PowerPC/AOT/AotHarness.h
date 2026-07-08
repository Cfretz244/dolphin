// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// AOT correctness harness: the AOT_COMPARE live comparison loop, the
// `dolphin-tool diff` offline comparison mode, and the AOT_* debugging
// switches (savestate rings, RAM dumps, PC logs, staged interpreter/AOT
// switching). Compiled only when DOLPHIN_AOT_HARNESS is defined — production
// and iOS builds carry none of this and AOTCore::Run stays a pure fast loop.
//
// Env-var switches (all read once, at harness creation / run start):
//   AOT_COMPARE=1                per-block AOT-vs-interpreter comparison
//   AOT_COMPARE_EVERY_VISIT=1    compare every visit, not just first per block
//   AOT_COMPARE_RAM_INTERVAL=N   full-RAM compare every Nth comparison (default 16)
//   AOT_INTERP_ONLY=1            bypass AOT blocks entirely
//   AOT_SWITCH_AT=N              interpreter for N dispatches, then AOT
//   AOT_LOAD_STATE=<file>        load a savestate after boot warmup
//   AOT_DUMP_ON_LOAD=<file>      dump RAM right after AOT_LOAD_STATE
//   AOT_DUMP_FRAME=<file>        dump RAM after the first VI frame
//   AOT_LOG_PC=<file>            log every dispatch PC
//   AOT_TRACK_FALLBACKS=1        count interpreter fallbacks per PC (works in
//                                the fast loop too; enabled as a side effect)
//   AOT_FRAME_SAVESTATE=<path>   savestate ring at every field (see OnNewField)
//   AOT_FRAME_SAVESTATE_SLOTS=N  ring size (default 20)
// dolphin-tool diff drives RunDiff via the MAIN_DEBUG_AOT_DIFF_* Config keys.

#include <atomic>
#include <cstdio>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/AOT/AotRegistry.h"

class Interpreter;

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;
}

class AotHarness
{
public:
  using DispatchFunc = void (*)(AOTState*);

  // Returns a harness only when a harness feature is engaged (an AOT_* env
  // switch or dolphin-tool diff's Config keys); nullptr for production runs.
  static std::unique_ptr<AotHarness> MaybeCreate(Core::System& system,
                                                 const std::optional<AotGameEntry>& entry,
                                                 DispatchFunc dispatch,
                                                 DispatchFunc interp_dispatch);

  // Instrumented replacement for AOTCore::Run (handles diff mode internally).
  void Run();

  // Shutdown flag — set by DiffCommand's signal handler, checked by the diff loop.
  static void RequestShutdown();

  // AOT_FRAME_SAVESTATE hook, called from Core::Callback_NewField at emulated
  // field boundaries.
  static void OnNewField(Core::System& system);

  AotHarness(Core::System& system, DispatchFunc dispatch, DispatchFunc interp_dispatch);
  ~AotHarness();

private:
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

  void RunDiff();
  void CaptureSnapshot(PPCSnapshot& snap);
  void RestoreSnapshot(const PPCSnapshot& snap);
  bool CompareSnapshots(const PPCSnapshot& a, const PPCSnapshot& b, u32 block_pc, FILE* log);
  void LogDivergence(u32 block_pc, u32 num_instr, const PPCSnapshot& pre,
                     const PPCSnapshot& aot_result, const PPCSnapshot& interp_result, FILE* log);
  bool BlockReadsTimebase(u32 block_addr, u32 num_instructions);
  bool BlockAccessesMMIO(const PPCSnapshot& pre, u32 block_addr, u32 num_instructions);
  int RunInterpreterBlock(Interpreter& interp, u32 block_addr, u32 num_instructions,
                          bool ignore_exceptions = false, u32 stop_pc = 0);

  static std::atomic<bool> s_shutdown_requested;

  Core::System& m_system;
  PowerPC::PowerPCState& m_ppc_state;

  DispatchFunc m_dispatch = nullptr;
  DispatchFunc m_interp_dispatch = nullptr;
  AOTLookupFunc m_lookup_block = nullptr;

  // Block boundary maps, from the AOT library's aot_register_block_sizes
  // (present in AOT_HARNESS builds of the generated code only).
  std::unordered_map<u32, u32> m_block_sizes;
  // REL module blocks: (module_id << 32 | section << 24 | offset) -> instructions
  std::unordered_map<u64, u32> m_module_block_sizes;

  // RAM shadow buffers for diff mode (24 MB each)
  u8* m_ram_shadow = nullptr;
  u8* m_ram_shadow_aot = nullptr;

  // Full emulator state buffer for diff mode (saves all HW registers too)
  std::vector<u8> m_state_buffer;
};
