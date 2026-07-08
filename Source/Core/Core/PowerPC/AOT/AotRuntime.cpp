// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// AOT Runtime: extern "C" helper functions called by AOT-translated PPC code.
// These wrap Dolphin's existing subsystems (MMU, Interpreter, CoreTiming).

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/AOT/AotModuleTracker.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/AOT/AotState.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// Layout contract between AOTState (aot_runtime.h, compiled into every AOT
// library) and PowerPCState. Generated code reads and writes CPU state through
// AOTState, so every field it can touch must sit at the PowerPCState offset.
// If any of these fail, AOT code would silently corrupt state.
#define AOT_ASSERT_FIELD(field, aot_field)                                                         \
  static_assert(offsetof(PowerPC::PowerPCState, field) == offsetof(AOTState, aot_field),          \
                #field " offset mismatch between PowerPCState and AOTState")
AOT_ASSERT_FIELD(pc, pc);
AOT_ASSERT_FIELD(npc, npc);
AOT_ASSERT_FIELD(stored_stack_pointer, stored_stack_pointer);
AOT_ASSERT_FIELD(gather_pipe_ptr, gather_pipe_ptr);
AOT_ASSERT_FIELD(gather_pipe_base_ptr, gather_pipe_base_ptr);
AOT_ASSERT_FIELD(gpr, gpr);
AOT_ASSERT_FIELD(ps, ps);
AOT_ASSERT_FIELD(cr, cr_fields);
AOT_ASSERT_FIELD(msr, msr);
AOT_ASSERT_FIELD(fpscr, fpscr);
AOT_ASSERT_FIELD(feature_flags, feature_flags);
AOT_ASSERT_FIELD(Exceptions, exceptions);
AOT_ASSERT_FIELD(downcount, downcount);
AOT_ASSERT_FIELD(xer_ca, xer_ca);
AOT_ASSERT_FIELD(xer_so_ov, xer_so_ov);
AOT_ASSERT_FIELD(xer_stringctrl, xer_stringctrl);
AOT_ASSERT_FIELD(reserve_address, reserve_address);
AOT_ASSERT_FIELD(reserve, reserve);
AOT_ASSERT_FIELD(pagetable_update_pending, pagetable_update_pending);
AOT_ASSERT_FIELD(m_enable_dcache, m_enable_dcache);
AOT_ASSERT_FIELD(sr, sr);
AOT_ASSERT_FIELD(spr, spr);
#undef AOT_ASSERT_FIELD
// AOTState is deliberately a prefix view: PowerPCState continues with fields
// generated code never touches (TLBs, caches, ...), so no sizeof() equality.
static_assert(sizeof(AOTState) <= sizeof(PowerPC::PowerPCState),
              "AOTState must not extend beyond PowerPCState");

// Single-block mode flag for diff harness: when set, dispatch returns
// immediately without calling any block. Declared in aot_runtime.h; defined
// here as the single authoritative definition.
int aot_single_block_mode = 0;

static PowerPC::PowerPCState& GetPPCState(AOTState* s)
{
  return FromAot(s);
}

// Cached singleton pointers, filled in aot_init_fast_mem() (called from AOTCore::Init()
// before any block runs, same lifecycle as s_ram_ptr below). Core::System::GetInstance()
// is a function-local static whose thread-safe guard costs an atomic load on EVERY call —
// and every delegated FP/PS/psq op funnels through it. System is a never-destroyed
// singleton, so the cached addresses are stable. Lazy fallback keeps pre-init calls
// correct.
static Core::System* s_system = nullptr;
static Interpreter* s_interpreter = nullptr;
static PowerPC::MMU* s_mmu = nullptr;

static Core::System& GetSystem()
{
  if (!s_system)
    s_system = &Core::System::GetInstance();
  return *s_system;
}

static PowerPC::MMU& GetMMU()
{
  if (!s_mmu)
    s_mmu = &GetSystem().GetMMU();
  return *s_mmu;
}

// Helper: construct a UGeckoInstruction with register fields for interpreter dispatch
static UGeckoInstruction MakeFPInst(int fd, int fa, int fb, int fc = 0, int rc = 0)
{
  UGeckoInstruction inst{};
  inst.FD = fd;
  inst.FA = fa;
  inst.FB = fb;
  inst.FC = fc;
  inst.Rc = rc;
  return inst;
}

static UGeckoInstruction MakeCRInst(int crfd, int fa, int fb)
{
  UGeckoInstruction inst{};
  inst.CRFD = crfd;
  inst.FA = fa;
  inst.FB = fb;
  return inst;
}

// ============================================================================
// Fast memory access
//
// For addresses in main RAM (0x80000000-0x81800000 and mirrors), we bypass
// the full MMU translation and read/write directly from the RAM buffer.
// This is equivalent to the JIT's "slow path" (without fastmem signal handlers)
// and eliminates 5+ levels of function call indirection per memory access.
//
// RAM pointer and size are cached at init time to avoid repeated global lookups.
// ============================================================================

static u8* s_ram_ptr = nullptr;
static u32 s_ram_size = 0;
static u8* s_l1_ptr = nullptr;
static u32 s_l1_size = 0;

// Interpreter fallback tracking (enabled by AOT_TRACK_FALLBACKS=1)
static bool s_track_fallbacks = false;
static std::unordered_map<u32, u64> s_fallback_counts;

// Check if address is in main RAM (cached or uncached mirror)
static inline bool IsRAMAddress(u32 addr)
{
  // 0x80000000-0x81800000 (cached) or 0xC0000000-0xC1800000 (uncached) only.
  // Low-memory (0x0xxxxxxx) and 0x4xxxxxxx accesses must take the slow path:
  // with MSR.DR=1 the interpreter raises a DSI for them (BAT miss), so the fast
  // path must not silently satisfy them from RAM.
  return ((addr & ~0x40000000u) - 0x80000000u) < s_ram_size;
}

// Resolve an effective address to a host pointer for fast-path access: main RAM
// (cached or uncached mirror) or the locked L1 cache at its conventional
// 0xE0000000 mapping ("Locked L1 technically doesn't have a fixed address, but
// games all use 0xE0000000" — MMU.cpp; the SDK's LCEnable sets up the identity
// DBAT). Mirrors MMU::WriteToHardware's L1 branch, which is a plain memcpy into
// the L1 buffer. Returns null for everything else (MMIO, EFB, gather pipe,
// unmapped) → slow path.
static inline u8* FastMemHostPtr(u32 addr)
{
  if (IsRAMAddress(addr))
    return s_ram_ptr + (addr & 0x3FFFFFFFu);
  // The +8 guard keeps the largest fast-path access (paired u32) inside the
  // small L1 buffer; the final few bytes fall back to the checked slow path.
  if (s_l1_ptr != nullptr && (addr >> 28) == 0xE && addr + 8 <= 0xE0000000u + s_l1_size)
    return s_l1_ptr + (addr & 0x0FFFFFFFu);
  return nullptr;
}

extern "C"
{

// ============================================================================
// Init — cache RAM pointer and size to avoid repeated global lookups
// ============================================================================

// RAM fast-path descriptor exported to generated code: aot_runtime.h inlines
// the RAM fast path into every block and calls the aot_*_slow functions below
// for everything else. AotFastMem itself is defined in aot_runtime.h.
AotFastMem aot_fast_mem = {nullptr, 0};

void aot_init_fast_mem()
{
  s_system = &Core::System::GetInstance();
  s_interpreter = &s_system->GetInterpreter();
  s_mmu = &s_system->GetMMU();
  s_ram_ptr = s_system->GetMemory().GetRAM();
  s_ram_size = s_system->GetMemory().GetRamSizeReal();
  s_l1_ptr = s_system->GetMemory().GetL1Cache();
  s_l1_size = s_system->GetMemory().GetL1CacheSize();
  aot_fast_mem.ram = s_ram_ptr;
  aot_fast_mem.size = s_ram_size;
}

// Counterpart to aot_init_fast_mem, called from AOTCore::Shutdown. The RAM/L1
// buffers are reallocated on the next boot, so the cached pointers must not
// survive into a second game's session.
void aot_shutdown()
{
  s_system = nullptr;
  s_interpreter = nullptr;
  s_mmu = nullptr;
  s_ram_ptr = nullptr;
  s_ram_size = 0;
  s_l1_ptr = nullptr;
  s_l1_size = 0;
  aot_fast_mem.ram = nullptr;
  aot_fast_mem.size = 0;
  s_track_fallbacks = false;
  s_fallback_counts.clear();
}

// ============================================================================
// Memory access — slow paths only (MMIO, EFB, locked cache, gather pipe).
// The RAM fast path is inlined into generated code by the header template; these
// delegate to Dolphin's general memory path, which also handles RAM correctly,
// so a pre-init call (aot_fast_mem.size == 0) degrades to correct-but-slow.
// ============================================================================

// The locked L1 cache is checked here rather than in the inlined header fast
// path: LC traffic is bursty (video decode, LC-DMA staging) so one call's
// overhead is fine, and it keeps the generated-code ABI unchanged.
uint32_t aot_read_u8_slow(AOTState* s, uint32_t addr)
{
  if (const u8* p = FastMemHostPtr(addr))
    return *p;
  return PowerPC::ReadFromJit<u8>(GetMMU(), addr);
}

uint32_t aot_read_u16_slow(AOTState* s, uint32_t addr)
{
  if (const u8* p = FastMemHostPtr(addr))
  {
    u16 v;
    std::memcpy(&v, p, sizeof(v));
    return Common::swap16(v);
  }
  return PowerPC::ReadFromJit<u16>(GetMMU(), addr);
}

uint32_t aot_read_u32_slow(AOTState* s, uint32_t addr)
{
  if (const u8* p = FastMemHostPtr(addr))
  {
    u32 v;
    std::memcpy(&v, p, sizeof(v));
    return Common::swap32(v);
  }
  return PowerPC::ReadFromJit<u32>(GetMMU(), addr);
}

uint64_t aot_read_u64_slow(AOTState* s, uint32_t addr)
{
  if (const u8* p = FastMemHostPtr(addr))
  {
    u64 v;
    std::memcpy(&v, p, sizeof(v));
    return Common::swap64(v);
  }
  return PowerPC::ReadFromJit<u64>(GetMMU(), addr);
}

void aot_write_u8_slow(AOTState* s, uint32_t val, uint32_t addr)
{
  if (u8* p = FastMemHostPtr(addr))
  {
    *p = static_cast<u8>(val);
    return;
  }
  PowerPC::WriteFromJit<u8>(GetMMU(), static_cast<u8>(val), addr);
}

void aot_write_u16_slow(AOTState* s, uint32_t val, uint32_t addr)
{
  if (u8* p = FastMemHostPtr(addr))
  {
    const u16 v = Common::swap16(static_cast<u16>(val));
    std::memcpy(p, &v, sizeof(v));
    return;
  }
  PowerPC::WriteFromJit<u16>(GetMMU(), static_cast<u16>(val), addr);
}

void aot_write_u16_br_slow(AOTState* s, uint32_t val, uint32_t addr)
{
  if (u8* p = FastMemHostPtr(addr))
  {
    const u16 v = static_cast<u16>(val);  // no swap — byte-reversed store
    std::memcpy(p, &v, sizeof(v));
    return;
  }
  GetMMU().Write_U16_Swap(val, addr);
}

void aot_write_u32_slow(AOTState* s, uint32_t val, uint32_t addr)
{
  if (u8* p = FastMemHostPtr(addr))
  {
    const u32 v = Common::swap32(val);
    std::memcpy(p, &v, sizeof(v));
    return;
  }
  PowerPC::WriteFromJit<u32>(GetMMU(), val, addr);
}

void aot_write_u64_slow(AOTState* s, uint64_t val, uint32_t addr)
{
  if (u8* p = FastMemHostPtr(addr))
  {
    const u64 v = Common::swap64(val);
    std::memcpy(p, &v, sizeof(v));
    return;
  }
  PowerPC::WriteFromJit<u64>(GetMMU(), val, addr);
}

// ============================================================================
// Interpreter fallback tracking
// ============================================================================

void aot_enable_fallback_tracking()
{
  s_track_fallbacks = true;
  s_fallback_counts.clear();
}

void aot_dump_fallback_stats()
{
  if (!s_track_fallbacks || s_fallback_counts.empty())
    return;

  std::vector<std::pair<u32, u64>> sorted(s_fallback_counts.begin(), s_fallback_counts.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  u64 total = 0;
  for (const auto& [pc, count] : sorted)
    total += count;

  fmt::print(stderr, "\n=== AOT Interpreter Fallback Stats ===\n");
  fmt::print(stderr, "Total fallbacks: {}\n", total);
  fmt::print(stderr, "Unique PCs: {}\n\n", sorted.size());

  const size_t limit = std::min<size_t>(sorted.size(), 50);
  for (size_t i = 0; i < limit; i++)
  {
    const u32 pc = sorted[i].first;
    const u64 count = sorted[i].second;
    std::string opname = "???";
    if (s_ram_ptr && IsRAMAddress(pc))
    {
      u32 inst_word;
      std::memcpy(&inst_word, &s_ram_ptr[pc & 0x3FFFFFFF], sizeof(u32));
      inst_word = Common::swap32(inst_word);
      UGeckoInstruction inst(inst_word);
      const GekkoOPInfo* info = PPCTables::GetOpInfo(inst, pc);
      if (info)
        opname = info->opname;
    }
    fmt::print(stderr, "  {:>12} hits  PC={:#010x}  {}\n", count, pc, opname);
  }
  if (sorted.size() > limit)
    fmt::print(stderr, "  ... and {} more unique PCs\n", sorted.size() - limit);
  fmt::print(stderr, "======================================\n\n");

  s_fallback_counts.clear();
  s_track_fallbacks = false;
}

// ============================================================================
// Interpreter fallback
// ============================================================================

void aot_interpreter_single_step(AOTState* s)
{
  auto& ppc_state = GetPPCState(s);

  if (s_track_fallbacks)
    s_fallback_counts[ppc_state.pc]++;

  auto& system = GetSystem();

  auto& interpreter = system.GetInterpreter();

  ppc_state.npc = ppc_state.pc + 4;
  int cycles = interpreter.SingleStepInner();
  ppc_state.pc = ppc_state.npc;
  ppc_state.downcount -= cycles;
}

// ============================================================================
// System
// ============================================================================

void aot_sc(AOTState* s)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.Exceptions |= EXCEPTION_SYSCALL;
  GetSystem().GetPowerPC().CheckExceptions();
}

// Returns 1 if FPU is available (MSR.FP=1), 0 if not.
// When FPU is unavailable, triggers EXCEPTION_FPU_UNAVAILABLE and sets PC.
int aot_check_fpu(AOTState* s, uint32_t pc)
{
  auto& ppc_state = GetPPCState(s);
  if (ppc_state.msr.FP)
    return 1;
  ppc_state.pc = pc;
  ppc_state.npc = pc;
  ppc_state.Exceptions |= EXCEPTION_FPU_UNAVAILABLE;
  GetSystem().GetPowerPC().CheckExceptions();
  return 0;
}

void aot_msr_updated(AOTState* s)
{
  // Lightweight MSR update for AOT — update feature flags and page table,
  // but skip JitInterface::UpdateMembase() which triggers expensive BAT
  // remapping that the AOT doesn't use (it accesses RAM via GetRAMPtr()).
  auto& ppc_state = GetPPCState(s);
  ppc_state.feature_flags = static_cast<CPUEmuFeatureFlags>(
      (ppc_state.feature_flags & FEATURE_FLAG_PERFMON) | ((ppc_state.msr.Hex >> 4) & 0x3));

  if (ppc_state.msr.DR && ppc_state.pagetable_update_pending)
    GetSystem().GetMMU().PageTableUpdated();
}

void aot_rfi(AOTState* s)
{
  auto& ppc_state = GetPPCState(s);
  const u32 mask = 0x87C0FFFF;
  const u32 clearMSR13 = 0xFFFBFFFF;
  ppc_state.msr.Hex = ((ppc_state.msr.Hex & ~mask) | (SRR1(ppc_state) & mask)) & clearMSR13;
  ppc_state.pc = SRR0(ppc_state);
  ppc_state.npc = ppc_state.pc;
  aot_msr_updated(s);
}

void aot_mtmsr(AOTState* s, uint32_t val)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.msr.Hex = val;
  aot_msr_updated(s);  // Lightweight — no BAT remapping
  GetSystem().GetPowerPC().CheckExceptions();
}

void aot_sr_updated(AOTState* s)
{
  GetSystem().GetMMU().SRUpdated();
}

int aot_twi(AOTState* s, uint32_t TO, int32_t a, int32_t b)
{
  if ((a < b && (TO & 0x10) != 0) || (a > b && (TO & 0x08) != 0) ||
      (a == b && (TO & 0x04) != 0) || ((u32)a < (u32)b && (TO & 0x02) != 0) ||
      ((u32)a > (u32)b && (TO & 0x01) != 0))
  {
    auto& ppc_state = GetPPCState(s);
    ppc_state.Exceptions |= EXCEPTION_PROGRAM;
    ppc_state.spr[SPR_SRR1] = static_cast<u32>(ProgramExceptionCause::Trap);
    GetSystem().GetPowerPC().CheckExceptions();
    return 1;
  }
  return 0;
}

// ============================================================================
// SPR access
// ============================================================================

uint32_t aot_mfspr_special(AOTState* s, uint32_t spr_index)
{
  auto& ppc_state = GetPPCState(s);
  // Handle SPRs with side effects
  switch (spr_index)
  {
  case SPR_TL:
  case SPR_TU:
  {
    auto& system = GetSystem();
    system.GetPowerPC().WriteFullTimeBaseValue(system.GetSystemTimers().GetFakeTimeBase());
    return ppc_state.spr[spr_index];
  }
  case SPR_DEC:
  {
    if ((ppc_state.spr[SPR_DEC] & 0x80000000) == 0)
      ppc_state.spr[SPR_DEC] = GetSystem().GetSystemTimers().GetFakeDecrementer();
    return ppc_state.spr[SPR_DEC];
  }
  case SPR_WPAR:
  {
    // BNE (buffer not empty) is in bit 0, matching the interpreter's behavior.
    u32 val = ppc_state.spr[spr_index];
    if (GetSystem().GetGPFifo().IsBNE())
      val |= 1;
    else
      val &= ~1;
    return val;
  }
  default:
    return ppc_state.spr[spr_index];
  }
}

void aot_mtspr_special(AOTState* s, uint32_t spr_index, uint32_t val)
{
  auto& ppc_state = GetPPCState(s);
  u32 old_value = ppc_state.spr[spr_index];
  ppc_state.spr[spr_index] = val;

  switch (spr_index)
  {
  case SPR_DEC:
    GetSystem().GetSystemTimers().DecrementerSet();
    break;

  case SPR_HID0:
  {
    UReg_HID0 old_hid0;
    old_hid0.Hex = old_value;
    if (HID0(ppc_state).ICFI)
    {
      HID0(ppc_state).ICFI = 0;
      ppc_state.iCache.Reset(GetSystem().GetJitInterface());
    }
    break;
  }

  case SPR_HID2:
    // Only lower half is modifiable, except DMAQL field
    ppc_state.spr[spr_index] = (ppc_state.spr[spr_index] & 0xF0FF0000) | (old_value & 0x0F000000);
    break;

  case SPR_HID4:
    if (old_value != ppc_state.spr[spr_index])
    {
      GetSystem().GetMMU().IBATUpdated();
      GetSystem().GetMMU().DBATUpdated();
    }
    break;

  case SPR_WPAR:
    GetSystem().GetGPFifo().ResetGatherPipe();
    break;

  // DBAT registers — must rebuild BAT lookup table when changed
  case SPR_DBAT0U: case SPR_DBAT0L: case SPR_DBAT1U: case SPR_DBAT1L:
  case SPR_DBAT2U: case SPR_DBAT2L: case SPR_DBAT3U: case SPR_DBAT3L:
  case SPR_DBAT4U: case SPR_DBAT4L: case SPR_DBAT5U: case SPR_DBAT5L:
  case SPR_DBAT6U: case SPR_DBAT6L: case SPR_DBAT7U: case SPR_DBAT7L:
    if (old_value != ppc_state.spr[spr_index])
      GetSystem().GetMMU().DBATUpdated();
    break;

  // IBAT registers — must rebuild BAT lookup table when changed
  case SPR_IBAT0U: case SPR_IBAT0L: case SPR_IBAT1U: case SPR_IBAT1L:
  case SPR_IBAT2U: case SPR_IBAT2L: case SPR_IBAT3U: case SPR_IBAT3L:
  case SPR_IBAT4U: case SPR_IBAT4L: case SPR_IBAT5U: case SPR_IBAT5L:
  case SPR_IBAT6U: case SPR_IBAT6L: case SPR_IBAT7U: case SPR_IBAT7L:
    if (old_value != ppc_state.spr[spr_index])
      GetSystem().GetMMU().IBATUpdated();
    break;

  // Locked cache DMA — write to DMAL with DMA_T set triggers transfer
  case SPR_DMAL:
  {
    if (DMAL(ppc_state).DMA_T)
    {
      auto& mmu = GetMMU();
      const u32 mem_address = DMAU(ppc_state).MEM_ADDR << 5;
      const u32 cache_address = DMAL(ppc_state).LC_ADDR << 5;
      u32 length = ((DMAU(ppc_state).DMA_LEN_U << 2) | DMAL(ppc_state).DMA_LEN_L);
      if (length == 0)
        length = 128;
      if (DMAL(ppc_state).DMA_LD)
        mmu.DMA_MemoryToLC(cache_address, mem_address, length);
      else
        mmu.DMA_LCToMemory(mem_address, cache_address, length);
    }
    DMAL(ppc_state).DMA_T = 0;
    break;
  }

  default:
    break;
  }
}

uint32_t aot_mftb(AOTState* s, uint32_t spr_encoded)
{
  // spr_encoded is the raw TBR field with upper/lower 5-bit halves swapped.
  // Decode to actual SPR number: 268=TBL, 269=TBU
  u32 spr = ((spr_encoded & 0x1F) << 5) | ((spr_encoded >> 5) & 0x1F);
  return aot_mfspr_special(s, spr);
}

// ============================================================================
// CR helpers
// ============================================================================

uint32_t aot_mfcr(AOTState* s)
{
  auto& ppc_state = GetPPCState(s);
  return ppc_state.cr.Get();
}

void aot_mtcrf(AOTState* s, uint32_t mask, uint32_t rs_reg)
{
  auto& ppc_state = GetPPCState(s);
  u32 val = ppc_state.gpr[rs_reg];
  for (int i = 0; i < 8; i++)
  {
    if (mask & (1 << (7 - i)))
    {
      ppc_state.cr.SetField(i, (val >> (28 - i * 4)) & 0xF);
    }
  }
}

void aot_cr_logical(AOTState* s, int crbD, int crbA, int crbB, const char* op)
{
  auto& ppc_state = GetPPCState(s);
  u32 a = ppc_state.cr.GetBit(crbA);
  u32 b = ppc_state.cr.GetBit(crbB);
  u32 result;

  // Decode the operation string
  if (op[0] == '&' && op[1] == 0) result = a & b;
  else if (op[0] == '|' && op[1] == 0) result = a | b;
  else if (op[0] == '^' && op[1] == 0) result = a ^ b;
  else if (op[0] == '~' && op[1] == '^') result = ~(a ^ b) & 1;  // creqv (XNOR)
  else if (op[0] == '&' && op[1] == '~') result = a & (~b & 1);  // crandc
  else if (op[0] == '|' && op[1] == '~') result = a | (~b & 1);  // crorc
  else if (op[0] == '~' && op[1] == '&') result = ~(a & b) & 1;  // crnand
  else if (op[0] == '~' && op[1] == '|') result = ~(a | b) & 1;  // crnor
  else result = 0;

  ppc_state.cr.SetBit(crbD, result & 1);
}

// ============================================================================
// FP conversion
// ============================================================================

uint64_t aot_convert_to_double(uint32_t single_bits)
{
  // Use Dolphin's exact ConvertToDouble implementation (Gekko-accurate bit manipulation)
  return ConvertToDouble(single_bits);
}

uint32_t aot_convert_to_single(uint64_t double_bits)
{
  // Double → single truncation (used by stfs)
  // Must use Gekko-accurate bit manipulation, NOT IEEE rounding.
  return ConvertToSingle(double_bits);
}

// ============================================================================
// FP arithmetic — all delegate to interpreter methods
// ============================================================================

// Helper: get the interpreter instance (cached — see s_interpreter above)
static Interpreter& GetInterpreter()
{
  if (!s_interpreter)
    s_interpreter = &GetSystem().GetInterpreter();
  return *s_interpreter;
}

#define FP_IMPL_3(name, interp_method) \
void name(AOTState* s, int fd, int fa, int fb) { \
  UGeckoInstruction inst = MakeFPInst(fd, fa, fb); \
  Interpreter::interp_method(GetInterpreter(), inst); \
}

#define FP_IMPL_3C(name, interp_method) \
void name(AOTState* s, int fd, int fa, int fc) { \
  UGeckoInstruction inst = MakeFPInst(fd, fa, 0, fc); \
  Interpreter::interp_method(GetInterpreter(), inst); \
}

#define FP_IMPL_4(name, interp_method) \
void name(AOTState* s, int fd, int fa, int fc, int fb) { \
  UGeckoInstruction inst = MakeFPInst(fd, fa, fb, fc); \
  Interpreter::interp_method(GetInterpreter(), inst); \
}

#define FP_IMPL_2(name, interp_method) \
void name(AOTState* s, int fd, int fb) { \
  UGeckoInstruction inst = MakeFPInst(fd, 0, fb); \
  Interpreter::interp_method(GetInterpreter(), inst); \
}

#define FP_CMP(name, interp_method) \
void name(AOTState* s, int crfd, int fa, int fb) { \
  UGeckoInstruction inst = MakeCRInst(crfd, fa, fb); \
  Interpreter::interp_method(GetInterpreter(), inst); \
}

// Double-precision
FP_IMPL_3(aot_faddx, faddx)
FP_IMPL_3(aot_fsubx, fsubx)
FP_IMPL_3C(aot_fmulx, fmulx)
FP_IMPL_3(aot_fdivx, fdivx)
FP_IMPL_4(aot_fmaddx, fmaddx)
FP_IMPL_4(aot_fmsubx, fmsubx)
FP_IMPL_4(aot_fnmsubx, fnmsubx)
FP_IMPL_4(aot_fnmaddx, fnmaddx)

// Single-precision
FP_IMPL_3(aot_faddsx, faddsx)
FP_IMPL_3(aot_fsubsx, fsubsx)
FP_IMPL_3C(aot_fmulsx, fmulsx)
FP_IMPL_3(aot_fdivsx, fdivsx)
FP_IMPL_4(aot_fmaddsx, fmaddsx)
FP_IMPL_4(aot_fmsubsx, fmsubsx)
FP_IMPL_4(aot_fnmsubsx, fnmsubsx)
FP_IMPL_4(aot_fnmaddsx, fnmaddsx)

// Comparison
FP_CMP(aot_fcmpu, fcmpu)
FP_CMP(aot_fcmpo, fcmpo)

// Conversion/misc
FP_IMPL_2(aot_frspx, frspx)
FP_IMPL_2(aot_fctiwx, fctiwx)
FP_IMPL_2(aot_fctiwzx, fctiwzx)
FP_IMPL_2(aot_fresx, fresx)
FP_IMPL_2(aot_frsqrtex, frsqrtex)

void aot_fselx(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  double a_val = ppc_state.ps[fa].PS0AsDouble();
  ppc_state.ps[fd].SetPS0(a_val >= -0.0 ? ppc_state.ps[fc].PS0AsU64() : ppc_state.ps[fb].PS0AsU64());
}

void aot_mtfsf(AOTState* s, int fm, int fb)
{
  UGeckoInstruction inst{};
  inst.FM = fm;
  inst.FB = fb;
  Interpreter::mtfsfx(GetInterpreter(), inst);
}

void aot_mtfsfi(AOTState* s, int crfd, int imm)
{
  UGeckoInstruction inst{};
  inst.CRFD = crfd;
  inst.RB = imm;
  Interpreter::mtfsfix(GetInterpreter(), inst);
}

void aot_mcrfs(AOTState* s, int crfd, int crfs)
{
  UGeckoInstruction inst{};
  inst.CRFD = crfd;
  inst.CRFS = crfs;
  Interpreter::mcrfs(GetInterpreter(), inst);
}

// ============================================================================
// Paired singles
// ============================================================================

// PS arithmetic — transcribed from Interpreter_Paired.cpp, dropping only the
// interpreter-method dispatch and UGeckoInstruction reconstruction. The Rc=1
// (UpdateCR1) branch is omitted because these wrappers always passed Rc=0;
// keep that in sync with the emitter. div/res/rsqrte stay delegated — they're
// rare and their exception logic is more involved.
void aot_ps_add(AOTState* s, int fd, int fa, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];

  const float ps0 =
      ForceSingle(ppc_state.fpscr, NI_add(ppc_state, a.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 =
      ForceSingle(ppc_state.fpscr, NI_add(ppc_state, a.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_sub(AOTState* s, int fd, int fa, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];

  const float ps0 =
      ForceSingle(ppc_state.fpscr, NI_sub(ppc_state, a.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 =
      ForceSingle(ppc_state.fpscr, NI_sub(ppc_state, a.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_mul(AOTState* s, int fd, int fa, int fc)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& c = ppc_state.ps[fc];

  const double c0 = Force25Bit(c.PS0AsDouble());
  const double c1 = Force25Bit(c.PS1AsDouble());

  const float ps0 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS0AsDouble(), c0).value);
  const float ps1 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS1AsDouble(), c1).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

FP_IMPL_3(aot_ps_div, ps_div)

void aot_ps_madd(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_msub(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(
      ppc_state.fpscr,
      NI_msub<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 = ForceSingle(
      ppc_state.fpscr,
      NI_msub<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_nmadd(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float tmp0 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float tmp1 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  const float ps0 = std::isnan(tmp0) ? tmp0 : -tmp0;
  const float ps1 = std::isnan(tmp1) ? tmp1 : -tmp1;

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_nmsub(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float tmp0 = ForceSingle(
      ppc_state.fpscr,
      NI_msub<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float tmp1 = ForceSingle(
      ppc_state.fpscr,
      NI_msub<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  const float ps0 = std::isnan(tmp0) ? tmp0 : -tmp0;
  const float ps1 = std::isnan(tmp1) ? tmp1 : -tmp1;

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_sum0(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 =
      ForceSingle(ppc_state.fpscr, NI_add(ppc_state, a.PS0AsDouble(), b.PS1AsDouble()).value);
  const float ps1 = ForceSingle(ppc_state.fpscr, c.PS1AsDouble());

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_sum1(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(ppc_state.fpscr, c.PS0AsDouble());
  const float ps1 =
      ForceSingle(ppc_state.fpscr, NI_add(ppc_state, a.PS0AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps1);
}

void aot_ps_muls0(AOTState* s, int fd, int fa, int fc)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& c = ppc_state.ps[fc];

  const double c0 = Force25Bit(c.PS0AsDouble());
  const float ps0 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS0AsDouble(), c0).value);
  const float ps1 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS1AsDouble(), c0).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_muls1(AOTState* s, int fd, int fa, int fc)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& c = ppc_state.ps[fc];

  const double c1 = Force25Bit(c.PS1AsDouble());
  const float ps0 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS0AsDouble(), c1).value);
  const float ps1 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS1AsDouble(), c1).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_madds0(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS1AsDouble(), c.PS0AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_madds1(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS1AsDouble(), b.PS0AsDouble()).value);
  const float ps1 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

void aot_ps_sel(AOTState* s, int fd, int fa, int fc, int fb)
{
  auto& ppc_state = GetPPCState(s);
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  ppc_state.ps[fd].SetBoth(a.PS0AsDouble() >= -0.0 ? c.PS0AsDouble() : b.PS0AsDouble(),
                           a.PS1AsDouble() >= -0.0 ? c.PS1AsDouble() : b.PS1AsDouble());
}

FP_IMPL_2(aot_ps_res, ps_res)
FP_IMPL_2(aot_ps_rsqrte, ps_rsqrte)

// PS moves — must operate on BOTH ps0 AND ps1 (unlike scalar fneg/fabs/fnabs)
void aot_ps_neg(AOTState* s, int fd, int fb)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.ps[fd].SetBoth(ppc_state.ps[fb].PS0AsU64() ^ (UINT64_C(1) << 63),
                            ppc_state.ps[fb].PS1AsU64() ^ (UINT64_C(1) << 63));
}

void aot_ps_mr(AOTState* s, int fd, int fb)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.ps[fd] = ppc_state.ps[fb];
}

void aot_ps_abs(AOTState* s, int fd, int fb)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.ps[fd].SetBoth(ppc_state.ps[fb].PS0AsU64() & ~(UINT64_C(1) << 63),
                            ppc_state.ps[fb].PS1AsU64() & ~(UINT64_C(1) << 63));
}

void aot_ps_nabs(AOTState* s, int fd, int fb)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.ps[fd].SetBoth(ppc_state.ps[fb].PS0AsU64() | (UINT64_C(1) << 63),
                            ppc_state.ps[fb].PS1AsU64() | (UINT64_C(1) << 63));
}

// PS merge
void aot_ps_merge00(AOTState* s, int fd, int fa, int fb)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.ps[fd].SetBoth(ppc_state.ps[fa].PS0AsU64(), ppc_state.ps[fb].PS0AsU64());
}
void aot_ps_merge01(AOTState* s, int fd, int fa, int fb)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.ps[fd].SetBoth(ppc_state.ps[fa].PS0AsU64(), ppc_state.ps[fb].PS1AsU64());
}
void aot_ps_merge10(AOTState* s, int fd, int fa, int fb)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.ps[fd].SetBoth(ppc_state.ps[fa].PS1AsU64(), ppc_state.ps[fb].PS0AsU64());
}
void aot_ps_merge11(AOTState* s, int fd, int fa, int fb)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.ps[fd].SetBoth(ppc_state.ps[fa].PS1AsU64(), ppc_state.ps[fb].PS1AsU64());
}

// PS comparison
FP_CMP(aot_ps_cmpu0, ps_cmpu0)
FP_CMP(aot_ps_cmpo0, ps_cmpo0)
FP_CMP(aot_ps_cmpu1, ps_cmpu1)
FP_CMP(aot_ps_cmpo1, ps_cmpo1)

}  // close extern "C" — the psq fast-path helpers below are C++ (templates)

// ============================================================================
// PS quantized load/store fast path
//
// Exact transcriptions of Helper_Dequantize / Helper_Quantize from
// Interpreter_LoadStorePaired.cpp with the MMU access replaced by the direct
// RAM path (same address rule as IsRAMAddress). The scale tables are the
// interpreter's own (external linkage granted there), so scaling stays
// bit-identical. The helpers return false to delegate to the interpreter:
// non-RAM address (MMIO/EFB/locked cache — also anything that could DSI) or
// an invalid GQR quantize type. The D-form wrappers additionally delegate when
// HID2.LSQE=0 so the program exception comes from the one authoritative
// implementation.
// ============================================================================

extern const float m_dequantizeTable[];  // defined in Interpreter_LoadStorePaired.cpp
extern const float m_quantizeTable[];

namespace
{
template <typename U>
inline U FastRead(const u8* p)
{
  U v;
  std::memcpy(&v, p, sizeof(U));
  if constexpr (sizeof(U) == 2)
    v = Common::swap16(v);
  else if constexpr (sizeof(U) == 4)
    v = Common::swap32(v);
  else if constexpr (sizeof(U) == 8)
    v = Common::swap64(v);
  return v;
}

template <typename U>
inline void FastWrite(U v, u8* p)
{
  if constexpr (sizeof(U) == 2)
    v = Common::swap16(v);
  else if constexpr (sizeof(U) == 4)
    v = Common::swap32(v);
  else if constexpr (sizeof(U) == 8)
    v = Common::swap64(v);
  std::memcpy(p, &v, sizeof(U));
}

// Pair access reads/writes one value of twice the element width, exactly like
// the interpreter's ReadPair/WritePair (element 0 in the high half).
template <typename U>
inline std::pair<U, U> FastReadPair(const u8* p)
{
  if constexpr (sizeof(U) == 1)
  {
    const u16 val = FastRead<u16>(p);
    return {U(val >> 8), U(val)};
  }
  else if constexpr (sizeof(U) == 2)
  {
    const u32 val = FastRead<u32>(p);
    return {U(val >> 16), U(val)};
  }
  else
  {
    const u64 val = FastRead<u64>(p);
    return {U(val >> 32), U(val)};
  }
}

template <typename U>
inline void FastWritePair(U val1, U val2, u8* p)
{
  if constexpr (sizeof(U) == 1)
    FastWrite<u16>(u16((u16{val1} << 8) | u16{val2}), p);
  else if constexpr (sizeof(U) == 2)
    FastWrite<u32>((u32{val1} << 16) | u32{val2}, p);
  else
    FastWrite<u64>((u64{val1} << 32) | u64{val2}, p);
}

template <typename SType>
SType FastScaleAndClamp(double ps, u32 st_scale)
{
  const float conv_ps = float(ps) * m_quantizeTable[st_scale];
  constexpr float min = float(std::numeric_limits<SType>::min());
  constexpr float max = float(std::numeric_limits<SType>::max());

  return SType(std::clamp(conv_ps, min, max));
}

template <typename T>
std::pair<double, double> FastLoadAndDequantize(const u8* p, u32 instW, u32 ld_scale)
{
  using U = std::make_unsigned_t<T>;

  float ps0, ps1;
  if (instW != 0)
  {
    const U value = FastRead<U>(p);
    ps0 = float(T(value)) * m_dequantizeTable[ld_scale];
    ps1 = 1.0f;
  }
  else
  {
    const auto [first, second] = FastReadPair<U>(p);
    ps0 = float(T(first)) * m_dequantizeTable[ld_scale];
    ps1 = float(T(second)) * m_dequantizeTable[ld_scale];
  }
  // ps0 and ps1 always contain finite and normal numbers, so casting to double is safe
  return {static_cast<double>(ps0), static_cast<double>(ps1)};
}

template <typename T>
void FastQuantizeAndStore(double ps0, double ps1, u8* p, u32 instW, u32 st_scale)
{
  using U = std::make_unsigned_t<T>;

  const U conv_ps0 = U(FastScaleAndClamp<T>(ps0, st_scale));
  if (instW != 0)
  {
    FastWrite<U>(conv_ps0, p);
  }
  else
  {
    const U conv_ps1 = U(FastScaleAndClamp<T>(ps1, st_scale));
    FastWritePair<U>(conv_ps0, conv_ps1, p);
  }
}

bool FastDequantize(PowerPC::PowerPCState& ppcs, u32 addr, u32 instI, u32 instRD, u32 instW)
{
  const u8* p = FastMemHostPtr(addr);
  if (p == nullptr)
    return false;

  const UGQR gqr(ppcs.spr[SPR_GQR0 + instI]);
  const u32 ld_scale = gqr.ld_scale;

  double ps0 = 0.0;
  double ps1 = 0.0;

  switch (gqr.ld_type)
  {
  case QUANTIZE_FLOAT:
    if (instW != 0)
    {
      const u32 value = FastRead<u32>(p);
      ps0 = std::bit_cast<double>(ConvertToDouble(value));
      ps1 = 1.0;
    }
    else
    {
      const auto [first, second] = FastReadPair<u32>(p);
      ps0 = std::bit_cast<double>(ConvertToDouble(first));
      ps1 = std::bit_cast<double>(ConvertToDouble(second));
    }
    break;

  case QUANTIZE_U8:
    std::tie(ps0, ps1) = FastLoadAndDequantize<u8>(p, instW, ld_scale);
    break;

  case QUANTIZE_U16:
    std::tie(ps0, ps1) = FastLoadAndDequantize<u16>(p, instW, ld_scale);
    break;

  case QUANTIZE_S8:
    std::tie(ps0, ps1) = FastLoadAndDequantize<s8>(p, instW, ld_scale);
    break;

  case QUANTIZE_S16:
    std::tie(ps0, ps1) = FastLoadAndDequantize<s16>(p, instW, ld_scale);
    break;

  default:  // QUANTIZE_INVALID1-3: let the interpreter's ASSERT handle it
    return false;
  }

  ppcs.ps[instRD].SetBoth(ps0, ps1);
  return true;
}

bool FastQuantize(PowerPC::PowerPCState& ppcs, u32 addr, u32 instI, u32 instRS, u32 instW)
{
  u8* p = FastMemHostPtr(addr);
  if (p == nullptr)
    return false;

  const UGQR gqr(ppcs.spr[SPR_GQR0 + instI]);
  const u32 st_scale = gqr.st_scale;

  const double ps0 = ppcs.ps[instRS].PS0AsDouble();
  const double ps1 = ppcs.ps[instRS].PS1AsDouble();

  switch (gqr.st_type)
  {
  case QUANTIZE_FLOAT:
  {
    const u32 conv_ps0 = ConvertToSingleFTZ(std::bit_cast<u64>(ps0));
    if (instW != 0)
    {
      FastWrite<u32>(conv_ps0, p);
    }
    else
    {
      const u32 conv_ps1 = ConvertToSingleFTZ(std::bit_cast<u64>(ps1));
      FastWritePair<u32>(conv_ps0, conv_ps1, p);
    }
    break;
  }

  case QUANTIZE_U8:
    FastQuantizeAndStore<u8>(ps0, ps1, p, instW, st_scale);
    break;

  case QUANTIZE_U16:
    FastQuantizeAndStore<u16>(ps0, ps1, p, instW, st_scale);
    break;

  case QUANTIZE_S8:
    FastQuantizeAndStore<s8>(ps0, ps1, p, instW, st_scale);
    break;

  case QUANTIZE_S16:
    FastQuantizeAndStore<s16>(ps0, ps1, p, instW, st_scale);
    break;

  default:  // QUANTIZE_INVALID1-3: let the interpreter's ASSERT handle it
    return false;
  }

  return true;
}
}  // namespace

extern "C"
{

// PS quantized load/store — RAM fast path above, interpreter for everything else.
// The u-forms update RA only after a successful access; the fast path cannot
// raise a DSI (RAM addresses only), so the update is unconditional there.
void aot_psq_l(AOTState* s, int fd, int ra, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  auto& ppc_state = GetPPCState(s);
  if (HID2(ppc_state).LSQE != 0)
  {
    const u32 EA = inst.RA ? (ppc_state.gpr[inst.RA] + u32(inst.SIMM_12)) : u32(inst.SIMM_12);
    if (FastDequantize(ppc_state, EA, inst.I, inst.RD, inst.W))
      return;
  }
  Interpreter::psq_l(GetInterpreter(), inst);
}
void aot_psq_lu(AOTState* s, int fd, int ra, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  auto& ppc_state = GetPPCState(s);
  if (HID2(ppc_state).LSQE != 0)
  {
    const u32 EA = ppc_state.gpr[inst.RA] + u32(inst.SIMM_12);
    if (FastDequantize(ppc_state, EA, inst.I, inst.RD, inst.W))
    {
      ppc_state.gpr[inst.RA] = EA;
      return;
    }
  }
  Interpreter::psq_lu(GetInterpreter(), inst);
}
void aot_psq_st(AOTState* s, int fs, int ra, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  auto& ppc_state = GetPPCState(s);
  if (HID2(ppc_state).LSQE != 0)
  {
    const u32 EA = inst.RA ? (ppc_state.gpr[inst.RA] + u32(inst.SIMM_12)) : u32(inst.SIMM_12);
    if (FastQuantize(ppc_state, EA, inst.I, inst.RS, inst.W))
      return;
  }
  Interpreter::psq_st(GetInterpreter(), inst);
}
void aot_psq_stu(AOTState* s, int fs, int ra, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  auto& ppc_state = GetPPCState(s);
  if (HID2(ppc_state).LSQE != 0)
  {
    const u32 EA = ppc_state.gpr[inst.RA] + u32(inst.SIMM_12);
    if (FastQuantize(ppc_state, EA, inst.I, inst.RS, inst.W))
    {
      ppc_state.gpr[inst.RA] = EA;
      return;
    }
  }
  Interpreter::psq_stu(GetInterpreter(), inst);
}
// Indexed forms: no LSQE check, matching the interpreter.
void aot_psq_lx(AOTState* s, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  auto& ppc_state = GetPPCState(s);
  const u32 EA =
      inst.RA ? (ppc_state.gpr[inst.RA] + ppc_state.gpr[inst.RB]) : ppc_state.gpr[inst.RB];
  if (FastDequantize(ppc_state, EA, inst.Ix, inst.RD, inst.Wx))
    return;
  Interpreter::psq_lx(GetInterpreter(), inst);
}
void aot_psq_stx(AOTState* s, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  auto& ppc_state = GetPPCState(s);
  const u32 EA =
      inst.RA ? (ppc_state.gpr[inst.RA] + ppc_state.gpr[inst.RB]) : ppc_state.gpr[inst.RB];
  if (FastQuantize(ppc_state, EA, inst.Ix, inst.RS, inst.Wx))
    return;
  Interpreter::psq_stx(GetInterpreter(), inst);
}
void aot_psq_lux(AOTState* s, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  auto& ppc_state = GetPPCState(s);
  const u32 EA = ppc_state.gpr[inst.RA] + ppc_state.gpr[inst.RB];
  if (FastDequantize(ppc_state, EA, inst.Ix, inst.RD, inst.Wx))
  {
    ppc_state.gpr[inst.RA] = EA;
    return;
  }
  Interpreter::psq_lux(GetInterpreter(), inst);
}
void aot_psq_stux(AOTState* s, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  auto& ppc_state = GetPPCState(s);
  const u32 EA = ppc_state.gpr[inst.RA] + ppc_state.gpr[inst.RB];
  if (FastQuantize(ppc_state, EA, inst.Ix, inst.RS, inst.Wx))
  {
    ppc_state.gpr[inst.RA] = EA;
    return;
  }
  Interpreter::psq_stux(GetInterpreter(), inst);
}

// ============================================================================
// Cache operations
// ============================================================================

// dcbz fast path: MMU::ClearDCacheLine is translate + 8 separate 4-byte
// WriteToHardware calls — for RAM/L1 lines that is exactly a 32-byte zero fill
// (a 32-aligned line can't cross out of either region, and neither region has
// the cache-inhibited write quirk). Video decoders dcbz their output buffers
// constantly, which is what made this loop hot.
static inline bool FastDcbz(uint32_t addr)
{
  const u32 line = addr & ~31u;
  if (u8* p = FastMemHostPtr(line))
  {
    std::memset(p, 0, 32);
    return true;
  }
  return false;
}

void aot_dcbz(AOTState* s, uint32_t addr)
{
  if (FastDcbz(addr))
    return;
  PowerPC::ClearDCacheLineFromJit(GetMMU(), addr & ~31u);
}

void aot_dcbz_l(AOTState* s, uint32_t addr)
{
  auto& ppc_state = GetPPCState(s);
  if (!HID2(ppc_state).LCE)
  {
    GenerateProgramException(ppc_state, ProgramExceptionCause::IllegalInstruction);
    return;
  }
  if (!HID0(ppc_state).DCE)
  {
    GenerateAlignmentException(ppc_state, addr);
    return;
  }
  if (FastDcbz(addr))
    return;
  PowerPC::ClearDCacheLineFromJit(GetMMU(), addr & ~31u);
}

void aot_dcbt(AOTState* s, uint32_t addr)
{
  // Prefetch hint — no-op
}

void aot_icbi(AOTState* s, uint32_t addr)
{
  // Must invalidate the emulated icache exactly like Interpreter::icbi: the interpreter
  // fallback fetches through iCache, and games that load code at runtime (REL modules)
  // reuse heap addresses — a stale line means executing instructions from an unloaded
  // module. There is no JIT block cache to flush in AOT mode, but JitInterface handles that.
  auto& ppc_state = GetPPCState(s);
  auto& system = GetSystem();
  ppc_state.iCache.Invalidate(system.GetMemory(), system.GetJitInterface(), addr);
  // Games icbi after loading/unloading module code; rescan the OS module queue
  // before the next module dispatch.
  AotModuleTracker::MarkDirty();
}

#undef FP_IMPL_3
#undef FP_IMPL_3C
#undef FP_IMPL_4
#undef FP_IMPL_2
#undef FP_CMP

}  // extern "C"
