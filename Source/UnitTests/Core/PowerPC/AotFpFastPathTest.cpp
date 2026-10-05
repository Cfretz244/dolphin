// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The static inline single-precision FP fast paths in aot_runtime.h
// (aot_{faddsx,fsubsx,fmulsx}_fast, aot_ps_{add,sub,mul,muls0,muls1}_fast) must
// be bit-identical to the interpreter: same ps[] contents, same FPSCR, same
// pending exceptions. The oracle is the real interpreter entry point run on the
// same System PPC state (the fast paths' slow-path fallbacks run the interpreter
// on that state, so it must be the system one).

#ifdef DOLPHIN_HAS_AOT
#include <array>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/FPURoundMode.h"
#include "Common/ScopeGuard.h"
#include "Core/Core.h"
#include "Core/PowerPC/AOT/AotState.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "TestValues.h"

namespace
{
using InterpFn = void (*)(Interpreter&, UGeckoInstruction);
using FastFn = void (*)(AOTState*, int, int, int);

struct FpOp
{
  const char* name;
  InterpFn interp;
  FastFn fast;
  bool uses_fc;  // mul family: second operand is FC, else FB
};

constexpr std::array<FpOp, 8> s_ops{{
    {"faddsx", &Interpreter::faddsx, &aot_faddsx_fast, false},
    {"fsubsx", &Interpreter::fsubsx, &aot_fsubsx_fast, false},
    {"fmulsx", &Interpreter::fmulsx, &aot_fmulsx_fast, true},
    {"ps_add", &Interpreter::ps_add, &aot_ps_add_fast, false},
    {"ps_sub", &Interpreter::ps_sub, &aot_ps_sub_fast, false},
    {"ps_mul", &Interpreter::ps_mul, &aot_ps_mul_fast, true},
    {"ps_muls0", &Interpreter::ps_muls0, &aot_ps_muls0_fast, true},
    {"ps_muls1", &Interpreter::ps_muls1, &aot_ps_muls1_fast, true},
}};

struct Snapshot
{
  std::array<PowerPC::PairedSingle, 32> ps;
  u32 fpscr;
  u32 exceptions;
  u32 srr1;

  bool operator==(const Snapshot& o) const
  {
    for (size_t i = 0; i < ps.size(); ++i)
    {
      if (ps[i].ps0 != o.ps[i].ps0 || ps[i].ps1 != o.ps[i].ps1)
        return false;
    }
    return fpscr == o.fpscr && exceptions == o.exceptions && srr1 == o.srr1;
  }
};

std::vector<std::pair<u64, u64>> BuildOperandPairs()
{
  std::vector<std::pair<u64, u64>> pairs;
  for (u64 a : double_test_values)
    for (u64 b : double_test_values)
      pairs.emplace_back(a, b);

  std::mt19937_64 rng(0x5EED'AD07'F00D'0001ULL);
  // Raw bit patterns: hits every exponent, NaN payloads, denormals.
  for (int i = 0; i < 2000; ++i)
  {
    const u64 a = rng();
    pairs.emplace_back(a, rng());
  }
  // Doubles that are exact singles (what games actually keep in FPRs), so the
  // finite fast path is exercised heavily, including results near the
  // single-subnormal boundary that the NI flush handles.
  auto single_as_double = [&rng](bool small) {
    u32 bits = static_cast<u32>(rng());
    if (small)  // exponent in [1, 40]: products/sums land in single-subnormal range
      bits = (bits & 0x807FFFFFu) | ((1u + (static_cast<u32>(rng()) % 40u)) << 23);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    const double d = static_cast<double>(f);
    u64 out;
    std::memcpy(&out, &d, sizeof(out));
    return out;
  };
  for (int i = 0; i < 2000; ++i)
  {
    const u64 a = single_as_double(false);
    pairs.emplace_back(a, single_as_double(false));
  }
  for (int i = 0; i < 2000; ++i)
  {
    const u64 a = single_as_double(true);
    pairs.emplace_back(a, single_as_double(i & 1));
  }
  // ForceSingle's NI quirk: a double just below the smallest normal single
  // (0x3810000000000000) that would ROUND UP to it is still flushed to zero.
  // Hit that window directly: x op identity (x + 0, x * 1.0) in both orders.
  constexpr u64 smallest_normal_single = 0x3810'0000'0000'0000ULL;
  constexpr u64 sign = 0x8000'0000'0000'0000ULL;
  constexpr u64 pos_zero = 0, one = 0x3FF0'0000'0000'0000ULL;
  for (int i = 0; i < 1000; ++i)
  {
    const u64 below = smallest_normal_single - 1 - (rng() & 0x3FFF'FFFFULL);
    const u64 above = smallest_normal_single + (rng() & 0x3FFF'FFFFULL);
    for (u64 x : {below, above, below | sign, above | sign})
    {
      pairs.emplace_back(x, pos_zero);
      pairs.emplace_back(x, pos_zero | sign);
      pairs.emplace_back(x, one);
      pairs.emplace_back(one, x);
    }
  }
  return pairs;
}
}  // namespace

class AotFpFastPathTest : public testing::Test
{
protected:
  Core::System& system = Core::System::GetInstance();
  Snapshot saved{};
  u32 saved_msr = 0;

  void SetUp() override
  {
    Core::DeclareAsCPUThread();
    saved = Take();
    saved_msr = system.GetPPCState().msr.Hex;
  }

  void TearDown() override
  {
    auto& state = system.GetPPCState();
    for (size_t i = 0; i < saved.ps.size(); ++i)
      state.ps[i] = saved.ps[i];
    state.fpscr.Hex = saved.fpscr;
    state.Exceptions = saved.exceptions;
    state.spr[SPR_SRR1] = saved.srr1;
    state.msr.Hex = saved_msr;
    Core::UndeclareAsCPUThread();
  }

  Snapshot Take() const
  {
    const auto& state = system.GetPPCState();
    Snapshot snap;
    for (size_t i = 0; i < snap.ps.size(); ++i)
      snap.ps[i] = state.ps[i];
    snap.fpscr = state.fpscr.Hex;
    snap.exceptions = state.Exceptions;
    snap.srr1 = state.spr[SPR_SRR1];
    return snap;
  }

  void Load(u64 a, u64 b, u32 fpscr, u32 msr)
  {
    auto& state = system.GetPPCState();
    for (int i = 0; i < 32; ++i)
    {
      state.ps[i].SetBoth(u64{0x7FF0'DEAD'BEEF'0000ULL} | u64(i),
                          u64{0xFFF0'0BAD'F00D'0000ULL} | u64(i));
    }
    state.ps[1].SetBoth(a, b);
    state.ps[2].SetBoth(b, a);
    state.fpscr.Hex = fpscr;
    state.msr.Hex = msr;
    state.Exceptions = 0;
    state.spr[SPR_SRR1] = 0;
  }

  // Returns the number of mismatches (reports the first few in detail).
  int RunOne(const FpOp& op, int fd, u64 a, u64 b, u32 fpscr, u32 msr, int& reported)
  {
    UGeckoInstruction inst{};
    inst.FD = fd;
    inst.FA = 1;
    if (op.uses_fc)
      inst.FC = 2;
    else
      inst.FB = 2;
    inst.Rc = 0;

    Load(a, b, fpscr, msr);
    op.interp(system.GetInterpreter(), inst);
    const Snapshot expected = Take();

    Load(a, b, fpscr, msr);
    op.fast(ToAot(system.GetPPCState()), fd, 1, 2);
    const Snapshot actual = Take();

    if (expected == actual)
      return 0;
    if (reported++ < 10)
    {
      ADD_FAILURE() << fmt::format(
          "{} fd={} a={:016x} b={:016x} fpscr_in={:08x} msr={:08x}: "
          "interp ps[fd]={:016x}/{:016x} fpscr={:08x} exc={:x} srr1={:x} | "
          "fast ps[fd]={:016x}/{:016x} fpscr={:08x} exc={:x} srr1={:x}",
          op.name, fd, a, b, fpscr, msr, expected.ps[fd].ps0, expected.ps[fd].ps1, expected.fpscr,
          expected.exceptions, expected.srr1, actual.ps[fd].ps0, actual.ps[fd].ps1, actual.fpscr,
          actual.exceptions, actual.srr1);
    }
    return 1;
  }
};

TEST_F(AotFpFastPathTest, BitIdenticalToInterpreter)
{
  const std::vector<std::pair<u64, u64>> pairs = BuildOperandPairs();

  Common::ScopeGuard restore_mode{
      [] { Common::FPU::SetSIMDMode(Common::FPU::RoundMode::ROUND_NEAR, false); }};

  // MSR.FE0|FE1 so that an enabled FP exception raises a program exception in
  // the interpreter -- the fast path must produce the same (or defer to it).
  constexpr u32 msr_fe = (1u << 11) | (1u << 8);

  int total_mismatches = 0;
  u64 cases = 0;
  // Every FPSCR.RN x NI combination, with the host SIMD mode set exactly as
  // RoundingModeUpdated() would for that FPSCR (the AOT fast paths, like the
  // interpreter, rely on the host mode for rounding and flush-to-zero).
  for (u32 rn = 0; rn < 4; ++rn)
  {
    for (const bool ni : {false, true})
    {
      Common::FPU::SetSIMDMode(static_cast<Common::FPU::RoundMode>(rn), ni);
      const u32 mode_bits = (ni ? (1u << 2) : 0u) | rn;

      struct Env
      {
        u32 fpscr;
        u32 msr;
      };
      const std::array<Env, 3> envs{{
          // Clean FPSCR: only RN/NI.
          {mode_bits, 0},
          // Stale FPRF/FI/FR + sticky exception bits, no enables.
          {0xFFFF'F000u | mode_bits, 0},
          // Same plus every enable bit (VE/OE/UE/ZE/XE) and FE0|FE1 in MSR.
          {0xFFFF'F0F8u | mode_bits, msr_fe},
      }};

      for (const FpOp& op : s_ops)
      {
        int reported = 0;
        int op_mismatches = 0;
        for (const Env& env : envs)
        {
          for (const auto& [a, b] : pairs)
          {
            // fd distinct, fd == fa, fd == fb/fc.
            for (const int fd : {3, 1, 2})
            {
              op_mismatches += RunOne(op, fd, a, b, env.fpscr, env.msr, reported);
              ++cases;
            }
          }
        }
        EXPECT_EQ(op_mismatches, 0) << op.name << " RN=" << rn << " NI=" << ni;
        total_mismatches += op_mismatches;
      }
    }
  }
  std::printf("AotFpFastPathTest: %llu cases, %d mismatches\n",
              static_cast<unsigned long long>(cases), total_mismatches);
  EXPECT_EQ(total_mismatches, 0);
}
#endif  // DOLPHIN_HAS_AOT
