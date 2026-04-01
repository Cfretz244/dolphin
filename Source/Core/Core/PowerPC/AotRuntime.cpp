// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// AOT Runtime: extern "C" helper functions called by AOT-translated PPC code.
// These wrap Dolphin's existing subsystems (MMU, Interpreter, CoreTiming).

#include <cstdint>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// The AOTState struct is layout-compatible with PowerPCState.
// At runtime, we cast between them.
struct AOTState;

static PowerPC::PowerPCState& GetPPCState(AOTState* s)
{
  return *reinterpret_cast<PowerPC::PowerPCState*>(s);
}

static Core::System& GetSystem()
{
  return Core::System::GetInstance();
}

static PowerPC::MMU& GetMMU()
{
  return GetSystem().GetMMU();
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
// ============================================================================

static inline u8* GetRAMPtr()
{
  return GetSystem().GetMemory().GetRAM();
}

static inline u32 GetRAMSize()
{
  return GetSystem().GetMemory().GetRamSizeReal();
}

// Check if address is in main RAM (cached or uncached mirror)
static inline bool IsRAMAddress(u32 addr)
{
  // 0x80000000-0x81800000 (cached) or 0xC0000000-0xC1800000 (uncached)
  u32 physical = addr & 0x3FFFFFFF;
  return physical < GetRAMSize();
}

extern "C"
{

// ============================================================================
// Memory access — fast path for RAM, slow path for MMIO/other
// ============================================================================

uint32_t aot_read_u8(AOTState* s, uint32_t addr)
{
  if (IsRAMAddress(addr))
    return GetRAMPtr()[addr & 0x3FFFFFFF];
  return PowerPC::ReadFromJit<u8>(GetMMU(), addr);
}

uint32_t aot_read_u16(AOTState* s, uint32_t addr)
{
  if (IsRAMAddress(addr))
  {
    u16 val;
    std::memcpy(&val, &GetRAMPtr()[addr & 0x3FFFFFFF], sizeof(u16));
    return Common::swap16(val);
  }
  return PowerPC::ReadFromJit<u16>(GetMMU(), addr);
}

uint32_t aot_read_u16_se(AOTState* s, uint32_t addr)
{
  return static_cast<uint32_t>(static_cast<int32_t>(
      static_cast<int16_t>(aot_read_u16(s, addr))));
}

uint32_t aot_read_u32(AOTState* s, uint32_t addr)
{
  if (IsRAMAddress(addr))
  {
    u32 val;
    std::memcpy(&val, &GetRAMPtr()[addr & 0x3FFFFFFF], sizeof(u32));
    return Common::swap32(val);
  }
  return PowerPC::ReadFromJit<u32>(GetMMU(), addr);
}

uint64_t aot_read_u64(AOTState* s, uint32_t addr)
{
  if (IsRAMAddress(addr))
  {
    u64 val;
    std::memcpy(&val, &GetRAMPtr()[addr & 0x3FFFFFFF], sizeof(u64));
    return Common::swap64(val);
  }
  return PowerPC::ReadFromJit<u64>(GetMMU(), addr);
}

void aot_write_u8(AOTState* s, uint32_t val, uint32_t addr)
{
  if (IsRAMAddress(addr))
  {
    GetRAMPtr()[addr & 0x3FFFFFFF] = static_cast<u8>(val);
    return;
  }
  PowerPC::WriteFromJit<u8>(GetMMU(), static_cast<u8>(val), addr);
}

void aot_write_u16(AOTState* s, uint32_t val, uint32_t addr)
{
  if (IsRAMAddress(addr))
  {
    u16 swapped = Common::swap16(static_cast<u16>(val));
    std::memcpy(&GetRAMPtr()[addr & 0x3FFFFFFF], &swapped, sizeof(u16));
    return;
  }
  PowerPC::WriteFromJit<u16>(GetMMU(), static_cast<u16>(val), addr);
}

void aot_write_u32(AOTState* s, uint32_t val, uint32_t addr)
{
  if (IsRAMAddress(addr))
  {
    u32 swapped = Common::swap32(val);
    std::memcpy(&GetRAMPtr()[addr & 0x3FFFFFFF], &swapped, sizeof(u32));
    return;
  }
  PowerPC::WriteFromJit<u32>(GetMMU(), val, addr);
}

void aot_write_u64(AOTState* s, uint64_t val, uint32_t addr)
{
  if (IsRAMAddress(addr))
  {
    u64 swapped = Common::swap64(val);
    std::memcpy(&GetRAMPtr()[addr & 0x3FFFFFFF], &swapped, sizeof(u64));
    return;
  }
  PowerPC::WriteFromJit<u64>(GetMMU(), val, addr);
}

// ============================================================================
// Interpreter fallback
// ============================================================================

void aot_interpreter_single_step(AOTState* s)
{
  auto& ppc_state = GetPPCState(s);
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

void aot_rfi(AOTState* s)
{
  auto& ppc_state = GetPPCState(s);
  const u32 mask = 0x87C0FFFF;
  const u32 clearMSR13 = 0xFFFBFFFF;
  ppc_state.msr.Hex = ((ppc_state.msr.Hex & ~mask) | (SRR1(ppc_state) & mask)) & clearMSR13;
  ppc_state.pc = SRR0(ppc_state);
  ppc_state.npc = ppc_state.pc;
}

void aot_msr_updated(AOTState* s)
{
  // Update feature flags from MSR bits — lightweight, no subsystem calls
  auto& ppc_state = GetPPCState(s);
  CPUEmuFeatureFlags flags{};
  if (ppc_state.msr.DR)
    flags = static_cast<CPUEmuFeatureFlags>(flags | FEATURE_FLAG_MSR_DR);
  if (ppc_state.msr.IR)
    flags = static_cast<CPUEmuFeatureFlags>(flags | FEATURE_FLAG_MSR_IR);
  ppc_state.feature_flags = flags;
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
    // Gather pipe status
    return 0x40000000;  // BNE set when gather pipe not empty
  default:
    return ppc_state.spr[spr_index];
  }
}

void aot_mtspr_special(AOTState* s, uint32_t spr_index, uint32_t val)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.spr[spr_index] = val;

  switch (spr_index)
  {
  case SPR_DEC:
  {
    GetSystem().GetSystemTimers().DecrementerSet();
    break;
  }
  case SPR_HID0:
  case SPR_HID2:
  case SPR_HID4:
    // These can trigger mode changes; for now just store
    break;
  default:
    break;
  }
}

uint32_t aot_mftb(AOTState* s, uint32_t spr_encoded)
{
  return aot_mfspr_special(s, SPR_TL);  // mftb uses TBL/TBU
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
  // Use Dolphin's ConvertToDouble from PairedSingle
  double result;
  // IEEE 754 single → double conversion via bit manipulation
  u32 sign = (single_bits >> 31) & 1;
  u32 exp = (single_bits >> 23) & 0xFF;
  u32 frac = single_bits & 0x7FFFFF;

  u64 double_bits;
  if (exp == 0 && frac == 0)
  {
    // Zero
    double_bits = static_cast<u64>(sign) << 63;
  }
  else if (exp == 0)
  {
    // Denormal — normalize it
    u32 e = 0;
    while (!(frac & 0x800000))
    {
      frac <<= 1;
      e++;
    }
    frac &= 0x7FFFFF;
    double_bits = (static_cast<u64>(sign) << 63) |
                  (static_cast<u64>(0x3FF - 0x7F - e + 1) << 52) |
                  (static_cast<u64>(frac) << 29);
  }
  else if (exp == 0xFF)
  {
    // Inf or NaN
    double_bits = (static_cast<u64>(sign) << 63) | (0x7FFULL << 52) |
                  (static_cast<u64>(frac) << 29);
  }
  else
  {
    // Normal
    double_bits = (static_cast<u64>(sign) << 63) |
                  (static_cast<u64>(exp - 0x7F + 0x3FF) << 52) |
                  (static_cast<u64>(frac) << 29);
  }

  return double_bits;
}

uint32_t aot_convert_to_single(uint64_t double_bits)
{
  // Double → single truncation (used by stfs)
  float f;
  double d;
  std::memcpy(&d, &double_bits, sizeof(d));
  f = static_cast<float>(d);
  uint32_t result;
  std::memcpy(&result, &f, sizeof(result));
  return result;
}

// ============================================================================
// FP arithmetic — all delegate to interpreter methods
// ============================================================================

// Helper: get the interpreter instance
static Interpreter& GetInterpreter()
{
  return GetSystem().GetInterpreter();
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
// Paired singles — delegate to interpreter
// ============================================================================

// PS arithmetic
FP_IMPL_3(aot_ps_add, ps_add)
FP_IMPL_3(aot_ps_sub, ps_sub)
FP_IMPL_3C(aot_ps_mul, ps_mul)
FP_IMPL_3(aot_ps_div, ps_div)
FP_IMPL_4(aot_ps_madd, ps_madd)
FP_IMPL_4(aot_ps_msub, ps_msub)
FP_IMPL_4(aot_ps_nmadd, ps_nmadd)
FP_IMPL_4(aot_ps_nmsub, ps_nmsub)
FP_IMPL_4(aot_ps_sum0, ps_sum0)
FP_IMPL_4(aot_ps_sum1, ps_sum1)
FP_IMPL_3C(aot_ps_muls0, ps_muls0)
FP_IMPL_3C(aot_ps_muls1, ps_muls1)
FP_IMPL_4(aot_ps_madds0, ps_madds0)
FP_IMPL_4(aot_ps_madds1, ps_madds1)
FP_IMPL_4(aot_ps_sel, ps_sel)
FP_IMPL_2(aot_ps_res, ps_res)
FP_IMPL_2(aot_ps_rsqrte, ps_rsqrte)

// PS moves
void aot_ps_neg(AOTState* s, int fd, int fb)
{
  auto& ppc_state = GetPPCState(s);
  u64 v0 = ppc_state.ps[fb].PS0AsU64();
  u64 v1 = ppc_state.ps[fb].PS1AsU64();
  ppc_state.ps[fd].SetPS0(v0 ^ (1ULL << 63));
  ppc_state.ps[fd].SetPS1(v1);
}

void aot_ps_mr(AOTState* s, int fd, int fb)
{
  auto& ppc_state = GetPPCState(s);
  ppc_state.ps[fd] = ppc_state.ps[fb];
}

void aot_ps_abs(AOTState* s, int fd, int fb)
{
  auto& ppc_state = GetPPCState(s);
  u64 v0 = ppc_state.ps[fb].PS0AsU64();
  u64 v1 = ppc_state.ps[fb].PS1AsU64();
  ppc_state.ps[fd].SetPS0(v0 & ~(1ULL << 63));
  ppc_state.ps[fd].SetPS1(v1);
}

void aot_ps_nabs(AOTState* s, int fd, int fb)
{
  auto& ppc_state = GetPPCState(s);
  u64 v0 = ppc_state.ps[fb].PS0AsU64();
  u64 v1 = ppc_state.ps[fb].PS1AsU64();
  ppc_state.ps[fd].SetPS0(v0 | (1ULL << 63));
  ppc_state.ps[fd].SetPS1(v1);
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

// PS quantized load/store — pass raw instruction to interpreter
void aot_psq_l(AOTState* s, int fd, int ra, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  Interpreter::psq_l(GetInterpreter(), inst);
}
void aot_psq_lu(AOTState* s, int fd, int ra, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  Interpreter::psq_lu(GetInterpreter(), inst);
}
void aot_psq_st(AOTState* s, int fs, int ra, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  Interpreter::psq_st(GetInterpreter(), inst);
}
void aot_psq_stu(AOTState* s, int fs, int ra, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  Interpreter::psq_stu(GetInterpreter(), inst);
}
void aot_psq_lx(AOTState* s, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  Interpreter::psq_lx(GetInterpreter(), inst);
}
void aot_psq_stx(AOTState* s, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  Interpreter::psq_stx(GetInterpreter(), inst);
}
void aot_psq_lux(AOTState* s, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  Interpreter::psq_lux(GetInterpreter(), inst);
}
void aot_psq_stux(AOTState* s, uint32_t inst_hex)
{
  UGeckoInstruction inst(inst_hex);
  Interpreter::psq_stux(GetInterpreter(), inst);
}

// ============================================================================
// Cache operations
// ============================================================================

void aot_dcbz(AOTState* s, uint32_t addr)
{
  auto& mmu = GetMMU();
  // Zero 32 bytes at the cache-line-aligned address
  u32 aligned = addr & ~31u;
  for (u32 i = 0; i < 32; i += 4)
    PowerPC::WriteFromJit<u32>(mmu, 0, aligned + i);
}

void aot_dcbz_l(AOTState* s, uint32_t addr)
{
  aot_dcbz(s, addr);
}

void aot_dcbt(AOTState* s, uint32_t addr)
{
  // Prefetch hint — no-op
}

void aot_icbi(AOTState* s, uint32_t addr)
{
  // Instruction cache block invalidate
  // In AOT mode, this is used for self-modifying code detection.
  // For now, just ignore it — SMC regions are already handled by interpreter fallback.
}

#undef FP_IMPL_3
#undef FP_IMPL_3C
#undef FP_IMPL_4
#undef FP_IMPL_2
#undef FP_CMP

}  // extern "C"
