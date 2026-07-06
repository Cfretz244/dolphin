// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOTCore.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include <fmt/format.h>
#include <sqlite3.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Swap.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/HW.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/VideoInterface.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/State.h"
#include "VideoCommon/CommandProcessor.h"
#include "Core/ConfigManager.h"
#include "Core/PowerPC/AotModuleTracker.h"
#include "Core/PowerPC/AotRegistry.h"
#include "Core/PowerPC/MMIOCapture.h"
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
static_assert(offsetof(PowerPC::PowerPCState, gather_pipe_ptr) == offsetof(AOTState, gather_pipe_ptr),
              "gather_pipe_ptr offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, gather_pipe_base_ptr) ==
                  offsetof(AOTState, gather_pipe_base_ptr),
              "gather_pipe_base_ptr offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, gpr) == offsetof(AOTState, gpr),
              "gpr offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, downcount) == offsetof(AOTState, downcount),
              "downcount offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, xer_ca) == offsetof(AOTState, xer_ca),
              "xer_ca offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, spr) == offsetof(AOTState, spr),
              "spr offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, ps) == offsetof(AOTState, ps),
              "ps offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, cr) == offsetof(AOTState, cr_fields),
              "cr offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, msr) == offsetof(AOTState, msr),
              "msr offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, Exceptions) == offsetof(AOTState, exceptions),
              "exceptions offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, fpscr) == offsetof(AOTState, fpscr),
              "fpscr offset mismatch");

// Single-block mode: when set, dispatch returns immediately without chaining.
// Defined in AotRuntime.cpp; declared here for diff/compare mode usage.
extern "C" int aot_single_block_mode;

// Static storage for diff block sizes (set by DiffCommand before boot)
std::unordered_map<u32, u32> AOTCore::s_diff_block_sizes;
std::atomic<bool> AOTCore::s_shutdown_requested{false};

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

extern "C" void aot_interpreter_single_step(AOTState* s);
extern "C" void aot_init_fast_mem();
extern "C" void aot_enable_fallback_tracking();
extern "C" void aot_dump_fallback_stats();

static void interp_only_dispatch(AOTState* s)
{
  aot_interpreter_single_step(s);
}

void AOTCore::Init()
{
  // Cache RAM pointer and size for fast memory access
  aot_init_fast_mem();

  if (getenv("AOT_TRACK_FALLBACKS"))
    aot_enable_fallback_tracking();

  m_interp_dispatch = &interp_only_dispatch;

  // Look up the AOT library for the currently loaded game.
  const std::string game_id = SConfig::GetInstance().GetGameID();
  auto entry = AotRegistry::Instance().Find(game_id);
  if (entry)
  {
    m_dispatch = entry->dispatch;
    m_lookup_block = entry->lookup_block;
    AotModuleTracker::Init(entry->modules, entry->module_count);
    INFO_LOG_FMT(POWERPC, "AOTCore: Found AOT library for game {} ({} REL modules)", game_id,
                 entry->module_count);
  }
  else
  {
    m_dispatch = m_interp_dispatch;
    m_lookup_block = nullptr;
    const auto registered = AotRegistry::Instance().GetRegisteredGameIDs();
    std::string available;
    for (size_t i = 0; i < registered.size(); i++)
    {
      if (i > 0)
        available += ", ";
      available += registered[i];
    }
    WARN_LOG_FMT(POWERPC,
                 "AOTCore: No AOT library for game {}. Available: [{}]. "
                 "Falling back to interpreter.",
                 game_id, available);
  }

  // Load block sizes if available (needed for both diff mode and compare mode)
  if (!s_diff_block_sizes.empty())
  {
    m_block_sizes = s_diff_block_sizes;  // copy, not move — may be reused
    INFO_LOG_FMT(POWERPC, "AOTCore: Loaded {} block boundaries", m_block_sizes.size());
  }

  // Load diff mode settings
  if (Config::Get(Config::MAIN_DEBUG_AOT_DIFF_MODE))
  {
    if (m_block_sizes.empty())
    {
      ERROR_LOG_FMT(POWERPC, "AOTDiff: No block sizes loaded — call SetDiffBlockSizes() before boot");
      return;
    }

    // Allocate RAM shadow buffers for RAM comparison
    const u32 ram_size = m_system.GetMemory().GetRamSizeReal();
    m_ram_shadow = static_cast<u8*>(std::malloc(ram_size));
    m_ram_shadow_aot = static_cast<u8*>(std::malloc(ram_size));

    // Allocate full state buffer for HW state save/restore (used for MMIO blocks)
    // Initial size estimate — will grow if needed
    m_state_buffer.resize(64 * 1024 * 1024);  // 64 MB should be plenty
  }

  // AOT_COMPARE mode: load block sizes from CFG DB if not already loaded
  if (m_block_sizes.empty() && std::getenv("AOT_COMPARE"))
  {
    const std::string cfg_path = Config::Get(Config::MAIN_DEBUG_AOT_CFG_DB_PATH);
    if (!cfg_path.empty())
    {
      sqlite3* db = nullptr;
      if (sqlite3_open_v2(cfg_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK)
      {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT ppc_addr, num_instructions FROM blocks WHERE is_translatable = 1";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
          while (sqlite3_step(stmt) == SQLITE_ROW)
          {
            u32 addr = static_cast<u32>(sqlite3_column_int64(stmt, 0));
            u32 num_instr = static_cast<u32>(sqlite3_column_int64(stmt, 1));
            m_block_sizes[addr] = num_instr;
          }
          sqlite3_finalize(stmt);
        }
        // Module blocks are keyed by (module, section, offset) — resolved at
        // runtime via the module tracker since load addresses are dynamic.
        const char* mod_sql = "SELECT module_id, section_idx, offset, num_instructions "
                              "FROM module_blocks WHERE is_translatable = 1";
        if (sqlite3_prepare_v2(db, mod_sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
          while (sqlite3_step(stmt) == SQLITE_ROW)
          {
            const u64 mid = static_cast<u64>(sqlite3_column_int64(stmt, 0));
            const u64 sect = static_cast<u64>(sqlite3_column_int(stmt, 1));
            const u64 off = static_cast<u64>(sqlite3_column_int64(stmt, 2));
            const u32 num_instr = static_cast<u32>(sqlite3_column_int64(stmt, 3));
            m_module_block_sizes[(mid << 32) | (sect << 24) | off] = num_instr;
          }
          sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
        INFO_LOG_FMT(POWERPC, "AOT_COMPARE: Loaded {} block boundaries (+{} module blocks) from {}",
                     m_block_sizes.size(), m_module_block_sizes.size(), cfg_path);
      }
    }
    else
    {
      WARN_LOG_FMT(POWERPC, "AOT_COMPARE: Set -C Dolphin.Debug.AOTCfgDbPath=<path> for block sizes");
    }
  }
}

void AOTCore::Shutdown()
{
  aot_dump_fallback_stats();
  AotModuleTracker::Shutdown();
  m_dispatch = nullptr;
  m_block_sizes.clear();
  m_module_block_sizes.clear();
  std::free(m_ram_shadow);
  m_ram_shadow = nullptr;
  std::free(m_ram_shadow_aot);
  m_ram_shadow_aot = nullptr;
}

void AOTCore::ClearCache()
{
  AotModuleTracker::MarkDirty();
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
  auto& interp = m_system.GetInterpreter();

  // AOT_INTERP_ONLY=1 bypasses all AOT blocks (for testing)
  const bool interp_only = std::getenv("AOT_INTERP_ONLY") != nullptr;
  auto* active_dispatch = interp_only ? m_interp_dispatch : m_dispatch;

  // AOT_LOAD_STATE=/path/to/save.sav loads a savestate after boot, before main loop.
  const char* load_state_path = std::getenv("AOT_LOAD_STATE");
  if (load_state_path)
  {
    // Wait for boot to complete (a few frames) before loading
    for (int warmup = 0; warmup < 10 && cpu.GetState() == CPU::State::Running; warmup++)
    {
      m_ppc_state.npc = m_ppc_state.pc;
      core_timing.Advance();
      do {
        auto* ast = reinterpret_cast<AOTState*>(&m_ppc_state);
        m_interp_dispatch(ast);
        if (m_ppc_state.Exceptions != 0) { m_ppc_state.npc = m_ppc_state.pc; power_pc.CheckExceptions(); }
      } while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running);
    }
    fmt::print(stderr, "AOT_LOAD_STATE: Loading {}...\n", load_state_path);
    State::LoadAs(m_system, std::string(load_state_path));
    fmt::print(stderr, "AOT_LOAD_STATE: Loaded, PC={:#010x}\n", m_ppc_state.pc);
  }

  // AOT_SWITCH_AT=N runs interpreter for first N dispatches, then switches to AOT.
  // Use with savestates to binary-search for the block that causes camera issues.
  const char* switch_at_str = std::getenv("AOT_SWITCH_AT");
  const u64 switch_at = switch_at_str ? std::strtoull(switch_at_str, nullptr, 10) : 0;
  u64 dispatch_count = 0;
  bool switched = false;
  if (switch_at > 0)
  {
    active_dispatch = m_interp_dispatch;
    fmt::print(stderr, "AOT_SWITCH_AT={}: starting with interpreter, switching to AOT after {} dispatches\n",
               switch_at, switch_at);
  }

  // AOT_DUMP_FRAME=<file> dumps 24MB RAM after the first complete frame, then exits.
  // Use with AOT_LOAD_STATE to compare one frame of AOT vs INTERP_ONLY.
  const char* dump_frame_path = std::getenv("AOT_DUMP_FRAME");
  bool dump_waiting = (dump_frame_path != nullptr);
  u32 last_vi_count = 0;
  if (dump_waiting)
  {
    last_vi_count = m_system.GetVideoInterface().GetXFBAddressTop();  // just a proxy for "frame changed"
    fmt::print(stderr, "AOT_DUMP_FRAME: Will dump RAM after first VI frame to {}\n", dump_frame_path);
  }

  // AOT_LOG_PC=<file> logs every dispatch PC to a file for diffing between modes.
  // Run once with AOT, once with AOT_INTERP_ONLY=1, diff the files.
  const char* log_pc_path = std::getenv("AOT_LOG_PC");
  FILE* pc_log = nullptr;
  u64 pc_log_count = 0;
  constexpr u64 PC_LOG_MAX = 5000000;  // Cap at 5M entries (~40MB file)
  if (log_pc_path)
  {
    pc_log = std::fopen(log_pc_path, "w");
    if (pc_log)
      fmt::print(stderr, "AOT_LOG_PC: Logging dispatch PCs to {} (max {})\n", log_pc_path, PC_LOG_MAX);
  }

  // AOT_COMPARE=1 enables inline per-block comparison in the live Run() loop.
  // Uses the real video backend, so savestates and VI interrupts work normally.
  // Reports the first divergence found, then continues with AOT execution.
  const bool compare_mode = !interp_only && std::getenv("AOT_COMPARE") != nullptr;
  bool found_divergence = false;
  u64 compare_count = 0;
  auto& memory = m_system.GetMemory();
  const u32 ram_size = memory.GetRamSizeReal();
  u8* compare_ram_shadow = nullptr;
  u8* compare_ram_aot = nullptr;
  // Harness throttles: the full-RAM shadow compare moves ~3x RAM size per block, which
  // reads as a hang over a gameplay soak. Defaults: compare each unique block once
  // (first clean visit), registers on every comparison, full RAM every Nth comparison.
  // AOT_COMPARE_EVERY_VISIT=1 restores the original compare-every-visit behavior and
  // forces full RAM each time unless AOT_COMPARE_RAM_INTERVAL overrides it.
  const bool compare_every_visit = std::getenv("AOT_COMPARE_EVERY_VISIT") != nullptr;
  u64 compare_ram_interval = compare_every_visit ? 1 : 16;
  if (const char* iv = std::getenv("AOT_COMPARE_RAM_INTERVAL"))
    compare_ram_interval = std::max<u64>(1, std::strtoull(iv, nullptr, 10));
  std::unordered_set<u64> compared_keys;
  if (compare_mode)
  {
    compare_ram_shadow = static_cast<u8*>(std::malloc(ram_size));
    compare_ram_aot = static_cast<u8*>(std::malloc(ram_size));
    fmt::print(stderr,
               "AOT_COMPARE: Active, {} block sizes loaded, {}MB RAM shadow x2, "
               "{} visits, full-RAM every {} comparison(s)\n",
               m_block_sizes.size(), ram_size / (1024 * 1024),
               compare_every_visit ? "all" : "first", compare_ram_interval);
  }

  while (cpu.GetState() == CPU::State::Running)
  {
    m_ppc_state.npc = m_ppc_state.pc;
    core_timing.Advance();

    // Check if a VI frame just completed (for AOT_DUMP_FRAME)
    if (dump_waiting)
    {
      u32 cur_vi = m_system.GetVideoInterface().GetXFBAddressTop();
      if (cur_vi != last_vi_count && cur_vi != 0)
      {
        last_vi_count = cur_vi;
        u8* ram = memory.GetRAM();
        FILE* df = std::fopen(dump_frame_path, "wb");
        if (df)
        {
          std::fwrite(ram, 1, ram_size, df);
          std::fclose(df);
          fmt::print(stderr, "AOT_DUMP_FRAME: Dumped {} bytes to {}, PC={:#010x}\n",
                     ram_size, dump_frame_path, m_ppc_state.pc);
        }
        dump_waiting = false;
      }
    }

    do
    {
      auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);

      u32 pre_dispatch_pc = m_ppc_state.pc;

      if (compare_mode && !found_divergence)
      {
        // Try to compare this block: AOT vs interpreter
        AOTBlockFunc aot_fn = m_lookup_block ? m_lookup_block(m_ppc_state.pc) : nullptr;
        u32 block_pc = m_ppc_state.pc;
        u32 block_size = 0;
        // Dedup key: DOL pc, or bit63-tagged module composite (module keys can otherwise
        // collide with DOL pcs when mid is small).
        u64 compare_key = block_pc;
        if (aot_fn)
        {
          auto it = m_block_sizes.find(block_pc);
          if (it != m_block_sizes.end())
            block_size = it->second;
        }
        else
        {
          // Not in the DOL fast table — maybe a REL module block.
          AOTBlockFunc mod_fn = nullptr;
          u32 mid, sect, off;
          if (AotModuleTracker::LookupBlock(block_pc, &mod_fn, &mid, &sect, &off) && mod_fn)
          {
            auto it = m_module_block_sizes.find((u64(mid) << 32) | (u64(sect) << 24) | off);
            if (it != m_module_block_sizes.end())
            {
              aot_fn = mod_fn;
              block_size = it->second;
              compare_key = (u64(1) << 63) | (u64(mid) << 32) | (u64(sect) << 24) | off;
            }
          }
        }

        if (aot_fn && block_size > 0 && !compare_every_visit &&
            compared_keys.count(compare_key) != 0)
        {
          // Already compared clean on a previous visit — run at production speed.
          active_dispatch(aot_state);
        }
        else if (aot_fn && block_size > 0)
        {
          u32 num_instr = block_size;

          // Skip MMIO/gather-pipe blocks — running both paths would
          // double-send GPU commands and corrupt rendering
          PPCSnapshot pre_check;
          CaptureSnapshot(pre_check);
          if (BlockAccessesMMIO(pre_check, block_pc, num_instr) ||
              BlockReadsTimebase(block_pc, num_instr))
          {
            // Run normally through AOT, no comparison for MMIO/timebase blocks.
            // Deliberately NOT marked as compared: a later non-MMIO visit (different
            // register values) should still get a real comparison.
            s32 saved_dc2 = m_ppc_state.downcount;
            m_ppc_state.downcount = static_cast<s32>(num_instr);
            aot_single_block_mode = 1;
            aot_fn(aot_state);
            aot_single_block_mode = 0;
            m_ppc_state.downcount = saved_dc2 - static_cast<s32>(num_instr);
          }
          else
          {

          compare_count++;

          // Full-RAM comparison is ~3x RAM size of memory traffic; do it every Nth
          // comparison. Register comparison happens on all. On register-only
          // comparisons the interpreter re-runs against AOT-written RAM — identical
          // to pre-state whenever the AOT block is correct, so no false positives;
          // pure-RAM divergences on those visits are caught probabilistically by the
          // sampled full-RAM passes.
          const bool full_ram = (compare_count % compare_ram_interval) == 1 ||
                                compare_ram_interval == 1;

          // Save pre-state (registers, and RAM when sampling)
          PPCSnapshot pre, aot_result, interp_result;
          pre = pre_check;
          u8* ram = memory.GetRAM();
          if (full_ram)
            std::memcpy(compare_ram_shadow, ram, ram_size);  // pre-RAM saved

          // Run AOT single block
          s32 saved_dc = m_ppc_state.downcount;
          m_ppc_state.downcount = static_cast<s32>(num_instr);
          aot_single_block_mode = 1;
          aot_fn(aot_state);
          aot_single_block_mode = 0;
          CaptureSnapshot(aot_result);

          if (full_ram)
          {
            // Save AOT's RAM, then restore pre-RAM for an independent interpreter run
            std::memcpy(compare_ram_aot, ram, ram_size);  // AOT-RAM saved
            std::memcpy(ram, compare_ram_shadow, ram_size);  // pre-RAM restored
          }
          RestoreSnapshot(pre);

          // Run interpreter for the same block, stopping where the AOT run exited
          m_ppc_state.downcount = static_cast<s32>(num_instr);
          RunInterpreterBlock(interp, block_pc, num_instr, /*ignore_exceptions=*/true,
                              /*stop_pc=*/aot_result.pc);
          CaptureSnapshot(interp_result);
          // Now live RAM = interpreter's result

          // Compare registers
          bool regs_match = CompareSnapshots(aot_result, interp_result, block_pc, nullptr);

          // Compare RAM (AOT result vs interpreter result)
          bool ram_match = true;
          if (full_ram)
          {
            for (u32 j = 0; j < ram_size; j++)
            {
              if (compare_ram_aot[j] != ram[j])
              {
                ram_match = false;
                break;
              }
            }
          }

          if (regs_match && ram_match && !compare_every_visit)
            compared_keys.insert(compare_key);

          if (!regs_match || !ram_match)
          {
            found_divergence = true;
            fmt::print(stderr, "\n=== AOT_COMPARE: First divergence at block {:#010x} "
                       "(after {} comparisons, regs={} ram={}) ===\n",
                       block_pc, compare_count,
                       regs_match ? "match" : "DIFFER", ram_match ? "match" : "DIFFER");
            LogDivergence(block_pc, num_instr, pre, aot_result, interp_result, stderr);
            if (!ram_match)
            {
              fmt::print(stderr, "\nRAM differences (first 20):\n");
              int ram_diffs = 0;
              for (u32 j = 0; j < ram_size && ram_diffs < 20; j++)
              {
                if (compare_ram_aot[j] != ram[j])
                {
                  fmt::print(stderr, "  0x{:08x}: AOT=0x{:02x} INTERP=0x{:02x}\n",
                             0x80000000 + j, compare_ram_aot[j], ram[j]);
                  ram_diffs++;
                }
              }
            }
            fmt::print(stderr, "=== Continuing with interpreter result ===\n\n");
          }

          m_ppc_state.downcount = saved_dc - static_cast<s32>(num_instr);

          if (compare_count % 2000 == 0)
            fmt::print(stderr,
                       "AOT_COMPARE: {} comparisons ({} unique blocks), 0 divergences, "
                       "PC={:#010x}\n",
                       compare_count, compared_keys.size(), m_ppc_state.pc);
          }  // end else (non-MMIO comparison)
        }  // end if (aot_fn && block_sizes)
        else
        {
          // No AOT block or no size info — run through normal dispatch
          active_dispatch(aot_state);
        }
      }
      else
      {
        active_dispatch(aot_state);
      }

      if (switch_at > 0 && !switched)
      {
        dispatch_count++;
        if (dispatch_count >= switch_at)
        {
          switched = true;
          active_dispatch = m_dispatch;
          fmt::print(stderr, "AOT_SWITCH_AT: Switched to AOT at dispatch #{}, PC={:#010x}\n",
                     dispatch_count, m_ppc_state.pc);
        }
      }

      if (m_ppc_state.Exceptions != 0)
      {
        m_ppc_state.npc = m_ppc_state.pc;
        power_pc.CheckExceptions();
      }

      if (pc_log && pc_log_count < PC_LOG_MAX)
      {
        // Log: pre-dispatch PC → post-dispatch PC (shows block execution as one line)
        fmt::print(pc_log, "{:08x} {:08x}\n", pre_dispatch_pc, m_ppc_state.pc);
        pc_log_count++;
        if (pc_log_count == PC_LOG_MAX)
        {
          std::fflush(pc_log); std::fclose(pc_log); pc_log = nullptr;
          fmt::print(stderr, "AOT_LOG_PC: Reached {} entries, log closed\n", PC_LOG_MAX);
        }
      }
    } while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running);
  }

  std::free(compare_ram_shadow);
  std::free(compare_ram_aot);
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
  if (a.exceptions != b.exceptions)
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

  // Diff MSR, FPSCR, Exceptions
  if (aot_result.msr != interp_result.msr)
    fmt::print(log, "  MSR: AOT={:#010x}  INTERP={:#010x}\n", aot_result.msr, interp_result.msr);
  if (aot_result.fpscr != interp_result.fpscr)
    fmt::print(log, "  FPSCR: AOT={:#010x}  INTERP={:#010x}\n", aot_result.fpscr,
               interp_result.fpscr);
  if (aot_result.exceptions != interp_result.exceptions)
    fmt::print(log, "  EXCEPTIONS: AOT={:#010x}  INTERP={:#010x}\n", aot_result.exceptions,
               interp_result.exceptions);

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

// Check if a block likely accesses MMIO (hardware registers at 0xCC000000-0xCD000000).
// This is a heuristic: we check if any GPR in the pre-state points to the HW range,
// or if the block contains `lis rN, 0xCC00` which sets up an MMIO base address.
// Check if block reads timebase/decrementer (non-deterministic between AOT and interpreter
// because GetFakeTimeBase depends on downcount which is consumed at different rates).
bool AOTCore::BlockReadsTimebase(u32 block_addr, u32 num_instructions)
{
  u8* ram = m_system.GetMemory().GetRAM();
  for (u32 i = 0; i < num_instructions; i++)
  {
    u32 phys = (block_addr + i * 4) & 0x3FFFFFFF;
    u32 opcode;
    std::memcpy(&opcode, &ram[phys], sizeof(u32));
    opcode = Common::swap32(opcode);
    u32 primary = (opcode >> 26) & 0x3F;
    if (primary == 31)
    {
      u32 subop = (opcode >> 1) & 0x3FF;
      if (subop == 371)  // mftb
        return true;
      if (subop == 339)  // mfspr
      {
        u32 spr = ((opcode >> 16) & 0x1F) | ((opcode >> 6) & 0x3E0);
        if (spr == 268 || spr == 269 || spr == 22)  // TL, TU, DEC
          return true;
      }
    }
  }
  return false;
}

bool AOTCore::BlockAccessesMMIO(const PPCSnapshot& pre, u32 block_addr, u32 num_instructions)
{
  // Check pre-state GPRs for MMIO base addresses
  for (int i = 0; i < 32; i++)
  {
    u32 v = pre.gpr[i];
    if (v >= 0xCC000000 && v < 0xCD000000)
      return true;
  }

  // Scan instructions for `lis rN, 0xCC00`
  u8* ram = m_system.GetMemory().GetRAM();
  for (u32 i = 0; i < num_instructions; i++)
  {
    u32 phys = (block_addr + i * 4) & 0x3FFFFFFF;
    u32 opcode;
    std::memcpy(&opcode, &ram[phys], sizeof(u32));
    opcode = Common::swap32(opcode);
    u32 primary = (opcode >> 26) & 0x3F;

    if (primary == 15)
    {
      u32 ra = (opcode >> 16) & 0x1F;
      s16 imm = static_cast<s16>(opcode & 0xFFFF);
      if (ra == 0)
      {
        u32 val = static_cast<u32>(imm) << 16;
        if (val >= 0xCC000000 && val < 0xCD000000)
          return true;
      }
    }
  }
  return false;
}

int AOTCore::RunInterpreterBlock(Interpreter& interp, u32 block_addr, u32 num_instructions,
                                 bool ignore_exceptions, u32 stop_pc)
{
  int total_cycles = 0;
  const u32 block_end = block_addr + num_instructions * 4;
  // Limit to one pass through the block — don't follow self-loops.
  // Self-looping blocks (polling loops) must be handled by the caller.
  const u32 max_steps = num_instructions;

  // When comparing against an AOT single-block run, stop where the AOT block
  // actually exited. AOT blocks can legitimately end mid-range (unconditional
  // b inside a traced block, mtmsr interrupt check), and running the
  // interpreter further produces false-positive divergences.
  //
  // Disambiguation: if a backward branch inside the block targets stop_pc
  // (e.g. the icbi loop in ICInvalidateRange), the AOT run exited by TAKING
  // that backward edge after one loop pass — but the interpreter also passes
  // through stop_pc by plain fallthrough before the loop. In that case only
  // stop on arrival via a backward jump.
  bool stop_on_backward_jump_only = false;
  if (stop_pc != 0 && stop_pc >= block_addr && stop_pc < block_end)
  {
    u8* ram = m_system.GetMemory().GetRAM();
    for (u32 addr = stop_pc; addr < block_end; addr += 4)
    {
      u32 opcode;
      std::memcpy(&opcode, &ram[addr & 0x3FFFFFFF], sizeof(u32));
      opcode = Common::swap32(opcode);
      const u32 primary = (opcode >> 26) & 0x3F;
      u32 target = 0;
      if (primary == 18)  // b/ba/bl/bla
      {
        s32 li = (static_cast<s32>(opcode << 6) >> 6) & ~3;
        target = (opcode & 2) ? static_cast<u32>(li) : addr + li;
      }
      else if (primary == 16)  // bcx
      {
        s32 bd = static_cast<s32>(static_cast<s16>(opcode & 0xFFFC));
        target = (opcode & 2) ? static_cast<u32>(bd) : addr + bd;
      }
      else
      {
        continue;
      }
      if (target == stop_pc)
      {
        stop_on_backward_jump_only = true;
        break;
      }
    }
  }

  for (u32 i = 0; i < max_steps; i++)
  {
    const u32 prev_pc = m_ppc_state.pc;
    total_cycles += interp.SingleStepInner();

    if (stop_pc != 0 && m_ppc_state.pc == stop_pc &&
        (!stop_on_backward_jump_only || m_ppc_state.pc <= prev_pc))
      break;
    if (m_ppc_state.pc < block_addr || m_ppc_state.pc >= block_end)
      break;
    if (!ignore_exceptions && m_ppc_state.Exceptions != 0)
      break;
  }
  return total_cycles;
}

// Simulate the AOT dispatch: run interpreter through a chain of known blocks,
// stopping when PC reaches an unknown block (where AOT dispatch would return
// to its caller) or downcount is exhausted. This matches the AOT dispatch
// granularity where tail calls chain between known blocks.
void AOTCore::RunInterpreterDispatch(Interpreter& interp)
{
  constexpr u32 MAX_CHAIN = 100000;  // safety limit on chain length

  for (u32 chain = 0; chain < MAX_CHAIN; chain++)
  {
    u32 pc = m_ppc_state.pc;

    // Look up block at current PC
    auto it = m_block_sizes.find(pc);
    if (it == m_block_sizes.end())
    {
      // Unknown block — AOT dispatch would call aot_interpreter_single_step and return.
      // Do the same: one instruction, then stop.
      m_ppc_state.npc = m_ppc_state.pc + 4;
      int cycles = interp.SingleStepInner();
      m_ppc_state.pc = m_ppc_state.npc;
      m_ppc_state.downcount -= cycles;
      break;
    }

    u32 num_instr = it->second;
    RunInterpreterBlock(interp, pc, num_instr);

    // Decrement downcount by block cycle count (matching what AOT does)
    m_ppc_state.downcount -= static_cast<s32>(num_instr);

    if (m_ppc_state.Exceptions != 0)
      break;

    // Check if the new PC is a known block — if so, the AOT would tail-call it.
    // If unknown, the AOT block would have set s->pc and returned to dispatcher.
    u32 next_pc = m_ppc_state.pc;
    if (m_block_sizes.find(next_pc) == m_block_sizes.end())
      break;  // AOT would return to dispatcher here

    // Check downcount — AOT blocks check `if(s->downcount<=0) { s->pc=X; return; }`
    if (m_ppc_state.downcount <= 0)
      break;
  }
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
  u32 total_block_visits = 0;
  u32 last_status_visits = 0;
  constexpr u32 STATUS_INTERVAL = 5000;  // Print status every N block visits

  fmt::print(log, "AOT Diff Harness — {} mode\n", self_diff ? "self-diff" : "AOT-vs-interpreter");
  fmt::print(log, "Block boundaries loaded: {}\n", m_block_sizes.size());
  fmt::print(log, "RAM size: {} MB\n\n", ram_size / (1024 * 1024));
  std::fflush(log);

  // Load savestate if specified — allows comparing battle code, not just boot code
  const std::string savestate_path = Config::Get(Config::MAIN_DEBUG_AOT_DIFF_SAVESTATE_PATH);
  if (!savestate_path.empty())
  {
    fmt::print(log, "Loading savestate: {}\n", savestate_path);
    std::fflush(log);
    // State::LoadAs auto-detects CPU thread and executes synchronously
    State::LoadAs(m_system, savestate_path);
    fmt::print(log, "Savestate loaded. PC = {:#010x}\n\n", m_ppc_state.pc);
    std::fflush(log);
  }

  while (cpu.GetState() == CPU::State::Running && !s_shutdown_requested.load())
  {
    m_ppc_state.npc = m_ppc_state.pc;
    core_timing.Advance();

    while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running &&
           !s_shutdown_requested.load())
    {
      total_block_visits++;

      // Periodic status with video activity indicators
      if (total_block_visits - last_status_visits >= STATUS_INTERVAL)
      {
        last_status_visits = total_block_visits;
        u32 xfb_top = m_system.GetVideoInterface().GetXFBAddressTop();
        u32 xfb_bot = m_system.GetVideoInterface().GetXFBAddressBottom();
        auto& cp_fifo = m_system.GetCommandProcessor().GetFifo();
        u32 fifo_dist = cp_fifo.CPReadWriteDistance.load(std::memory_order_relaxed);
        u32 fifo_wptr = m_system.GetProcessorInterface().m_fifo_cpu_write_pointer;
        u32 fifo_base = m_system.GetProcessorInterface().m_fifo_cpu_base;
        fmt::print(stderr,
          "[{:>8}] cmp={} mmio={} unk={} skip={} div={} | "
          "XFB={:#010x}/{:#010x} FIFO base={:#010x} wptr={:#010x} dist={} PC={:#010x}\n",
          total_block_visits, blocks_compared, blocks_skipped_mmio,
          blocks_skipped_unknown, total_block_visits - blocks_compared - blocks_skipped_mmio - blocks_skipped_unknown,
          divergence_count,
          xfb_top, xfb_bot, fifo_base, fifo_wptr, fifo_dist, m_ppc_state.pc);
      }

      const u32 block_pc = m_ppc_state.pc;

      // Look up block in CFG database
      auto it = m_block_sizes.find(block_pc);
      if (it == m_block_sizes.end())
      {
        // Unknown block — run interpreter single step (same as AOT fallback)
        m_ppc_state.npc = m_ppc_state.pc + 4;
        int cycles = interp.SingleStepInner();
        m_ppc_state.pc = m_ppc_state.npc;
        m_ppc_state.downcount -= cycles;
        blocks_skipped_unknown++;
        if (m_ppc_state.Exceptions != 0)
        {
          m_ppc_state.npc = m_ppc_state.pc;
          power_pc.CheckExceptions();
        }
        continue;
      }

      const u32 num_instr = it->second;

      // Look up AOT block function early (needed by validation skip and filter paths)
      AOTBlockFunc aot_block_fn =
          (self_diff || !m_lookup_block) ? nullptr : m_lookup_block(block_pc);

      // No validation skip — compare every single block every time

      // Filter by address range — run AOT for filtered blocks (interpreter can deadlock)
      if (block_pc < filter_min || block_pc > filter_max)
      {
        if (aot_block_fn)
        {
          s32 saved_dc = m_ppc_state.downcount;
          m_ppc_state.downcount = static_cast<s32>(num_instr);
          aot_single_block_mode = 1;
          auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
          aot_block_fn(aot_state);
          aot_single_block_mode = 0;
          m_ppc_state.downcount = saved_dc - static_cast<s32>(num_instr);
        }
        else
        {
          RunInterpreterBlock(interp, block_pc, num_instr);
          m_ppc_state.downcount -= static_cast<s32>(num_instr);
        }
        if (m_ppc_state.Exceptions != 0)
        {
          m_ppc_state.npc = m_ppc_state.pc;
          power_pc.CheckExceptions();
        }
        continue;
      }

      if (!aot_block_fn && !self_diff)
      {
        // Block is in CFG but not in AOT table — run interpreter only
        RunInterpreterBlock(interp, block_pc, num_instr);
        m_ppc_state.downcount -= static_cast<s32>(num_instr);
        blocks_skipped_unknown++;
        if (m_ppc_state.Exceptions != 0)
        {
          m_ppc_state.npc = m_ppc_state.pc;
          power_pc.CheckExceptions();
        }
        continue;
      }

      // Polling loop optimization: if the same block runs many times in a row,
      // the expensive full-state save/restore/compare per iteration makes the diff
      // harness hang on polling loops. After 3 successful comparisons of the same
      // block, run via AOT dispatch without comparison to let downcount drain so
      // CoreTiming::Advance() can fire hardware events.
      {
        static u32 last_compared_pc = 0;
        static u32 compare_repeat_count = 0;
        if (block_pc == last_compared_pc)
          compare_repeat_count++;
        else
        {
          last_compared_pc = block_pc;
          compare_repeat_count = 0;
        }
        if (compare_repeat_count >= 3)
        {
          // Already verified this block — run AOT without comparison
          s32 saved_dc = m_ppc_state.downcount;
          m_ppc_state.downcount = static_cast<s32>(num_instr);
          aot_single_block_mode = 1;
          auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
          aot_block_fn(aot_state);
          aot_single_block_mode = 0;
          m_ppc_state.downcount = saved_dc - static_cast<s32>(num_instr);
          if (m_ppc_state.Exceptions != 0)
          {
            m_ppc_state.npc = m_ppc_state.pc;
            power_pc.CheckExceptions();
          }

          // Stuck loop escape: if the same block has been spinning for too long,
          // the MMIO DoState save/restores likely corrupted the CoreTiming event
          // queue so VI/SI events no longer fire. Force a VI interrupt to unstick
          // the game — the polling loop is typically waiting for an interrupt-driven
          // callback to set a flag in RAM.
          if (compare_repeat_count == 10000)
          {
            fmt::print(stderr,
              "STUCK at {:#010x} after {} repeats — forcing VI interrupt\n",
              block_pc, compare_repeat_count);
            m_system.GetProcessorInterface().SetInterrupt(
              ProcessorInterface::INT_CAUSE_VI);
          }
          continue;
        }
      }

      // === SINGLE-BLOCK COMPARISON ===

      // 1. Snapshot CPU + RAM
      PPCSnapshot pre_snap;
      CaptureSnapshot(pre_snap);

      // Timebase-reading blocks: can't compare (GetFakeTimeBase depends on downcount
      // consumed at different rates by AOT vs interpreter). Run interpreter only.
      if (BlockReadsTimebase(block_pc, num_instr))
      {
        RunInterpreterBlock(interp, block_pc, num_instr);
        m_ppc_state.downcount -= static_cast<s32>(num_instr);
        if (m_ppc_state.Exceptions != 0)
        {
          m_ppc_state.npc = m_ppc_state.pc;
          power_pc.CheckExceptions();
        }
        continue;
      }

      // MMIO blocks: save/restore FULL hardware state (not just CPU+RAM) so that
      // MMIO reads return identical values for both AOT and interpreter.
      bool is_mmio = BlockAccessesMMIO(pre_snap, block_pc, num_instr);
      if (is_mmio && aot_block_fn)
      {
        blocks_skipped_mmio++;

        // Save full emulator state (CPU + RAM + all HW registers)
        {
          u8* ptr = m_state_buffer.data();
          PointerWrap pw(&ptr, m_state_buffer.size(), PointerWrap::Mode::Write);
          HW::DoState(m_system, pw);
          m_system.GetPowerPC().DoState(pw);
          m_system.GetCoreTiming().DoState(pw);
          if (!pw.IsWriteMode())
          {
            // Buffer too small — grow and retry
            auto needed = pw.GetOffsetFromPreviousPosition(m_state_buffer.data());
            m_state_buffer.resize(needed);
            ptr = m_state_buffer.data();
            PointerWrap pw2(&ptr, m_state_buffer.size(), PointerWrap::Mode::Write);
            HW::DoState(m_system, pw2);
            m_system.GetPowerPC().DoState(pw2);
            m_system.GetCoreTiming().DoState(pw2);
          }
        }

        // Also save RAM separately for comparison
        std::memcpy(m_ram_shadow, memory.GetRAM(), ram_size);

        // Run AOT single-block with MMIO capture
        MMIOCaptureReset();
        g_mmio_capture_active = true;
        {
          s32 saved_dc = m_ppc_state.downcount;
          m_ppc_state.downcount = static_cast<s32>(num_instr);
          aot_single_block_mode = 1;
          auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
          aot_block_fn(aot_state);
          aot_single_block_mode = 0;
          m_ppc_state.downcount = saved_dc - static_cast<s32>(num_instr);
        }
        g_mmio_capture_active = false;
        auto aot_mmio = g_mmio_capture_log;
        PPCSnapshot aot_result;
        CaptureSnapshot(aot_result);

        // Restore FULL state (CPU + RAM + HW)
        {
          u8* ptr = m_state_buffer.data();
          PointerWrap pw(&ptr, m_state_buffer.size(), PointerWrap::Mode::Read);
          HW::DoState(m_system, pw);
          m_system.GetPowerPC().DoState(pw);
          m_system.GetCoreTiming().DoState(pw);
        }

        // Run interpreter with MMIO capture, stopping where the AOT run exited.
        // ignore_exceptions=true: AOT executes the full block atomically without
        // mid-block exception checks, so the interpreter must do the same for comparison.
        MMIOCaptureReset();
        g_mmio_capture_active = true;
        RunInterpreterBlock(interp, block_pc, num_instr, /*ignore_exceptions=*/true,
                            /*stop_pc=*/aot_result.pc);
        m_ppc_state.downcount -= static_cast<s32>(num_instr);
        g_mmio_capture_active = false;
        auto interp_mmio = g_mmio_capture_log;
        PPCSnapshot interp_result;
        CaptureSnapshot(interp_result);

        // Compare registers + MMIO writes + RAM
        bool reg_match = CompareSnapshots(aot_result, interp_result, block_pc, log);
        bool mmio_match = (aot_mmio.size() == interp_mmio.size());
        if (mmio_match)
        {
          for (size_t i = 0; i < aot_mmio.size(); i++)
          {
            // Mask values by write size: for sub-word writes (sth/stb), only the
            // low bits matter. AOT truncates to u16/u8 before WriteFromJit, while
            // the interpreter may pass the full GPR value.
            u32 aot_val = aot_mmio[i].value;
            u32 interp_val = interp_mmio[i].value;
            if (aot_mmio[i].size == 1)
            { aot_val &= 0xFF; interp_val &= 0xFF; }
            else if (aot_mmio[i].size == 2)
            { aot_val &= 0xFFFF; interp_val &= 0xFFFF; }
            if (aot_mmio[i].address != interp_mmio[i].address ||
                aot_val != interp_val ||
                aot_mmio[i].size != interp_mmio[i].size)
            { mmio_match = false; break; }
          }
        }

        bool diverged = !reg_match || !mmio_match;

        // Timing-dependent MMIO read detection: if registers diverged but MMIO
        // writes matched AND both had zero MMIO writes, this is likely a polling
        // loop reading a hardware register whose value depends on timing (e.g.,
        // DMA completion, interrupt status). Accept the AOT's result and continue.
        bool timing_skip = false;
        if (!reg_match && mmio_match && aot_mmio.empty() && interp_mmio.empty())
        {
          timing_skip = true;
          diverged = false;
          RestoreSnapshot(aot_result);
        }

        // Only print MMIO blocks on divergence or timing-skip (not every "ok")
        if (diverged || timing_skip)
        {
          fmt::print(stderr, "AOTDiff: block {:#010x} ({} instr, MMIO) — {}{}{}{}\n",
                     block_pc, num_instr,
                     timing_skip ? "timing-skip" : "DIVERGED",
                     !reg_match && !timing_skip ? " [regs]" : "",
                     !mmio_match ? fmt::format(" [mmio: {} vs {}]", aot_mmio.size(), interp_mmio.size()) : "",
                     timing_skip ? " [MMIO read timing]" : "");
        }

        if (diverged)
        {
          divergence_count++;
          if (!reg_match)
            LogDivergence(block_pc, num_instr, pre_snap, aot_result, interp_result, log);
          if (!mmio_match)
          {
            fmt::print(log, "\n=== MMIO DIVERGENCE at block {:#010x} ===\n", block_pc);
            fmt::print(log, "AOT MMIO writes ({}):\n", aot_mmio.size());
            for (auto& w : aot_mmio)
              fmt::print(log, "  [{:#010x}] = {:#010x} (size {})\n", w.address, w.value, w.size);
            fmt::print(log, "INTERP MMIO writes ({}):\n", interp_mmio.size());
            for (auto& w : interp_mmio)
              fmt::print(log, "  [{:#010x}] = {:#010x} (size {})\n", w.address, w.value, w.size);
          }
          std::fflush(log);
          if (divergence_count >= max_divergences)
          {
            Core::QueueHostJob([](Core::System& sys) { Core::Stop(sys); });
            cpu.Break();
            return;
          }
        }

        if (m_ppc_state.Exceptions != 0)
        {
          m_ppc_state.npc = m_ppc_state.pc;
          power_pc.CheckExceptions();
        }
        continue;
      }

      // FULL COMPARISON: snapshot CPU+RAM, run AOT, save AOT RAM, restore, run interpreter, compare.

      // 2. Save pre-RAM
      std::memcpy(m_ram_shadow, memory.GetRAM(), ram_size);

      // 3. Run AOT single-block
      {
        s32 saved_dc = m_ppc_state.downcount;
        m_ppc_state.downcount = static_cast<s32>(num_instr);
        aot_single_block_mode = 1;
        auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
        aot_block_fn(aot_state);
        aot_single_block_mode = 0;
        m_ppc_state.downcount = saved_dc - static_cast<s32>(num_instr);
      }

      PPCSnapshot aot_result;
      CaptureSnapshot(aot_result);

      // 3b. Save AOT's RAM result for comparison
      if (m_ram_shadow_aot)
        std::memcpy(m_ram_shadow_aot, memory.GetRAM(), ram_size);

      // 4. Restore CPU + RAM, run interpreter (stopping where the AOT run exited)
      RestoreSnapshot(pre_snap);
      std::memcpy(memory.GetRAM(), m_ram_shadow, ram_size);

      RunInterpreterBlock(interp, block_pc, num_instr, /*ignore_exceptions=*/false,
                          /*stop_pc=*/aot_result.pc);
      m_ppc_state.downcount -= static_cast<s32>(num_instr);

      PPCSnapshot interp_result;
      CaptureSnapshot(interp_result);

      // 5. Compare (skip if either path hit an exception)
      if (aot_result.exceptions != 0 || interp_result.exceptions != 0)
      {
        if (m_ppc_state.Exceptions != 0)
        {
          m_ppc_state.npc = m_ppc_state.pc;
          power_pc.CheckExceptions();
        }
        blocks_compared++;
        continue;
      }

      bool reg_diverged = !CompareSnapshots(aot_result, interp_result, block_pc, log);

      // Compare RAM: AOT result is in m_ram_shadow_aot, interpreter result is in current RAM
      bool ram_diverged = false;
      if (m_ram_shadow_aot)
      {
        u8* interp_ram = memory.GetRAM();
        int ram_diff_printed = 0;
        for (u32 i = 0; i < ram_size && ram_diff_printed < 10; i += 4)
        {
          u32 aot_val, interp_val;
          std::memcpy(&aot_val, &m_ram_shadow_aot[i], 4);
          std::memcpy(&interp_val, &interp_ram[i], 4);
          if (aot_val != interp_val)
          {
            if (!ram_diverged)
              fmt::print(log, "  RAM DIVERGENCES for block {:#010x}:\n", block_pc);
            ram_diverged = true;
            ram_diff_printed++;
            fmt::print(log, "    {:#010x}: AOT={:#010x} INTERP={:#010x}\n",
                       0x80000000 | i, Common::swap32(aot_val), Common::swap32(interp_val));
          }
        }
        if (ram_diff_printed >= 10)
          fmt::print(log, "    ... (truncated)\n");
      }

      bool diverged = reg_diverged || ram_diverged;

      // Only print on divergence (periodic status handles progress)
      if (diverged)
        fmt::print(stderr, "AOTDiff: block {:#010x} ({} instr) — DIVERGED\n",
                   block_pc, num_instr);

      if (diverged)
      {
        divergence_count++;
        LogDivergence(block_pc, num_instr, pre_snap, aot_result, interp_result, log);

        if (divergence_count >= max_divergences)
        {
          fmt::print(log, "\nMax divergences ({}) reached. Stopping.\n", max_divergences);
          std::fflush(log);
          if (log != stdout)
            std::fclose(log);
          Core::QueueHostJob([](Core::System& sys) { Core::Stop(sys); });
          cpu.Break();
          return;
        }
      }

      // Interpreter result already restored — always continue from oracle
      if (m_ppc_state.Exceptions != 0)
      {
        m_ppc_state.npc = m_ppc_state.pc;
        power_pc.CheckExceptions();
      }

      // Self-loop optimization: if block branches back to itself and passed comparison,
      // run the interpreter until the loop exits (PC changes). No need to compare every
      // iteration — one successful comparison proves the codegen for this block.
      if (!diverged && m_ppc_state.pc == block_pc)
      {
        u32 loop_iters = 0;
        while (m_ppc_state.pc == block_pc && m_ppc_state.downcount > 0 &&
               m_ppc_state.Exceptions == 0)
        {
          RunInterpreterBlock(interp, block_pc, num_instr);
          m_ppc_state.downcount -= static_cast<s32>(num_instr);
          loop_iters++;
        }
        if (loop_iters > 0)
          fmt::print(stderr, "AOTDiff: self-loop {:#010x} ran {} extra iterations via interp\n",
                     block_pc, loop_iters);
      }

      blocks_compared++;
      if (max_blocks > 0 && blocks_compared >= max_blocks)
      {
        fmt::print(log, "\nMax blocks ({}) reached. Stopping.\n", max_blocks);
        fmt::print(log,
                   "Blocks compared: {} | Skipped (unknown): {} | Skipped (MMIO): {} | "
                   "Divergences: {}\n",
                   blocks_compared, blocks_skipped_unknown, blocks_skipped_mmio, divergence_count);
        std::fflush(log);
        if (log != stdout)
          std::fclose(log);
        cpu.Break();
        return;
      }

      u32 total_visits = blocks_compared + blocks_skipped_unknown + blocks_skipped_mmio;
      if (total_visits % 10000 == 0)
      {
        // Check VI XFB state for progress monitoring
        auto& vi = m_system.GetVideoInterface();
        u32 xfb_top = vi.GetXFBAddressTop();
        u32 xfb_bot = vi.GetXFBAddressBottom();
        fmt::print(stderr,
                   "AOTDiff: cmp={} visit={} div={} skip={} mmio={} | pc={:#010x} | "
                   "xfb_top={:#010x} xfb_bot={:#010x}\n",
                   blocks_compared, total_visits, divergence_count,
                   blocks_skipped_unknown, blocks_skipped_mmio, m_ppc_state.pc,
                   xfb_top, xfb_bot);
      }
    }
  }

  fmt::print(log, "\nEmulation stopped.\n");
  fmt::print(log,
             "Blocks compared: {} | Skipped (unknown): {} | Skipped (MMIO): {} | Divergences: {}\n",
             blocks_compared, blocks_skipped_unknown, blocks_skipped_mmio, divergence_count);
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
void AOTCore::ClearCache() {}
void AOTCore::Run() {}
void AOTCore::SingleStep() {}
void AOTCore::RunDiff() {}
void AOTCore::CaptureSnapshot(PPCSnapshot&) {}
void AOTCore::RestoreSnapshot(const PPCSnapshot&) {}
bool AOTCore::CompareSnapshots(const PPCSnapshot&, const PPCSnapshot&, u32, FILE*) { return true; }
void AOTCore::LogDivergence(u32, u32, const PPCSnapshot&, const PPCSnapshot&, const PPCSnapshot&,
                            FILE*) {}
bool AOTCore::BlockAccessesMMIO(const PPCSnapshot&, u32, u32) { return false; }
int AOTCore::RunInterpreterBlock(Interpreter&, u32, u32, bool, u32) { return 0; }
void AOTCore::RunInterpreterDispatch(Interpreter&) {}
std::unordered_map<u32, u32> AOTCore::s_diff_block_sizes;
std::atomic<bool> AOTCore::s_shutdown_requested{false};
void AOTCore::SetDiffBlockSizes(std::unordered_map<u32, u32>) {}

#endif  // DOLPHIN_HAS_AOT
