// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOTCore.h"

#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef DOLPHIN_HAS_AOT

// AOTState definition — must be layout-compatible with PowerPCState.
// This is the struct that AOT-generated C code operates on.
// Defined here for the static_assert checks; the actual definition used by
// generated code is in the emitted aot_runtime.h header.
struct AOTState
{
  uint32_t pc;
  uint32_t npc;
  void* stored_stack_pointer;
  void* gather_pipe_ptr;
  void* gather_pipe_base_ptr;
  uint32_t gpr[32];
  struct
  {
    uint64_t ps0;
    uint64_t ps1;
  } ps[32] __attribute__((aligned(16)));
  uint64_t cr_fields[8];
  uint32_t msr;
  uint32_t fpscr;
  uint32_t feature_flags;
  uint32_t exceptions;
  int32_t downcount;
  uint8_t xer_ca;
  uint8_t xer_so_ov;
  uint16_t xer_stringctrl;
  uint32_t reserve_address;
  uint8_t reserve;
  uint8_t pagetable_update_pending;
  uint8_t m_enable_dcache;
  uint8_t _pad0;
  uint32_t sr[16];
  uint32_t spr[1024] __attribute__((aligned(8)));
};

// Verify layout compatibility between PowerPCState and AOTState.
// If any of these fail, the AOT code will silently corrupt state.
static_assert(offsetof(PowerPC::PowerPCState, pc) == offsetof(AOTState, pc),
              "pc offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, gpr) == offsetof(AOTState, gpr),
              "gpr offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, downcount) == offsetof(AOTState, downcount),
              "downcount offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, xer_ca) == offsetof(AOTState, xer_ca),
              "xer_ca offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, spr) == offsetof(AOTState, spr),
              "spr offset mismatch");

// The AOT dispatch function — defined in the pre-compiled AOT static library.
// This symbol is resolved at link time.
extern "C" void GALE01_dispatch(AOTState* s);

// Static storage for diff block sizes (set by DiffCommand before boot)
std::unordered_map<u32, u32> AOTCore::s_diff_block_sizes;

void AOTCore::SetDiffBlockSizes(std::unordered_map<u32, u32> sizes)
{
  s_diff_block_sizes = std::move(sizes);
}

AOTCore::AOTCore(Core::System& system)
    : m_system(system), m_ppc_state(system.GetPPCState())
{
}

AOTCore::~AOTCore()
{
  std::free(m_ram_shadow);
  std::free(m_ram_shadow_aot);
}

void AOTCore::Init()
{
  m_dispatch = &GALE01_dispatch;
  INFO_LOG_FMT(POWERPC, "AOTCore: Initialized with GALE01 dispatch table");

  // Load diff mode settings
  if (Config::Get(Config::MAIN_DEBUG_AOT_DIFF_MODE))
  {
    // Block sizes are loaded by DiffCommand before boot via SetDiffBlockSizes()
    m_block_sizes = std::move(s_diff_block_sizes);
    if (m_block_sizes.empty())
    {
      ERROR_LOG_FMT(POWERPC, "AOTDiff: No block sizes loaded — call SetDiffBlockSizes() before boot");
      return;
    }

    INFO_LOG_FMT(POWERPC, "AOTDiff: Loaded {} block boundaries from CFG DB",
                 m_block_sizes.size());

    // Allocate RAM shadow buffers
    const u32 ram_size = m_system.GetMemory().GetRamSizeReal();
    m_ram_shadow = static_cast<u8*>(std::malloc(ram_size));
    if (Config::Get(Config::MAIN_DEBUG_AOT_COMPARE_RAM))
      m_ram_shadow_aot = static_cast<u8*>(std::malloc(ram_size));
  }
}

void AOTCore::Shutdown()
{
  m_dispatch = nullptr;
  m_block_sizes.clear();
  std::free(m_ram_shadow);
  m_ram_shadow = nullptr;
  std::free(m_ram_shadow_aot);
  m_ram_shadow_aot = nullptr;
}

void AOTCore::Run()
{
  if (Config::Get(Config::MAIN_DEBUG_AOT_DIFF_MODE))
  {
    RunDiff();
    return;
  }

  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();
  auto& power_pc = m_system.GetPowerPC();

  while (cpu.GetState() == CPU::State::Running)
  {
    // Sync npc before Advance() — CheckExternalExceptions() saves npc into SRR0
    // as the interrupt return address. Without this, rfi returns to a stale address.
    m_ppc_state.npc = m_ppc_state.pc;
    core_timing.Advance();

    // Idle loop detection: track recent PCs. If we see the same PC
    // appear twice within a short window, we're in a spin loop.
    u32 pc_history[8] = {};
    u32 pc_idx = 0;

    while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running)
    {
      u32 pc = m_ppc_state.pc;

      // Check if this PC appeared recently in history
      for (u32 i = 0; i < 8; i++)
      {
        if (pc_history[i] == pc && pc != 0)
        {
          // Idle loop detected — skip remaining timeslice
          m_ppc_state.downcount = 0;
          goto next_slice;
        }
      }
      pc_history[pc_idx & 7] = pc;
      pc_idx++;

      auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
      m_dispatch(aot_state);

      // Check for exceptions raised during block execution (DSI, PROGRAM, etc.)
      if (m_ppc_state.Exceptions != 0)
      {
        m_ppc_state.npc = m_ppc_state.pc;
        power_pc.CheckExceptions();
      }
    }
    next_slice:;
  }
}

void AOTCore::SingleStep()
{
  auto& core_timing = m_system.GetCoreTiming();
  core_timing.Advance();

  auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
  m_dispatch(aot_state);

  m_ppc_state.downcount = 0;
}

// ============================================================================
// Diff harness
// ============================================================================

void AOTCore::CaptureSnapshot(PPCSnapshot& snap)
{
  snap.pc = m_ppc_state.pc;
  snap.npc = m_ppc_state.npc;
  std::memcpy(snap.gpr, m_ppc_state.gpr, sizeof(snap.gpr));
  for (int i = 0; i < 32; i++)
  {
    snap.ps[i][0] = m_ppc_state.ps[i].PS0AsU64();
    snap.ps[i][1] = m_ppc_state.ps[i].PS1AsU64();
  }
  std::memcpy(snap.cr_fields, m_ppc_state.cr.fields, sizeof(snap.cr_fields));
  snap.msr = m_ppc_state.msr.Hex;
  snap.fpscr = m_ppc_state.fpscr.Hex;
  snap.exceptions = m_ppc_state.Exceptions;
  snap.downcount = m_ppc_state.downcount;
  snap.xer_ca = m_ppc_state.xer_ca;
  snap.xer_so_ov = m_ppc_state.xer_so_ov;
  snap.spr_lr = m_ppc_state.spr[SPR_LR];
  snap.spr_ctr = m_ppc_state.spr[SPR_CTR];
  snap.spr_xer = m_ppc_state.spr[SPR_XER];
}

void AOTCore::RestoreSnapshot(const PPCSnapshot& snap)
{
  m_ppc_state.pc = snap.pc;
  m_ppc_state.npc = snap.npc;
  std::memcpy(m_ppc_state.gpr, snap.gpr, sizeof(snap.gpr));
  for (int i = 0; i < 32; i++)
  {
    m_ppc_state.ps[i].SetPS0(snap.ps[i][0]);
    m_ppc_state.ps[i].SetPS1(snap.ps[i][1]);
  }
  std::memcpy(m_ppc_state.cr.fields, snap.cr_fields, sizeof(snap.cr_fields));
  m_ppc_state.msr.Hex = snap.msr;
  m_ppc_state.fpscr.Hex = snap.fpscr;
  m_ppc_state.Exceptions = snap.exceptions;
  m_ppc_state.downcount = snap.downcount;
  m_ppc_state.xer_ca = snap.xer_ca;
  m_ppc_state.xer_so_ov = snap.xer_so_ov;
  m_ppc_state.spr[SPR_LR] = snap.spr_lr;
  m_ppc_state.spr[SPR_CTR] = snap.spr_ctr;
  m_ppc_state.spr[SPR_XER] = snap.spr_xer;
}

bool AOTCore::CompareSnapshots(const PPCSnapshot& a, const PPCSnapshot& b, u32 block_pc,
                               FILE* log)
{
  bool match = true;

  if (a.pc != b.pc)
    match = false;
  for (int i = 0; i < 32; i++)
  {
    if (a.gpr[i] != b.gpr[i])
      match = false;
  }
  for (int i = 0; i < 8; i++)
  {
    if (a.cr_fields[i] != b.cr_fields[i])
      match = false;
  }
  for (int i = 0; i < 32; i++)
  {
    if (a.ps[i][0] != b.ps[i][0] || a.ps[i][1] != b.ps[i][1])
      match = false;
  }
  if (a.xer_ca != b.xer_ca || a.xer_so_ov != b.xer_so_ov)
    match = false;
  if (a.spr_lr != b.spr_lr || a.spr_ctr != b.spr_ctr)
    match = false;
  if (a.fpscr != b.fpscr)
    match = false;
  if (a.msr != b.msr)
    match = false;

  return match;
}

void AOTCore::LogDivergence(u32 block_pc, u32 num_instr, const PPCSnapshot& pre,
                            const PPCSnapshot& aot_result, const PPCSnapshot& interp_result,
                            FILE* log)
{
  fmt::print(log, "\n=== DIVERGENCE at block {:#010x} ({} instructions) ===\n", block_pc,
             num_instr);

  // Pre-state summary
  fmt::print(log, "\nPRE-STATE:\n");
  fmt::print(log, "  PC={:#010x}  LR={:#010x}  CTR={:#010x}\n", pre.pc, pre.spr_lr, pre.spr_ctr);

  // Diff GPRs
  for (int i = 0; i < 32; i++)
  {
    if (aot_result.gpr[i] != interp_result.gpr[i])
    {
      fmt::print(log, "  GPR r{}: AOT={:#010x}  INTERP={:#010x}  (pre={:#010x})\n", i,
                 aot_result.gpr[i], interp_result.gpr[i], pre.gpr[i]);
    }
  }

  // Diff CR
  for (int i = 0; i < 8; i++)
  {
    if (aot_result.cr_fields[i] != interp_result.cr_fields[i])
    {
      fmt::print(log, "  CR[{}]: AOT={:#018x}  INTERP={:#018x}  (pre={:#018x})\n", i,
                 aot_result.cr_fields[i], interp_result.cr_fields[i], pre.cr_fields[i]);
    }
  }

  // Diff PC
  if (aot_result.pc != interp_result.pc)
    fmt::print(log, "  PC: AOT={:#010x}  INTERP={:#010x}\n", aot_result.pc, interp_result.pc);

  // Diff LR/CTR
  if (aot_result.spr_lr != interp_result.spr_lr)
    fmt::print(log, "  LR: AOT={:#010x}  INTERP={:#010x}\n", aot_result.spr_lr,
               interp_result.spr_lr);
  if (aot_result.spr_ctr != interp_result.spr_ctr)
    fmt::print(log, "  CTR: AOT={:#010x}  INTERP={:#010x}\n", aot_result.spr_ctr,
               interp_result.spr_ctr);

  // Diff XER
  if (aot_result.xer_ca != interp_result.xer_ca)
    fmt::print(log, "  XER_CA: AOT={}  INTERP={}\n", aot_result.xer_ca, interp_result.xer_ca);
  if (aot_result.xer_so_ov != interp_result.xer_so_ov)
    fmt::print(log, "  XER_SO_OV: AOT={:#04x}  INTERP={:#04x}\n", aot_result.xer_so_ov,
               interp_result.xer_so_ov);

  // Diff FP
  for (int i = 0; i < 32; i++)
  {
    if (aot_result.ps[i][0] != interp_result.ps[i][0])
      fmt::print(log, "  PS[{}].ps0: AOT={:#018x}  INTERP={:#018x}\n", i, aot_result.ps[i][0],
                 interp_result.ps[i][0]);
    if (aot_result.ps[i][1] != interp_result.ps[i][1])
      fmt::print(log, "  PS[{}].ps1: AOT={:#018x}  INTERP={:#018x}\n", i, aot_result.ps[i][1],
                 interp_result.ps[i][1]);
  }

  // Diff MSR, FPSCR
  if (aot_result.msr != interp_result.msr)
    fmt::print(log, "  MSR: AOT={:#010x}  INTERP={:#010x}\n", aot_result.msr, interp_result.msr);
  if (aot_result.fpscr != interp_result.fpscr)
    fmt::print(log, "  FPSCR: AOT={:#010x}  INTERP={:#010x}\n", aot_result.fpscr,
               interp_result.fpscr);

  // Disassembly
  fmt::print(log, "\nDISASSEMBLY:\n");
  auto& mmu = m_system.GetMMU();
  for (u32 i = 0; i < num_instr; i++)
  {
    u32 addr = block_pc + i * 4;
    u32 opcode = mmu.Read_Opcode(addr);
    std::string disasm = Common::GekkoDisassembler::Disassemble(opcode, addr);
    fmt::print(log, "  {:08x}: {}\n", addr, disasm);
  }
  fmt::print(log, "\n");
  std::fflush(log);
}

int AOTCore::RunInterpreterBlock(Interpreter& interp, u32 block_addr, u32 num_instructions)
{
  int total_cycles = 0;
  const u32 block_end = block_addr + num_instructions * 4;

  for (u32 i = 0; i < num_instructions; i++)
  {
    total_cycles += interp.SingleStepInner();

    // Stop if PC left the block range (branch taken) or exception fired
    if (m_ppc_state.pc < block_addr || m_ppc_state.pc >= block_end)
      break;
    if (m_ppc_state.Exceptions != 0)
      break;
  }
  return total_cycles;
}

void AOTCore::RunDiff()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();
  auto& power_pc = m_system.GetPowerPC();
  auto& interp = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();

  const bool self_diff = Config::Get(Config::MAIN_DEBUG_AOT_SELF_DIFF);
  const bool compare_ram = Config::Get(Config::MAIN_DEBUG_AOT_COMPARE_RAM);
  const u32 max_blocks = Config::Get(Config::MAIN_DEBUG_AOT_DIFF_MAX_BLOCKS);
  const u32 max_divergences = Config::Get(Config::MAIN_DEBUG_AOT_DIFF_MAX_DIVERGENCES);
  const u32 filter_min = Config::Get(Config::MAIN_DEBUG_AOT_DIFF_FILTER_MIN);
  const u32 filter_max = Config::Get(Config::MAIN_DEBUG_AOT_DIFF_FILTER_MAX);
  const u32 ram_size = memory.GetRamSizeReal();

  const std::string log_path = Config::Get(Config::MAIN_DEBUG_AOT_DIFF_LOG_PATH);
  FILE* log = stdout;
  if (!log_path.empty())
  {
    log = std::fopen(log_path.c_str(), "w");
    if (!log)
    {
      ERROR_LOG_FMT(POWERPC, "AOTDiff: Cannot open log file: {}", log_path);
      log = stdout;
    }
  }

  u32 blocks_compared = 0;
  u32 blocks_skipped_unknown = 0;
  u32 blocks_skipped_mmio = 0;
  u32 divergence_count = 0;

  fmt::print(log, "AOT Diff Harness — {} mode\n", self_diff ? "self-diff" : "AOT-vs-interpreter");
  fmt::print(log, "Block boundaries loaded: {}\n", m_block_sizes.size());
  fmt::print(log, "RAM size: {} MB\n\n", ram_size / (1024 * 1024));
  std::fflush(log);

  while (cpu.GetState() == CPU::State::Running)
  {
    m_ppc_state.npc = m_ppc_state.pc;
    core_timing.Advance();

    while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running)
    {
      const u32 block_pc = m_ppc_state.pc;

      // Look up block size from CFG database
      auto it = m_block_sizes.find(block_pc);
      if (it == m_block_sizes.end())
      {
        // Unknown block — run through AOT/interpreter without comparison
        blocks_skipped_unknown++;
        if (self_diff)
        {
          interp.SingleStepInner();
          m_ppc_state.downcount -= 1;
        }
        else
        {
          auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
          m_dispatch(aot_state);
        }
        if (m_ppc_state.Exceptions != 0)
        {
          m_ppc_state.npc = m_ppc_state.pc;
          power_pc.CheckExceptions();
        }
        continue;
      }

      const u32 num_instr = it->second;

      // Filter by address range
      if (block_pc < filter_min || block_pc > filter_max)
      {
        if (self_diff)
        {
          RunInterpreterBlock(interp, block_pc, num_instr);
          m_ppc_state.downcount -= static_cast<s32>(num_instr);
        }
        else
        {
          auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
          m_dispatch(aot_state);
        }
        if (m_ppc_state.Exceptions != 0)
        {
          m_ppc_state.npc = m_ppc_state.pc;
          power_pc.CheckExceptions();
        }
        continue;
      }

      // === COMPARISON BLOCK ===

      // 1. Snapshot CPU + RAM
      PPCSnapshot pre_snap;
      CaptureSnapshot(pre_snap);
      std::memcpy(m_ram_shadow, memory.GetRAM(), ram_size);

      // 2. Run path A (AOT or interpreter for self-diff)
      if (self_diff)
      {
        RunInterpreterBlock(interp, block_pc, num_instr);
        m_ppc_state.downcount -= static_cast<s32>(num_instr);
      }
      else
      {
        auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
        m_dispatch(aot_state);
      }

      PPCSnapshot path_a_result;
      CaptureSnapshot(path_a_result);

      // Optionally save RAM state after path A for comparison
      if (compare_ram && m_ram_shadow_aot)
        std::memcpy(m_ram_shadow_aot, memory.GetRAM(), ram_size);

      // 3. Restore CPU + RAM
      RestoreSnapshot(pre_snap);
      std::memcpy(memory.GetRAM(), m_ram_shadow, ram_size);

      // 4. Run path B (interpreter)
      RunInterpreterBlock(interp, block_pc, num_instr);
      m_ppc_state.downcount -= static_cast<s32>(num_instr);

      PPCSnapshot path_b_result;
      CaptureSnapshot(path_b_result);

      // 5. Compare
      // Skip comparison if either path hit an exception (DSI during memory access, etc.)
      if (path_a_result.exceptions != 0 || path_b_result.exceptions != 0)
      {
        // Continue from path B result (interpreter is authoritative)
        if (m_ppc_state.Exceptions != 0)
        {
          m_ppc_state.npc = m_ppc_state.pc;
          power_pc.CheckExceptions();
        }
        blocks_compared++;
        continue;
      }

      if (!CompareSnapshots(path_a_result, path_b_result, block_pc, log))
      {
        divergence_count++;
        LogDivergence(block_pc, num_instr, pre_snap, path_a_result, path_b_result, log);

        // Optional RAM comparison
        if (compare_ram && m_ram_shadow_aot)
        {
          u32 ram_diffs = 0;
          for (u32 offset = 0; offset < ram_size; offset += 4)
          {
            u32 a_val, b_val;
            std::memcpy(&a_val, m_ram_shadow_aot + offset, 4);
            std::memcpy(&b_val, memory.GetRAM() + offset, 4);
            if (a_val != b_val)
            {
              if (ram_diffs < 20)
              {
                fmt::print(log, "  RAM[{:#010x}]: pathA={:#010x}  pathB={:#010x}\n",
                           0x80000000 + offset, a_val, b_val);
              }
              ram_diffs++;
            }
          }
          if (ram_diffs > 0)
            fmt::print(log, "  Total RAM differences: {} words\n", ram_diffs);
        }

        if (divergence_count >= max_divergences)
        {
          fmt::print(log, "\nMax divergences ({}) reached. Stopping.\n", max_divergences);
          fmt::print(log, "Blocks compared: {} | Skipped (unknown): {} | Divergences: {}\n",
                     blocks_compared, blocks_skipped_unknown, divergence_count);
          std::fflush(log);
          if (log != stdout)
            std::fclose(log);
          Core::QueueHostJob([](Core::System& sys) { Core::Stop(sys); });
          cpu.Break();
          return;
        }
      }

      // Continue from interpreter result (path B is authoritative)
      // State is already set from path B
      if (m_ppc_state.Exceptions != 0)
      {
        m_ppc_state.npc = m_ppc_state.pc;
        power_pc.CheckExceptions();
      }

      blocks_compared++;
      if (max_blocks > 0 && blocks_compared >= max_blocks)
      {
        fmt::print(log, "\nMax blocks ({}) reached. Stopping.\n", max_blocks);
        fmt::print(log, "Blocks compared: {} | Skipped (unknown): {} | Divergences: {}\n",
                   blocks_compared, blocks_skipped_unknown, divergence_count);
        std::fflush(log);
        if (log != stdout)
          std::fclose(log);
        cpu.Break();
        return;
      }

      // Progress reporting
      if (blocks_compared % 10000 == 0)
      {
        fmt::print(stderr, "AOTDiff: {} blocks compared, {} divergences, {} skipped\n",
                   blocks_compared, divergence_count, blocks_skipped_unknown);
      }
    }
  }

  fmt::print(log, "\nEmulation stopped.\n");
  fmt::print(log, "Blocks compared: {} | Skipped (unknown): {} | Divergences: {}\n",
             blocks_compared, blocks_skipped_unknown, divergence_count);
  std::fflush(log);
  if (log != stdout)
    std::fclose(log);
}

#else  // !DOLPHIN_HAS_AOT

// Stub implementation when AOT is not enabled
AOTCore::AOTCore(Core::System& system)
    : m_system(system), m_ppc_state(system.GetPPCState())
{
}
AOTCore::~AOTCore() = default;
void AOTCore::Init() {}
void AOTCore::Shutdown() {}
void AOTCore::Run() {}
void AOTCore::SingleStep() {}
void AOTCore::RunDiff() {}
void AOTCore::CaptureSnapshot(PPCSnapshot&) {}
void AOTCore::RestoreSnapshot(const PPCSnapshot&) {}
bool AOTCore::CompareSnapshots(const PPCSnapshot&, const PPCSnapshot&, u32, FILE*) { return true; }
void AOTCore::LogDivergence(u32, u32, const PPCSnapshot&, const PPCSnapshot&, const PPCSnapshot&,
                            FILE*) {}
int AOTCore::RunInterpreterBlock(Interpreter&, u32, u32) { return 0; }

#endif  // DOLPHIN_HAS_AOT
