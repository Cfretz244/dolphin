// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// AOT Runtime: extern "C" helper functions called by AOT-translated PPC code.
// These wrap Dolphin's existing subsystems (MMU, Interpreter, CoreTiming).

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/AOT/AotModuleTracker.h"
#ifdef DOLPHIN_AOT_HARNESS
#include "Core/PowerPC/AOT/AotMmioCapture.h"
#endif
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/Interpreter/Interpreter_PairedTables.h"
#include "Core/PowerPC/Interpreter/Interpreter_PairedUtils.h"
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

// Constants behind the inline gather-pipe path in aot_runtime.h (aot_gp_physical).
static_assert(AOT_BAT_INDEX_SHIFT == PowerPC::BAT_INDEX_SHIFT);
static_assert((1u << AOT_BAT_INDEX_SHIFT) == PowerPC::BAT_PAGE_SIZE);
static_assert(AOT_BAT_MAPPED_BIT == PowerPC::BAT_MAPPED_BIT);
static_assert(AOT_BAT_RESULT_MASK == PowerPC::BAT_RESULT_MASK);
static_assert(AOT_GP_PHYS_PAGE == GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS);
static_assert(AOT_GP_SIZE == GPFifo::GATHER_PIPE_SIZE);
// UReg_MSR's constructor is not constexpr; check the bitfield's position instead.
static_assert(decltype(UReg_MSR::DR)::NumBits() == 1 &&
                  (1u << decltype(UReg_MSR::DR)::StartBit()) == AOT_MSR_DR,
              "AOT_MSR_DR must be exactly MSR.DR");
static_assert(std::is_same_v<PowerPC::BatTable::value_type, uint32_t>,
              "AotFastMem::dbat reads the DBAT table as uint32_t");
// Constants behind the inline mtmsr / FP-available paths in aot_runtime.h
// (aot_mtmsr_fast, aot_fpu_available) and MsrUpdatedFast below.
static_assert(decltype(UReg_MSR::FP)::NumBits() == 1 &&
                  (1u << decltype(UReg_MSR::FP)::StartBit()) == AOT_MSR_FP,
              "AOT_MSR_FP must be exactly MSR.FP");
static_assert(AOT_MSR_DR_BIT == AOT_MSR_DR);
static_assert(decltype(UReg_MSR::IR)::StartBit() == 5 && decltype(UReg_MSR::DR)::StartBit() == 4);
static_assert(FEATURE_FLAG_MSR_DR == 1u << 0 && FEATURE_FLAG_MSR_IR == 1u << 1);
static_assert(AOT_FEATURE_FLAG_PERFMON == FEATURE_FLAG_PERFMON);
static_assert(sizeof(CPUEmuFeatureFlags) == sizeof(uint32_t));

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
static u8* s_exram_ptr = nullptr;  // Wii MEM2 host buffer; null on GC
static u32 s_exram_size = 0;
static u8* s_l1_ptr = nullptr;
static u32 s_l1_size = 0;

// Interpreter fallback tracking (enabled by AOT_TRACK_FALLBACKS=1)
// Expected instruction arrays are immutable generated constants. Cache only
// successful resident comparisons, bounded independently of game coverage.
struct GuardMemo
{
  const u32* words = nullptr;
  u64 generation = 0;
  u32 pc = 0;
  u32 translation = 0;
  u32 count = 0;
  u32 line_count = 0;
  struct Location
  {
    u8 set;
    u8 way;
  };
  std::array<Location, 4> lines{};
};
static std::array<GuardMemo, 16384> s_guard_memos{};
// Trust mode intentionally omits per-fetch cache residency and PLRU emulation.
// It assumes executable modifications are published with explicit invalidation.
// Never cache failures: unknown or newly loaded code still needs identification.
struct TrustedCodeMemo
{
  const u32* words = nullptr;
  u64 generation = 0;
  u32 pc = 0;
  u32 translation = 0;
  u32 count = 0;
};
static std::array<TrustedCodeMemo, 131072> s_trusted_code_memos{};
static bool s_trust_code = false;
// Frontend override for trust-code mode (aot_set_trust_code): -1 = not set
// (AOT_TRUST_CODE env decides), 0 = force off, 1 = force on. iOS cannot set
// env vars, so the bridge sets this before boot. Survives aot_shutdown.
static int s_trust_code_request = -1;
static u64 s_guard_checks = 0;
static u64 s_guard_reuses = 0;

static bool s_track_fallbacks = false;
static std::unordered_map<u32, u64> s_fallback_counts;
// Event counters printed with the fallback report (AOT_TRACK_FALLBACKS=1).
extern "C" int aot_stats_enabled = 0;
extern "C" uint64_t aot_stat_exception_checks = 0;   // Run-loop CheckExceptions calls
extern "C" uint64_t aot_stat_exception_bits[16] = {};  // pending bit histogram at those calls
extern "C" uint64_t aot_stat_exception_masked = 0;   // ... with MSR.EE clear
// s_stat_gp_psq counts only helper-path pipe psq_st: the inline float path in
// aot_runtime.h (aot_psq_st_fast) bypasses it, like the other _fast paths.
// Likewise s_stat_gp_store counts only integer/float pipe stores that reach
// TryGatherPipeWrite: generated code handles the SDK 0xCC008000 mapping in
// aot_runtime.h's aot_write_*_gp before calling the _slow helpers.
static u64 s_stat_rfi = 0, s_stat_idle = 0, s_stat_mtmsr = 0, s_stat_gp_store = 0,
           s_stat_gp_psq = 0;

// Check if address is in guest RAM: MEM1 (cached or uncached mirror), or on
// Wii, MEM2 at 0x90000000/0xD0000000. Low-memory (0x0xxxxxxx) and 0x4xxxxxxx
// accesses must take the slow path: with MSR.DR=1 the interpreter raises a
// DSI for them (BAT miss), so the fast path must not silently satisfy them
// from RAM. Must mirror aot_host_ptr in aot_runtime.h exactly.
static inline bool IsRAMAddress(u32 addr)
{
  const u32 off1 = (addr & ~AOT_MEM_UNCACHED_BIT) - AOT_MEM_CACHED_BASE;
  if (off1 < s_ram_size)
    return true;
  return (off1 - AOT_MEM2_FOLDED_OFFSET) < s_exram_size;
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
  const u32 off1 = (addr & ~AOT_MEM_UNCACHED_BIT) - AOT_MEM_CACHED_BASE;
  if (off1 < s_ram_size)
    return s_ram_ptr + off1;
  const u32 off2 = off1 - AOT_MEM2_FOLDED_OFFSET;
  if (off2 < s_exram_size)
    return s_exram_ptr + off2;
  // The +8 guard keeps the largest fast-path access (paired u32) inside the
  // small L1 buffer; the final few bytes fall back to the checked slow path.
  if (s_l1_ptr != nullptr && (addr >> 28) == 0xE && addr + 8 <= 0xE0000000u + s_l1_size)
    return s_l1_ptr + (addr & 0x0FFFFFFFu);
  return nullptr;
}

// Fallback-report opcode lookup: real-mode pcs (the exception vectors) are
// physical MEM1 addresses, which FastMemHostPtr (effective addresses) rejects.
static inline const u8* StatsCodePtr(u32 pc)
{
  if (const u8* p = FastMemHostPtr(pc))
    return p;
  return s_ram_ptr && pc + 4 <= s_ram_size ? s_ram_ptr + pc : nullptr;
}

extern "C"
{

// ============================================================================
// Init — cache RAM pointer and size to avoid repeated global lookups
// ============================================================================

// RAM fast-path descriptor exported to generated code: aot_runtime.h inlines
// the RAM fast path into every block and calls the aot_*_slow functions below
// for everything else. AotFastMem itself is defined in aot_runtime.h.
AotFastMem aot_fast_mem = {nullptr, 0, nullptr, 0, nullptr, nullptr, 0, 0};

void aot_init_fast_mem()
{
  s_guard_memos.fill({});
  s_trusted_code_memos.fill({});
  s_guard_checks = s_guard_reuses = 0;
  const char* trust = std::getenv("AOT_TRUST_CODE");
  const bool env_trust = trust && std::strcmp(trust, "1") == 0;
  s_trust_code = s_trust_code_request == -1 ? env_trust : s_trust_code_request == 1;
  if (s_trust_code)
  {
    std::fprintf(stderr, "AOT: experimental trust-code mode ON; validated code is reused until "
                         "explicit invalidation (cache fill/eviction and PLRU checks omitted).\n");
    NOTICE_LOG_FMT(CORE, "AOT: experimental trust-code mode ON (env={} request={}); validated "
                         "code is reused until explicit invalidation (cache fill/eviction and "
                         "PLRU checks omitted).",
                   env_trust, s_trust_code_request);
  }
  else
  {
    NOTICE_LOG_FMT(CORE, "AOT: strict guard mode (trust-code OFF; env={} request={})", env_trust,
                   s_trust_code_request);
  }
  s_system = &Core::System::GetInstance();
  s_interpreter = &s_system->GetInterpreter();
  s_mmu = &s_system->GetMMU();
  s_ram_ptr = s_system->GetMemory().GetRAM();
  s_ram_size = s_system->GetMemory().GetRamSizeReal();
  // CAUTION: GetExRamSizeReal() reports the retail MEM2 size even on GC —
  // only the allocation is Wii-gated. Key everything off the pointer.
  s_exram_ptr = s_system->GetMemory().GetEXRAM();
  s_exram_size = s_exram_ptr ? s_system->GetMemory().GetExRamSizeReal() : 0;
  s_l1_ptr = s_system->GetMemory().GetL1Cache();
  s_l1_size = s_system->GetMemory().GetL1CacheSize();
  aot_fast_mem.ram = s_ram_ptr;
  aot_fast_mem.size = s_ram_size;
  aot_fast_mem.exram = s_exram_ptr;
  aot_fast_mem.exram_size = s_exram_size;
  aot_fast_mem.dbat = s_mmu->GetDBATTable().data();
  aot_fast_mem.l1 = s_l1_ptr;
  aot_fast_mem.l1_size = s_l1_size;
  // aot_fm_resolve's L1 arm: (addr ^ 0xE0000000) < l1_lim == FastMemHostPtr's
  // L1 test (+8 guard); 0 disables it, like a null s_l1_ptr.
  aot_fast_mem.l1_lim = (s_l1_ptr != nullptr && s_l1_size >= 8) ? s_l1_size - 7 : 0;
}

// Counterpart to aot_init_fast_mem, called from AOTCore::Shutdown. The RAM/L1
// buffers are reallocated on the next boot, so the cached pointers must not
// survive into a second game's session.
void aot_shutdown()
{
  s_guard_memos.fill({});
  s_trusted_code_memos.fill({});
  s_guard_checks = s_guard_reuses = 0;
  s_trust_code = false;
  s_system = nullptr;
  s_interpreter = nullptr;
  s_mmu = nullptr;
  s_ram_ptr = nullptr;
  s_ram_size = 0;
  s_exram_ptr = nullptr;
  s_exram_size = 0;
  s_l1_ptr = nullptr;
  s_l1_size = 0;
  aot_fast_mem.ram = nullptr;
  aot_fast_mem.size = 0;
  aot_fast_mem.exram = nullptr;
  aot_fast_mem.exram_size = 0;
  aot_fast_mem.dbat = nullptr;
  aot_fast_mem.l1 = nullptr;
  aot_fast_mem.l1_size = 0;
  aot_fast_mem.l1_lim = 0;
  s_track_fallbacks = false;
  aot_stats_enabled = s_track_fallbacks ? 1 : 0;
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
AOT_SLOWPATH_CC uint32_t aot_read_u8_slow(AOTState* s, uint32_t addr)
{
  if (const u8* p = FastMemHostPtr(addr))
    return *p;
  return PowerPC::ReadFromJit<u8>(GetMMU(), addr);
}

AOT_SLOWPATH_CC uint32_t aot_read_u16_slow(AOTState* s, uint32_t addr)
{
  if (const u8* p = FastMemHostPtr(addr))
  {
    u16 v;
    std::memcpy(&v, p, sizeof(v));
    return Common::swap16(v);
  }
  return PowerPC::ReadFromJit<u16>(GetMMU(), addr);
}

AOT_SLOWPATH_CC uint32_t aot_read_u32_slow(AOTState* s, uint32_t addr)
{
  if (const u8* p = FastMemHostPtr(addr))
  {
    u32 v;
    std::memcpy(&v, p, sizeof(v));
    return Common::swap32(v);
  }
  return PowerPC::ReadFromJit<u32>(GetMMU(), addr);
}

AOT_SLOWPATH_CC uint64_t aot_read_u64_slow(AOTState* s, uint32_t addr)
{
  if (const u8* p = FastMemHostPtr(addr))
  {
    u64 v;
    std::memcpy(&v, p, sizeof(v));
    return Common::swap64(v);
  }
  return PowerPC::ReadFromJit<u64>(GetMMU(), addr);
}

// Write-gather pipe fast path. MMU::WriteToHardware reaches the same
// GPFifo::Write* calls only after its generic translation preamble; GX command
// submission (GXPosition/GXColor/...) is a store per vertex attribute, so that
// preamble dominated large scenes. Mirrors WriteToHardware exactly: BAT
// translation when MSR.DR is set (anything not BAT-mapped, and page-crossing
// stores, stay on the MMU path), then the same 0xFFFFF000 physical-address
// mask. Returns false when the store is not a gather-pipe write.
// Physical gather-pipe address for a `size`-byte store at effective `addr`, or
// 0 when the store is anything else (not BAT-mapped, not the pipe page, or
// page-crossing -- all left to the MMU path).
static inline u32 GatherPipePhysical(const PowerPC::PowerPCState& ppc_state, uint32_t addr,
                                     uint32_t size)
{
  u32 physical = addr;
  if (ppc_state.msr.DR)
  {
    const u32 bat = GetMMU().GetDBATTable()[addr >> PowerPC::BAT_INDEX_SHIFT];
    if (!(bat & PowerPC::BAT_MAPPED_BIT))
      return 0;
    physical = (bat & PowerPC::BAT_RESULT_MASK) | (addr & (PowerPC::BAT_PAGE_SIZE - 1));
  }
  if ((physical & 0xFFFFF000) != GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS)
    return 0;
  if ((addr & 0xFFF) + size > 0x1000)
    return 0;
  return physical;
}

// One element into the pipe, exactly as MMU::WriteToHardware's gather-pipe
// branch would do it (including its double harness capture record).
static inline void GatherPipeElement(u32 physical, u32 val, u32 size)
{
#ifdef DOLPHIN_AOT_HARNESS
  MMIOCaptureRecord(physical, val, size);
  MMIOCaptureRecord(physical, val, size);
#else
  (void)physical;
#endif
  auto& gpfifo = GetSystem().GetGPFifo();
  switch (size)
  {
  case 1:
    gpfifo.Write8(static_cast<u8>(val));
    break;
  case 2:
    gpfifo.Write16(static_cast<u16>(val));
    break;
  default:
    gpfifo.Write32(val);
    break;
  }
}

static inline bool TryGatherPipeWrite(AOTState* s, uint32_t addr, uint32_t val, uint32_t size)
{
  const u32 physical = GatherPipePhysical(GetPPCState(s), addr, size);
  if (!physical)
    return false;
  if (aot_stats_enabled)
    ++s_stat_gp_store;
  GatherPipeElement(physical, val, size);
  return true;
}

// Fill-level check for the inline gather-pipe path in aot_runtime.h (called only
// when the pipe holds >= GATHER_PIPE_SIZE bytes). CheckGatherPipe, not
// FastCheckGatherPipe: identical to what GPFifo::Write32 does after each element.
AOT_SLOWPATH_CC void aot_gp_flush(AOTState* s)
{
  (void)s;
  GetSystem().GetGPFifo().CheckGatherPipe();
}

// Harness mirror of GatherPipeElement's capture records (one call = one record;
// the header calls it twice per element, as MMU::WriteToHardware records twice).
// Defined unconditionally: the macOS lib is compiled with -DAOT_HARNESS=1 and
// must link against a core built with or without DOLPHIN_AOT_HARNESS.
void aot_gp_capture(uint32_t physical, uint32_t val, uint32_t size)
{
#ifdef DOLPHIN_AOT_HARNESS
  MMIOCaptureRecord(physical, val, size);
#else
  (void)physical;
  (void)val;
  (void)size;
#endif
}

AOT_SLOWPATH_CC void aot_write_u8_slow(AOTState* s, uint32_t val, uint32_t addr)
{
  if (TryGatherPipeWrite(s, addr, val, 1))
    return;
  if (u8* p = FastMemHostPtr(addr))
  {
    *p = static_cast<u8>(val);
    return;
  }
  PowerPC::WriteFromJit<u8>(GetMMU(), static_cast<u8>(val), addr);
}

AOT_SLOWPATH_CC void aot_write_u16_slow(AOTState* s, uint32_t val, uint32_t addr)
{  if (TryGatherPipeWrite(s, addr, val, 2))
    return;
  if (u8* p = FastMemHostPtr(addr))
  {
    const u16 v = Common::swap16(static_cast<u16>(val));
    std::memcpy(p, &v, sizeof(v));
    return;
  }
  PowerPC::WriteFromJit<u16>(GetMMU(), static_cast<u16>(val), addr);
}

AOT_SLOWPATH_CC void aot_write_u16_br_slow(AOTState* s, uint32_t val, uint32_t addr)
{
  if (u8* p = FastMemHostPtr(addr))
  {
    const u16 v = static_cast<u16>(val);  // no swap — byte-reversed store
    std::memcpy(p, &v, sizeof(v));
    return;
  }
  GetMMU().Write_U16_Swap(val, addr);
}

AOT_SLOWPATH_CC void aot_write_u32_slow(AOTState* s, uint32_t val, uint32_t addr)
{  if (TryGatherPipeWrite(s, addr, val, 4))
    return;
  if (u8* p = FastMemHostPtr(addr))
  {
    const u32 v = Common::swap32(val);
    std::memcpy(p, &v, sizeof(v));
    return;
  }
  PowerPC::WriteFromJit<u32>(GetMMU(), val, addr);
}

AOT_SLOWPATH_CC void aot_write_u64_slow(AOTState* s, uint64_t val, uint32_t addr)
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
  aot_stats_enabled = 1;
  s_fallback_counts.clear();
}

// Frontend switch for trust-code mode; takes effect at the next aot_init_fast_mem
// (i.e. call before boot). 1 = on, 0 = off (overrides AOT_TRUST_CODE=1).
void aot_set_trust_code(int enabled)
{
  s_trust_code_request = enabled ? 1 : 0;
}

// Periodic, NON-destructive variant of aot_dump_fallback_stats for frontends
// without a visible stderr (iOS): logs through NOTICE_LOG(CORE), keeps the map
// and tracking enabled, and reports deltas since the previous call. Must run
// on the CPU thread (owner of s_fallback_counts).
void aot_log_fallback_stats(int top_n)
{
  if (!s_track_fallbacks)
    return;

  static u64 last_total = 0, last_guard = 0, last_reuse = 0, last_excchk = 0, last_rfi = 0,
             last_mtmsr = 0, last_idle = 0, last_gp = 0;

  std::vector<std::pair<u32, u64>> sorted(s_fallback_counts.begin(), s_fallback_counts.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  u64 total = 0;
  for (const auto& [pc, count] : sorted)
    total += count;

  // Counters can be reset under us (new session); never report a negative delta.
  const auto delta = [](u64 now, u64& last) {
    const u64 d = now >= last ? now - last : now;
    last = now;
    return d;
  };
  const u64 gp = s_stat_gp_store + s_stat_gp_psq;
  const u64 dfb = delta(total, last_total);
  const u64 dguard = delta(s_guard_checks, last_guard);
  const u64 dreuse = delta(s_guard_reuses, last_reuse);
  const u64 dexc = delta(aot_stat_exception_checks, last_excchk);
  const u64 drfi = delta(s_stat_rfi, last_rfi);
  const u64 dmtmsr = delta(s_stat_mtmsr, last_mtmsr);
  const u64 didle = delta(s_stat_idle, last_idle);
  const u64 dgp = delta(gp, last_gp);
  NOTICE_LOG_FMT(CORE,
                 "AOTSTAT total_fb={} dfb={} uniq={} guard={} dguard={} reuse={} dreuse={} "
                 "trust={} excchk={} dexcchk={} rfi={} drfi={} mtmsr={} dmtmsr={} idle={} "
                 "didle={} gp={} dgp={}",
                 total, dfb, sorted.size(), s_guard_checks, dguard, s_guard_reuses, dreuse,
                 s_trust_code ? 1 : 0, aot_stat_exception_checks, dexc, s_stat_rfi, drfi,
                 s_stat_mtmsr, dmtmsr, s_stat_idle, didle, gp, dgp);

  const size_t limit = std::min<size_t>(sorted.size(), top_n > 0 ? size_t(top_n) : 0);
  for (size_t i = 0; i < limit; i++)
  {
    const u32 pc = sorted[i].first;
    std::string opname = "???";
    if (const u8* code = StatsCodePtr(pc))
    {
      u32 inst_word;
      std::memcpy(&inst_word, code, sizeof(u32));
      inst_word = Common::swap32(inst_word);
      UGeckoInstruction inst(inst_word);
      const GekkoOPInfo* info = PPCTables::GetOpInfo(inst, pc);
      if (info)
        opname = info->opname;
    }
    NOTICE_LOG_FMT(CORE, "AOTSTAT fb {:>10} pc={:#010x} {}", sorted[i].second, pc, opname);
  }
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
  fmt::print(stderr, "Guard checks: {} | Reused: {}\n", s_guard_checks, s_guard_reuses);
  fmt::print(stderr,
             "Events: run-loop exception checks {} (EE clear {}), rfi {}, mtmsr {}, idle exits "
             "{}, gather-pipe stores {} (+{} psq)\n",
             aot_stat_exception_checks, aot_stat_exception_masked, s_stat_rfi, s_stat_mtmsr,
             s_stat_idle, s_stat_gp_store, s_stat_gp_psq);
  static const char* const kBitNames[16] = {"DEC", "SYSCALL", "EXTINT", "DSI", "ISI", "ALIGN",
                                            "FPU", "PROGRAM", "PMC", "bit9", "bit10", "bit11",
                                            "bit12", "bit13", "bit14", "bit15"};
  fmt::print(stderr, "Pending exception bits at those checks:");
  for (int b = 0; b < 16; b++)
    if (aot_stat_exception_bits[b])
      fmt::print(stderr, " {}={}", kBitNames[b], aot_stat_exception_bits[b]);
  fmt::print(stderr, "\n");
  fmt::print(stderr, "Unique PCs: {}\n\n", sorted.size());

  const size_t limit = std::min<size_t>(sorted.size(), 50);
  for (size_t i = 0; i < limit; i++)
  {
    const u32 pc = sorted[i].first;
    const u64 count = sorted[i].second;
    std::string opname = "???";
    if (const u8* code = StatsCodePtr(pc))
    {
      u32 inst_word;
      std::memcpy(&inst_word, code, sizeof(u32));
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

// PowerPCManager::MSRUpdatedInternal, inlined: the feature_flags update is two
// ALU ops, and the rare msr.DR && pagetable_update_pending case calls the real
// function (which recomputes the same flags, then MMU::PageTableUpdated). Same
// computation as aot_mtmsr_fast in aot_runtime.h.
static inline void MsrUpdatedFast(PowerPC::PowerPCState& ppc_state)
{
  if (ppc_state.msr.DR && ppc_state.pagetable_update_pending) [[unlikely]]
  {
    GetSystem().GetPowerPC().MSRUpdatedInternal();
    return;
  }
  ppc_state.feature_flags = static_cast<CPUEmuFeatureFlags>(
      (ppc_state.feature_flags & FEATURE_FLAG_PERFMON) | ((ppc_state.msr.Hex >> 4) & 0x3));
}

void aot_msr_updated(AOTState* s)
{
  // MSRUpdated minus JitInterface::UpdateMembase() — the AOT runtime accesses
  // RAM through aot_fast_mem, not the JIT's fastmem base.
  MsrUpdatedFast(GetPPCState(s));
}

void aot_rfi(AOTState* s)
{
  if (aot_stats_enabled)
    ++s_stat_rfi;
  auto& ppc_state = GetPPCState(s);
  const u32 mask = 0x87C0FFFF;
  const u32 clearMSR13 = 0xFFFBFFFF;
  ppc_state.msr.Hex = ((ppc_state.msr.Hex & ~mask) | (SRR1(ppc_state) & mask)) & clearMSR13;
  ppc_state.pc = SRR0(ppc_state);
  ppc_state.npc = ppc_state.pc;
  MsrUpdatedFast(ppc_state);
  // Generated code dispatches straight to pc (no Run-loop bounce), so deliver
  // anything the restored MSR.EE now permits here, as the Run loop would.
  if (ppc_state.Exceptions != 0)
    GetSystem().GetPowerPC().CheckExceptions();
}

// mtmsr without ending the block: returns 1 when CheckExceptions delivered an
// exception (pc moved to a vector), 0 when the block may continue with the
// next instruction. The caller sets pc/npc to the fallthrough first.
int aot_mtmsr_check(AOTState* s, uint32_t val)
{
  if (aot_stats_enabled)
    ++s_stat_mtmsr;
  auto& ppc_state = GetPPCState(s);
  ppc_state.msr.Hex = val;
  MsrUpdatedFast(ppc_state);  // Lightweight -- no BAT remapping
  if (ppc_state.Exceptions == 0)
    return 0;
  const u32 pc = ppc_state.pc;
  GetSystem().GetPowerPC().CheckExceptions();
  return ppc_state.pc != pc;
}

void aot_mtmsr(AOTState* s, uint32_t val)
{
  if (aot_stats_enabled)
    ++s_stat_mtmsr;
  auto& ppc_state = GetPPCState(s);
  ppc_state.msr.Hex = val;
  MsrUpdatedFast(ppc_state);  // Lightweight — no BAT remapping
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
    ++ppc_state.iCache.invalidation_generation;
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

void aot_cr_logical(AOTState* s, int crbD, int crbA, int crbB, AotCrOp op)
{
  auto& ppc_state = GetPPCState(s);
  u32 a = ppc_state.cr.GetBit(crbA);
  u32 b = ppc_state.cr.GetBit(crbB);
  u32 result;

  switch (op)
  {
  case AOT_CR_AND:
    result = a & b;
    break;
  case AOT_CR_OR:
    result = a | b;
    break;
  case AOT_CR_XOR:
    result = a ^ b;
    break;
  case AOT_CR_EQV:
    result = ~(a ^ b) & 1;
    break;
  case AOT_CR_ANDC:
    result = a & (~b & 1);
    break;
  case AOT_CR_ORC:
    result = a | (~b & 1);
    break;
  case AOT_CR_NAND:
    result = ~(a & b) & 1;
    break;
  case AOT_CR_NOR:
    result = ~(a | b) & 1;
    break;
  default:
    result = 0;
    break;
  }

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

// PS arithmetic — thin wrappers over the op cores in Interpreter_PairedUtils.h
// (the same functions the interpreter's ps_* handlers execute). The Rc=1
// (UpdateCR1) branch is omitted because the emitter always passes Rc=0.
// div/res/rsqrte stay delegated to the interpreter methods — they're rare and
// their exception logic is more involved.
void aot_ps_add(AOTState* s, int fd, int fa, int fb)
{
  PS_Add(GetPPCState(s), fd, fa, fb);
}

void aot_ps_sub(AOTState* s, int fd, int fa, int fb)
{
  PS_Sub(GetPPCState(s), fd, fa, fb);
}

void aot_ps_mul(AOTState* s, int fd, int fa, int fc)
{
  PS_Mul(GetPPCState(s), fd, fa, fc);
}

FP_IMPL_3(aot_ps_div, ps_div)

void aot_ps_madd(AOTState* s, int fd, int fa, int fc, int fb)
{
  PS_Madd(GetPPCState(s), fd, fa, fc, fb);
}

void aot_ps_msub(AOTState* s, int fd, int fa, int fc, int fb)
{
  PS_Msub(GetPPCState(s), fd, fa, fc, fb);
}

void aot_ps_nmadd(AOTState* s, int fd, int fa, int fc, int fb)
{
  PS_Nmadd(GetPPCState(s), fd, fa, fc, fb);
}

void aot_ps_nmsub(AOTState* s, int fd, int fa, int fc, int fb)
{
  PS_Nmsub(GetPPCState(s), fd, fa, fc, fb);
}

void aot_ps_sum0(AOTState* s, int fd, int fa, int fc, int fb)
{
  PS_Sum0(GetPPCState(s), fd, fa, fc, fb);
}

void aot_ps_sum1(AOTState* s, int fd, int fa, int fc, int fb)
{
  PS_Sum1(GetPPCState(s), fd, fa, fc, fb);
}

void aot_ps_muls0(AOTState* s, int fd, int fa, int fc)
{
  PS_Muls0(GetPPCState(s), fd, fa, fc);
}

void aot_ps_muls1(AOTState* s, int fd, int fa, int fc)
{
  PS_Muls1(GetPPCState(s), fd, fa, fc);
}

void aot_ps_madds0(AOTState* s, int fd, int fa, int fc, int fb)
{
  PS_Madds0(GetPPCState(s), fd, fa, fc, fb);
}

void aot_ps_madds1(AOTState* s, int fd, int fa, int fc, int fb)
{
  PS_Madds1(GetPPCState(s), fd, fa, fc, fb);
}

void aot_ps_sel(AOTState* s, int fd, int fa, int fc, int fb)
{
  PS_Sel(GetPPCState(s), fd, fa, fc, fb);
}

FP_IMPL_2(aot_ps_res, ps_res)
FP_IMPL_2(aot_ps_rsqrte, ps_rsqrte)

// PS moves — must operate on BOTH ps0 AND ps1 (unlike scalar fneg/fabs/fnabs)
void aot_ps_neg(AOTState* s, int fd, int fb)
{
  PS_Neg(GetPPCState(s), fd, fb);
}

void aot_ps_mr(AOTState* s, int fd, int fb)
{
  PS_Mr(GetPPCState(s), fd, fb);
}

void aot_ps_abs(AOTState* s, int fd, int fb)
{
  PS_Abs(GetPPCState(s), fd, fb);
}

void aot_ps_nabs(AOTState* s, int fd, int fb)
{
  PS_Nabs(GetPPCState(s), fd, fb);
}

// PS merge
void aot_ps_merge00(AOTState* s, int fd, int fa, int fb)
{
  PS_Merge00(GetPPCState(s), fd, fa, fb);
}
void aot_ps_merge01(AOTState* s, int fd, int fa, int fb)
{
  PS_Merge01(GetPPCState(s), fd, fa, fb);
}
void aot_ps_merge10(AOTState* s, int fd, int fa, int fb)
{
  PS_Merge10(GetPPCState(s), fd, fa, fb);
}
void aot_ps_merge11(AOTState* s, int fd, int fa, int fb)
{
  PS_Merge11(GetPPCState(s), fd, fa, fb);
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

template <typename T>
std::pair<double, double> FastLoadAndDequantize(const u8* p, u32 instW, u32 ld_scale)
{
  using U = std::make_unsigned_t<T>;

  float ps0, ps1;
  if (instW != 0)
  {
    const U value = FastRead<U>(p);
    ps0 = float(T(value)) * PowerPC::DEQUANTIZE_TABLE[ld_scale];
    ps1 = 1.0f;
  }
  else
  {
    const auto [first, second] = FastReadPair<U>(p);
    ps0 = float(T(first)) * PowerPC::DEQUANTIZE_TABLE[ld_scale];
    ps1 = float(T(second)) * PowerPC::DEQUANTIZE_TABLE[ld_scale];
  }
  // ps0 and ps1 always contain finite and normal numbers, so casting to double is safe
  return {static_cast<double>(ps0), static_cast<double>(ps1)};
}

template <typename T>
void FastQuantizeAndStore(double ps0, double ps1, u8* p, u32 instW, u32 st_scale)
{
  using U = std::make_unsigned_t<T>;

  const U conv_ps0 = U(PowerPC::ScaleAndClamp<T>(ps0, st_scale));
  if (instW != 0)
  {
    FastWrite<U>(conv_ps0, p);
  }
  else
  {
    const U conv_ps1 = U(PowerPC::ScaleAndClamp<T>(ps1, st_scale));
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

// psq_st into the write-gather pipe (GX packed vertex attributes). Quantizes
// into a scratch buffer with the RAM fast path's routines, then pushes each
// element in the interpreter's order (ps0 then ps1) as separate pipe writes.
// The float case normally no longer arrives here from non-update psq_st: the
// inline path in aot_runtime.h (aot_psq_st_fast -> aot_gp_store_*) handles it.
// It still serves quantized types, psq_stu/stx/stux, DR-off / remapped-BAT
// cases and page-crossing stores, so keep it complete.
static bool GatherPipeQuantize(PowerPC::PowerPCState& ppcs, u32 addr, u32 instI, u32 instRS,
                               u32 instW)
{
  const UGQR gqr(ppcs.spr[SPR_GQR0 + instI]);
  u32 elem = 0;
  switch (gqr.st_type)
  {
  case QUANTIZE_FLOAT:
    elem = 4;
    break;
  case QUANTIZE_U16:
  case QUANTIZE_S16:
    elem = 2;
    break;
  case QUANTIZE_U8:
  case QUANTIZE_S8:
    elem = 1;
    break;
  default:
    return false;
  }
  const u32 count = instW != 0 ? 1 : 2;
  const u32 physical = GatherPipePhysical(ppcs, addr, elem * count);
  if (!physical)
    return false;
  alignas(8) u8 buf[8] = {};
  const double ps0 = ppcs.ps[instRS].PS0AsDouble();
  const double ps1 = ppcs.ps[instRS].PS1AsDouble();
  switch (gqr.st_type)
  {
  case QUANTIZE_FLOAT:
  {
    const u32 conv_ps0 = ConvertToSingleFTZ(std::bit_cast<u64>(ps0));
    if (instW != 0)
      FastWrite<u32>(conv_ps0, buf);
    else
      FastWritePair<u32>(conv_ps0, ConvertToSingleFTZ(std::bit_cast<u64>(ps1)), buf);
    break;
  }
  case QUANTIZE_U8:
    FastQuantizeAndStore<u8>(ps0, ps1, buf, instW, gqr.st_scale);
    break;
  case QUANTIZE_U16:
    FastQuantizeAndStore<u16>(ps0, ps1, buf, instW, gqr.st_scale);
    break;
  case QUANTIZE_S8:
    FastQuantizeAndStore<s8>(ps0, ps1, buf, instW, gqr.st_scale);
    break;
  default:
    FastQuantizeAndStore<s16>(ps0, ps1, buf, instW, gqr.st_scale);
    break;
  }
  if (aot_stats_enabled)
    ++s_stat_gp_psq;
  for (u32 i = 0; i < count; i++)
  {
    u32 val = 0;
    if (elem == 4)
      val = FastRead<u32>(buf + 4 * i);
    else if (elem == 2)
      val = FastRead<u16>(buf + 2 * i);
    else
      val = buf[i];
    GatherPipeElement(physical + elem * i, val, elem);
  }
  return true;
}

bool FastQuantize(PowerPC::PowerPCState& ppcs, u32 addr, u32 instI, u32 instRS, u32 instW)
{
  u8* p = FastMemHostPtr(addr);
  if (p == nullptr)
    return GatherPipeQuantize(ppcs, addr, instI, instRS, instW);

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

// Busy-wait loop back-edge (emitter IsBusyWaitLoop): same as the JIT's idle
// exit — advance to the next scheduled event instead of spinning. Zeroes the
// downcount; the generated code returns to the Run loop right after.
void aot_idle(AOTState* s)
{
  (void)s;
  if (aot_stats_enabled)
    ++s_stat_idle;
  GetSystem().GetCoreTiming().Idle();
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

// Fast path for content guards: compare words already resident in the emulated
// instruction cache, translating once per BAT range instead of once per word.
// Successful comparisons may be reused while cache contents and the physical
// mapping stay unchanged. Cache hits still update the replacement policy.
static std::optional<bool> MatchResidentCode(uint32_t pc, const uint32_t* words, uint32_t count)
{
  auto& system = GetSystem();
  auto& state = system.GetPPCState();
  auto& cache = state.iCache;
  if (!HID0(state).ICE || cache.m_disable_icache ||
      (pc >> PowerPC::BAT_INDEX_SHIFT) != ((pc + (count - 1) * 4) >> PowerPC::BAT_INDEX_SHIFT))
    return std::nullopt;

  u32 physical = pc;
  if (state.msr.IR)
  {
    const u32 bat = GetMMU().GetIBATTable()[pc >> PowerPC::BAT_INDEX_SHIFT];
    if (!(bat & PowerPC::BAT_MAPPED_BIT))
      return std::nullopt;
    physical = (bat & PowerPC::BAT_RESULT_MASK) | (pc & (PowerPC::BAT_PAGE_SIZE - 1));
  }
  // Limit the fast path to real MEM1/MEM2. Page-table mappings, fake VMEM,
  // uncached instruction fetches and other memory types use the MMU below.
  const u32 bytes = count * 4;
  const u32 mem2_offset = physical - 0x10000000u;
  if (!((physical < s_ram_size && bytes <= s_ram_size - physical) ||
        (mem2_offset < s_exram_size && bytes <= s_exram_size - mem2_offset)))
    return std::nullopt;

  const uintptr_t key = reinterpret_cast<uintptr_t>(words);
  const size_t slot = ((key >> 2) ^ (key >> 16)) & (s_guard_memos.size() - 1);
  auto& memo = s_guard_memos[slot];
  GuardMemo pending;
  pending.words = words;
  pending.generation = cache.content_generation;
  pending.pc = pc;
  pending.translation = state.msr.IR ? GetMMU().GetIBATTable()[pc >> PowerPC::BAT_INDEX_SHIFT] : 0;
  pending.count = count;
  u32 checked = 0;
  while (checked < count)
  {
    const u32 address = physical + checked * 4;
    // locked=true is a non-filling lookup, regardless of the guest's ILOCK.
    // GetCache also preserves the normal replacement-policy update on hits.
    const auto [set, way] = cache.GetCache(system.GetMemory(), address, true);
    if (way == 0xff)
      return std::nullopt;
    const u32 offset = (address & 31) / 4;
    const u32 length = std::min(count - checked, PowerPC::CACHE_BLOCK_SIZE - offset);
    for (u32 n = 0; n < length; ++n)
      if (Common::swap32(cache.data[set][way][offset + n]) != words[checked + n])
        return false;
    if (pending.line_count < pending.lines.size())
      pending.lines[pending.line_count] = {static_cast<u8>(set), static_cast<u8>(way)};
    ++pending.line_count;
    checked += length;
  }
  if (pending.line_count <= pending.lines.size())
    memo = pending;
  return true;
}

// Content-guarded alternate DOLs. Writes before icbi must continue to select
// the cached instructions, not freshly modified RAM.
// Keep cache fills, word comparisons and their stack temporaries out of the
// per-block memo hit path, including when the runtime is linked with ThinLTO.
[[gnu::noinline]] static int MatchCodeSlow(uint32_t pc, const uint32_t* words, uint32_t count,
                                           bool resident_only)
{
  if (!words || !count || (pc & 3) || count > (UINT32_MAX - pc) / 4)
    return 0;
  if (const auto match = MatchResidentCode(pc, words, count))
    return *match;
  auto& mmu = GetMMU();
  for (uint32_t i = 0; i < count; ++i)
  {
    const auto inst = mmu.TryReadInstruction(pc + i * 4);
    if (!inst.valid || inst.hex != words[i])
      return 0;
  }
  // A normal fetch may have filled the line. Locked misses and uncached or
  // page-table execution still cannot certify a fused chain.
  return resident_only ? MatchResidentCode(pc, words, count).value_or(false) : 1;
}

template <bool ResidentOnly>
static int MatchCode(uint32_t pc, const uint32_t* words, uint32_t count)
{
  // Pre-init calls use the slow path; normal hits need no lazy singleton setup.
  if (!s_system)
    return MatchCodeSlow(pc, words, count, ResidentOnly);
  auto& state = s_system->GetPPCState();
  auto& cache = state.iCache;
  const uintptr_t key = reinterpret_cast<uintptr_t>(words);
  const size_t slot = ((key >> 2) ^ (key >> 16)) & (s_guard_memos.size() - 1);
  const auto& memo = s_guard_memos[slot];
  // A populated memo also certifies alignment, bounds and real MEM1/MEM2.
  // Recheck the effective address and BAT descriptor before reusing it: the
  // same instruction array can be queried at different virtual addresses.
  const bool reused =
      memo.line_count && memo.words == words && memo.pc == pc && memo.count == count &&
      memo.generation == cache.content_generation && HID0(state).ICE && !cache.m_disable_icache &&
      (state.msr.IR ? memo.translation != 0 && s_mmu &&
                          memo.translation == s_mmu->GetIBATTable()[pc >> PowerPC::BAT_INDEX_SHIFT]
                    : memo.translation == 0);
  if (s_track_fallbacks)
  {
    ++s_guard_checks;
    s_guard_reuses += reused;
  }
  if (!reused)
    return MatchCodeSlow(pc, words, count, ResidentOnly);
  for (u32 n = 0; n < memo.line_count; ++n)
    cache.MarkUsed(memo.lines[n].set, memo.lines[n].way);
  return 1;
}

template <bool ResidentOnly>
[[gnu::noinline]] static int RememberTrustedCode(uint32_t pc, const uint32_t* words, uint32_t count)
{
  const int matches = MatchCode<ResidentOnly>(pc, words, count);
  // Only certify actual resident MEM1/MEM2 instructions, including for normal
  // entries. This prevents a locked miss or uncached fetch seeding a chain memo.
  if (matches && MatchResidentCode(pc, words, count).value_or(false))
  {
    auto& state = s_system->GetPPCState();
    const u32 translation =
        state.msr.IR ? s_mmu->GetIBATTable()[pc >> PowerPC::BAT_INDEX_SHIFT] : 0;
    const uintptr_t key = reinterpret_cast<uintptr_t>(words);
    auto& memo =
        s_trusted_code_memos[((key >> 2) ^ (key >> 16)) & (s_trusted_code_memos.size() - 1)];
    memo = {words, state.iCache.invalidation_generation, pc, translation, count};
  }
  return matches;
}

// Experimental fast mode: a successful resident comparison establishes the
// instruction identity until an explicit invalidation event. Ordinary fills do
// not expire it, and hits neither fetch instructions nor touch replacement state.
// Translation and cache-enable checks keep unsupported execution on strict paths.
template <bool ResidentOnly>
static int MatchTrustedCode(uint32_t pc, const uint32_t* words, uint32_t count)
{
  if (!s_system)
    return MatchCode<ResidentOnly>(pc, words, count);
  auto& state = s_system->GetPPCState();
  auto& cache = state.iCache;
  const u32 translation = state.msr.IR ? s_mmu->GetIBATTable()[pc >> PowerPC::BAT_INDEX_SHIFT] : 0;
  if (!HID0(state).ICE || cache.m_disable_icache ||
      (state.msr.IR && !(translation & PowerPC::BAT_MAPPED_BIT)))
    return MatchCode<ResidentOnly>(pc, words, count);
  const uintptr_t key = reinterpret_cast<uintptr_t>(words);
  auto& memo = s_trusted_code_memos[((key >> 2) ^ (key >> 16)) & (s_trusted_code_memos.size() - 1)];
  if (memo.words == words && words && memo.pc == pc && memo.count == count &&
      memo.generation == cache.invalidation_generation && memo.translation == translation)
    return 1;
  return RememberTrustedCode<ResidentOnly>(pc, words, count);
}

extern "C" int aot_match_code(uint32_t pc, const uint32_t* words, uint32_t count)
{
  return s_trust_code ? MatchTrustedCode<false>(pc, words, count)
                      : MatchCode<false>(pc, words, count);
}

// A fused chain may omit intermediate content checks only when all of its
// instructions are already in one cache line, and its bodies cannot change
// instruction translation or invalidate that line. The emitter proves the
// latter; this entry point refuses uncached/locked-miss/page-table execution.
extern "C" int aot_match_chain(uint32_t pc, const uint32_t* words, uint32_t count)
{
  if (!count || count > (32 - (pc & 31)) / 4)
    return 0;
  return s_trust_code ? MatchTrustedCode<true>(pc, words, count)
                      : MatchCode<true>(pc, words, count);
}
