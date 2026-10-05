// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The static inline single-precision FP fast paths in aot_runtime.h
// (aot_{faddsx,fsubsx,fmulsx}_fast, aot_ps_{add,sub,mul,muls0,muls1}_fast, the
// fmadds family aot_{fmaddsx,fmsubsx,fnmaddsx,fnmsubsx}_fast and
// aot_ps_{madd,msub,nmadd,nmsub,madds0,madds1}_fast) must be bit-identical to
// the interpreter: same ps[] contents, same FPSCR, same pending exceptions. The
// single<->double bit converts (aot_convert_to_{double,single,single_ftz}_fast)
// must match Interpreter_FPUtils.h ConvertToDouble/ConvertToSingle/
// ConvertToSingleFTZ. The oracle is the real interpreter entry point run on the
// same System PPC state (the fast paths' slow-path fallbacks run the interpreter
// on that state, so it must be the system one).

#ifdef DOLPHIN_HAS_AOT
#include <array>
#include <bit>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Common/FPURoundMode.h"
#include "Common/ScopeGuard.h"
#include "Core/Core.h"
#include "Core/PowerPC/AOT/AotState.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
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

using FmaFastFn = void (*)(AOTState*, int, int, int, int);

struct FmaOp
{
  const char* name;
  InterpFn interp;
  FmaFastFn fast;
};

constexpr std::array<FmaOp, 10> s_fma_ops{{
    {"fmaddsx", &Interpreter::fmaddsx, &aot_fmaddsx_fast},
    {"fmsubsx", &Interpreter::fmsubsx, &aot_fmsubsx_fast},
    {"fnmaddsx", &Interpreter::fnmaddsx, &aot_fnmaddsx_fast},
    {"fnmsubsx", &Interpreter::fnmsubsx, &aot_fnmsubsx_fast},
    {"ps_madd", &Interpreter::ps_madd, &aot_ps_madd_fast},
    {"ps_msub", &Interpreter::ps_msub, &aot_ps_msub_fast},
    {"ps_nmadd", &Interpreter::ps_nmadd, &aot_ps_nmadd_fast},
    {"ps_nmsub", &Interpreter::ps_nmsub, &aot_ps_nmsub_fast},
    {"ps_madds0", &Interpreter::ps_madds0, &aot_ps_madds0_fast},
    {"ps_madds1", &Interpreter::ps_madds1, &aot_ps_madds1_fast},
}};

struct Triple
{
  u64 a, c, b;
};

// One paired-single test case: ps0 halves (a0, c0, b0), ps1 halves (a1, c1, b1).
struct FmaCase
{
  Triple lo, hi;
};

u64 SingleAsDouble(u32 bits)
{
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return std::bit_cast<u64>(static_cast<double>(f));
}

// The interpreter comment's Mario Strikers Charged example (singles):
// fmadds(a, c, b) must be 0xbf55bf17, naive f32(fma(...)) gives 0xbf55bf18.
const Triple s_strikers_tie{SingleAsDouble(0x42480000u), SingleAsDouble(0xbc88cc38u),
                            SingleAsDouble(0x1b1c72a0u)};

// Triples whose double FMA (a * Force25Bit(c) + b, RN) lands exactly on the
// "even tie" pattern (low 29 bits == 0x10000000) the interpreter corrects.
// Built at host RN: p = a*c (exact: both singles), t = p with its low 29 bits
// replaced by the tie pattern, b0 = t - p (exact, Sterbenz), then b = b0 nudged
// by k ulps of b0 -- far below t's ulp, so the FMA still rounds to t but with
// a nonzero error of either sign (k = 0 is the exact-tie, no-correction case).
// Each triple is also emitted with b negated so the msub forms tie as well.
std::vector<Triple> BuildTieTriples(std::mt19937_64& rng)
{
  std::vector<Triple> out;
  auto rand_single = [&rng] {
    u32 bits = static_cast<u32>(rng());
    // exponent in [100, 155]: product well inside the normal range.
    bits = (bits & 0x807FFFFFu) | ((100u + static_cast<u32>(rng() % 56u)) << 23);
    return bits;
  };
  for (int i = 0; i < 300; ++i)
  {
    const u64 a = SingleAsDouble(rand_single());
    const u64 c = SingleAsDouble(rand_single());
    const double p = std::bit_cast<double>(a) * std::bit_cast<double>(c);
    const u64 t_bits = (std::bit_cast<u64>(p) & ~u64{0x1FFF'FFFF}) | u64{0x1000'0000};
    const double b0 = std::bit_cast<double>(t_bits) - p;
    for (const s64 k : {0, 1, -1, 3, -7})
    {
      if (b0 == 0.0 && k != 0)
        continue;
      const u64 b = std::bit_cast<u64>(b0) + static_cast<u64>(k);
      out.push_back({a, c, b});
      out.push_back({a, c, b ^ 0x8000'0000'0000'0000ULL});
    }
  }
  return out;
}

std::vector<FmaCase> BuildFmaCases()
{
  std::mt19937_64 rng(0x5EED'AD07'F00D'0002ULL);
  std::vector<Triple> triples;
  // All (a, b) pairs from the test values, c cycling through them.
  size_t ci = 0;
  for (u64 a : double_test_values)
  {
    for (u64 b : double_test_values)
    {
      triples.push_back({a, double_test_values[ci], b});
      ci = (ci + 1) % double_test_values.size();
    }
  }
  // Raw bit patterns: every exponent, NaN payloads, denormals.
  for (int i = 0; i < 5000; ++i)
  {
    const u64 a = rng(), c = rng();
    triples.push_back({a, c, rng()});
  }
  // Exact singles (what games keep in FPRs).
  for (int i = 0; i < 8000; ++i)
  {
    const u64 a = SingleAsDouble(static_cast<u32>(rng()));
    const u64 c = SingleAsDouble(static_cast<u32>(rng()));
    triples.push_back({a, c, SingleAsDouble(static_cast<u32>(rng()))});
  }
  // Small exponents: results in / near the single-subnormal range (NI flush).
  auto small_single = [&rng] {
    u32 bits = static_cast<u32>(rng());
    return SingleAsDouble((bits & 0x807FFFFFu) | ((1u + static_cast<u32>(rng() % 70u)) << 23));
  };
  for (int i = 0; i < 4000; ++i)
  {
    const u64 a = small_single(), c = small_single();
    triples.push_back({a, c, small_single()});
  }
  // Heavy cancellation: b ~= -(a*c) rounded to single.
  for (int i = 0; i < 3000; ++i)
  {
    const u64 a = SingleAsDouble(static_cast<u32>(rng()) & 0xBFFF'FFFFu);
    const u64 c = SingleAsDouble(static_cast<u32>(rng()) & 0xBFFF'FFFFu);
    const float prod = static_cast<float>(std::bit_cast<double>(a) * std::bit_cast<double>(c));
    triples.push_back({a, c, std::bit_cast<u64>(-static_cast<double>(prod))});
  }
  // Full-double (non-single) operands with normal exponents: Force25Bit rounding.
  for (int i = 0; i < 2000; ++i)
  {
    auto mid = [&rng] {
      return (rng() & 0x800F'FFFF'FFFF'FFFFULL) | (u64{900 + rng() % 250} << 52);
    };
    const u64 a = mid(), c = mid();
    triples.push_back({a, c, mid()});
  }
  const std::vector<Triple> ties = BuildTieTriples(rng);
  triples.push_back(s_strikers_tie);

  std::vector<FmaCase> cases;
  const size_t n = triples.size();
  for (size_t i = 0; i < n; ++i)
  {
    // Even: same triple in both halves (ps_madds0/1 then see c0 == c1);
    // odd: an unrelated triple in ps1.
    cases.push_back({triples[i], (i & 1) ? triples[(i * 7919 + 1) % n] : triples[i]});
  }
  // Tie triples: in ps0 only, in ps1 only, and in both.
  for (size_t i = 0; i < ties.size(); ++i)
  {
    const Triple& benign = triples[(i * 104729) % n];
    cases.push_back({ties[i], ties[i]});
    cases.push_back({ties[i], benign});
    cases.push_back({benign, ties[i]});
  }
  cases.push_back({s_strikers_tie, s_strikers_tie});
  return cases;
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

  // fa=1, fc=2, fb=4; fd varies (distinct, and aliasing each operand).
  void LoadFma(const FmaCase& fc, u32 fpscr, u32 msr)
  {
    Load(0, 0, fpscr, msr);
    auto& state = system.GetPPCState();
    state.ps[1].SetBoth(fc.lo.a, fc.hi.a);
    state.ps[2].SetBoth(fc.lo.c, fc.hi.c);
    state.ps[4].SetBoth(fc.lo.b, fc.hi.b);
  }

  int RunFma(const FmaOp& op, int fd, const FmaCase& fc, u32 fpscr, u32 msr, int& reported)
  {
    UGeckoInstruction inst{};
    inst.OPCD = 59;
    inst.FD = fd;
    inst.FA = 1;
    inst.FC = 2;
    inst.FB = 4;
    inst.Rc = 0;

    LoadFma(fc, fpscr, msr);
    op.interp(system.GetInterpreter(), inst);
    const Snapshot expected = Take();

    LoadFma(fc, fpscr, msr);
    op.fast(ToAot(system.GetPPCState()), fd, 1, 2, 4);
    const Snapshot actual = Take();

    if (expected == actual)
      return 0;
    if (reported++ < 10)
    {
      ADD_FAILURE() << fmt::format(
          "{} fd={} a={:016x}/{:016x} c={:016x}/{:016x} b={:016x}/{:016x} fpscr_in={:08x} "
          "msr={:08x}: interp ps[fd]={:016x}/{:016x} fpscr={:08x} exc={:x} srr1={:x} | "
          "fast ps[fd]={:016x}/{:016x} fpscr={:08x} exc={:x} srr1={:x}",
          op.name, fd, fc.lo.a, fc.hi.a, fc.lo.c, fc.hi.c, fc.lo.b, fc.hi.b, fpscr, msr,
          expected.ps[fd].ps0, expected.ps[fd].ps1, expected.fpscr, expected.exceptions,
          expected.srr1, actual.ps[fd].ps0, actual.ps[fd].ps1, actual.fpscr, actual.exceptions,
          actual.srr1);
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

TEST_F(AotFpFastPathTest, FmaddsFamilyBitIdenticalToInterpreter)
{
  const std::vector<FmaCase> cases = BuildFmaCases();

  Common::ScopeGuard restore_mode{
      [] { Common::FPU::SetSIMDMode(Common::FPU::RoundMode::ROUND_NEAR, false); }};
  constexpr u32 msr_fe = (1u << 11) | (1u << 8);

  int total_mismatches = 0;
  u64 runs = 0;
  for (u32 rn = 0; rn < 4; ++rn)
  {
    for (const bool ni : {false, true})
    {
      Common::FPU::SetSIMDMode(static_cast<Common::FPU::RoundMode>(rn), ni);
      const u32 mode_bits = (ni ? (1u << 2) : 0u) | rn;
      const std::array<std::pair<u32, u32>, 3> envs{{
          {mode_bits, 0},
          {0xFFFF'F000u | mode_bits, 0},
          {0xFFFF'F0F8u | mode_bits, msr_fe},
      }};
      for (const FmaOp& op : s_fma_ops)
      {
        int reported = 0;
        int op_mismatches = 0;
        for (const auto& [fpscr, msr] : envs)
        {
          for (const FmaCase& fc : cases)
          {
            for (const int fd : {3, 1, 2, 4})
            {
              op_mismatches += RunFma(op, fd, fc, fpscr, msr, reported);
              ++runs;
            }
          }
        }
        EXPECT_EQ(op_mismatches, 0) << op.name << " RN=" << rn << " NI=" << ni;
        total_mismatches += op_mismatches;
      }
    }
  }
  std::printf("AotFpFastPathTest fmadds family: %zu cases, %llu runs, %d mismatches\n",
              cases.size(), static_cast<unsigned long long>(runs), total_mismatches);
  EXPECT_EQ(total_mismatches, 0);
}

// The constructed tie triples really hit the interpreter's even-tie correction
// pattern, the fast-path core declines every one of them (=> exact helper),
// and the fallback is load-bearing: a naive f32(fma(...)) gets some wrong.
TEST_F(AotFpFastPathTest, FmaddsTieCasesTakeExactPath)
{
  Common::FPU::SetSIMDMode(Common::FPU::RoundMode::ROUND_NEAR, false);
  std::mt19937_64 rng(0x5EED'AD07'F00D'0002ULL);
  std::vector<Triple> ties = BuildTieTriples(rng);
  ties.push_back(s_strikers_tie);

  int madd_ties = 0, msub_ties = 0, declined = 0, naive_wrong = 0;
  for (const Triple& t : ties)
  {
    const double a = std::bit_cast<double>(t.a), c = std::bit_cast<double>(t.c),
                 b = std::bit_cast<double>(t.b);
    for (const int sub : {0, 1})
    {
      const double r = std::fma(a, Force25Bit(c), sub ? -b : b);
      if ((std::bit_cast<u64>(r) & 0x1FFF'FFFFULL) != 0x1000'0000ULL)
        continue;
      (sub ? msub_ties : madd_ties)++;
      double out = 0.0;
      if (aot_fmadds_core(a, c, b, sub, &out) == 0)
        ++declined;
      // What the interpreter produces (fmaddsx / fmsubsx, ps0).
      Load(0, 0, 0, 0);
      auto& state = system.GetPPCState();
      state.ps[1].SetBoth(t.a, t.a);
      state.ps[2].SetBoth(t.c, t.c);
      state.ps[4].SetBoth(t.b, t.b);
      UGeckoInstruction inst{};
      inst.OPCD = 59;
      inst.FD = 3;
      inst.FA = 1;
      inst.FC = 2;
      inst.FB = 4;
      if (sub)
        Interpreter::fmsubsx(system.GetInterpreter(), inst);
      else
        Interpreter::fmaddsx(system.GetInterpreter(), inst);
      const u64 interp = state.ps[3].PS0AsU64();
      if (std::bit_cast<u64>(static_cast<double>(static_cast<float>(r))) != interp)
        ++naive_wrong;
    }
  }
  std::printf("AotFpFastPathTest ties: %zu triples, madd ties %d, msub ties %d, declined %d, "
              "naive f32(fma) wrong %d\n",
              ties.size(), madd_ties, msub_ties, declined, naive_wrong);
  EXPECT_GE(madd_ties, 1000);
  EXPECT_GE(msub_ties, 1000);
  EXPECT_EQ(declined, madd_ties + msub_ties);
  EXPECT_GT(naive_wrong, 0);

  // The Strikers example end to end through the fast entry point.
  Load(0, 0, 0, 0);
  auto& state = system.GetPPCState();
  state.ps[1].SetBoth(s_strikers_tie.a, s_strikers_tie.a);
  state.ps[2].SetBoth(s_strikers_tie.c, s_strikers_tie.c);
  state.ps[4].SetBoth(s_strikers_tie.b, s_strikers_tie.b);
  double out = 0.0;
  EXPECT_EQ(aot_fmadds_core(std::bit_cast<double>(s_strikers_tie.a),
                            std::bit_cast<double>(s_strikers_tie.c),
                            std::bit_cast<double>(s_strikers_tie.b), 0, &out),
            0);
  aot_fmaddsx_fast(ToAot(state), 3, 1, 2, 4);
  EXPECT_EQ(state.ps[3].PS0AsU64(), SingleAsDouble(0xbf55bf17u));
  EXPECT_EQ(state.ps[3].PS1AsU64(), SingleAsDouble(0xbf55bf17u));
}

TEST(AotFpConvertTest, ConvertToDoubleExhaustive)
{
  u64 mismatches = 0;
  u32 first_bad = 0;
  for (u64 i = 0; i <= 0xFFFF'FFFFULL; ++i)
  {
    const u32 v = static_cast<u32>(i);
    if (aot_convert_to_double_fast(v) != ConvertToDouble(v))
    {
      if (mismatches++ == 0)
        first_bad = v;
    }
  }
  EXPECT_EQ(mismatches, 0u) << fmt::format("first mismatch at {:08x}: fast {:016x} ref {:016x}",
                                           first_bad, aot_convert_to_double_fast(first_bad),
                                           ConvertToDouble(first_bad));
}

TEST(AotFpConvertTest, ConvertToSingleAndFTZ)
{
  std::vector<u64> inputs(double_test_values.begin(), double_test_values.end());
  for (u32 sv : single_test_values)
    inputs.push_back(ConvertToDouble(sv));
  std::mt19937_64 rng(0x5EED'AD07'F00D'0003ULL);
  for (int i = 0; i < (1 << 20); ++i)
    inputs.push_back(rng());
  constexpr u64 mantissas[] = {0, 1, 0x0000'0000'1FFF'FFFFULL, 0x0000'0000'2000'0000ULL,
                               0x0008'0000'0000'0000ULL, 0x000F'FFFF'FFFF'FFFFULL,
                               0x0005'5555'5555'5555ULL, 0x000A'AAAA'AAAA'AAAAULL};
  for (u64 exp = 0; exp < 2048; ++exp)
  {
    for (u64 m : mantissas)
    {
      inputs.push_back((exp << 52) | m);
      inputs.push_back((exp << 52) | m | 0x8000'0000'0000'0000ULL);
      inputs.push_back((exp << 52) | (m ^ (rng() & 0x000F'FFFF'FFFF'FFFFULL)));
    }
  }
  int bad_single = 0, bad_ftz = 0;
  for (u64 x : inputs)
  {
    if (aot_convert_to_single_fast(x) != ConvertToSingle(x) && bad_single++ < 5)
      ADD_FAILURE() << fmt::format("convert_to_single {:016x}: fast {:08x} ref {:08x}", x,
                                   aot_convert_to_single_fast(x), ConvertToSingle(x));
    if (aot_convert_to_single_ftz_fast(x) != ConvertToSingleFTZ(x) && bad_ftz++ < 5)
      ADD_FAILURE() << fmt::format("convert_to_single_ftz {:016x}: fast {:08x} ref {:08x}", x,
                                   aot_convert_to_single_ftz_fast(x), ConvertToSingleFTZ(x));
  }
  std::printf("AotFpConvertTest: %zu single/ftz inputs\n", inputs.size());
  EXPECT_EQ(bad_single, 0);
  EXPECT_EQ(bad_ftz, 0);
}

// psq_l / psq_st inline float-GQR path against a private RAM buffer (the
// diff gates prove it in-game; this pins the byte order, W=1 handling and the
// ps1 = 1.0 rule against FastDequantize/FastQuantize's float case: big-endian
// u32s through ConvertToDouble / ConvertToSingleFTZ). Only the inline path is
// exercised: every case satisfies LSQE, float type and a RAM address.
TEST(AotFpPsqTest, FloatPathMatchesConvert)
{
  alignas(8) static u8 ram[0x1000];
  const AotFastMem fm{ram, sizeof(ram), nullptr, 0};

  AOTState s{};
  s.spr[920] = 0x8000'0000u;  // HID2.LSQE
  for (int i = 0; i < 8; ++i)  // float ld/st types, garbage scales (ignored for float)
    s.spr[912 + i] = 0x3F00'3F00u;

  std::mt19937_64 rng(0x5EED'AD07'F00D'0004ULL);
  std::vector<u64> doubles(double_test_values.begin(), double_test_values.end());
  for (int i = 0; i < 20000; ++i)
    doubles.push_back(rng());
  int bad = 0;
  for (size_t k = 0; k + 1 < doubles.size(); ++k)
  {
    const u32 ea = 0x8000'0100u + 8u * static_cast<u32>(k % 64);
    const u32 off = ea - 0x8000'0000u;
    const int gqr = static_cast<int>(k % 8);
    for (const int w : {0, 1})
    {
      // store
      std::memset(ram + off, 0xA5, 8);
      s.ps[5].ps0 = doubles[k];
      s.ps[5].ps1 = doubles[k + 1];
      aot_psq_st_fast(&s, &fm, 5, 1, ea, gqr, w, 0);
      const u32 w0 = Common::swap32(ConvertToSingleFTZ(doubles[k]));
      const u32 w1 = w ? 0xA5A5'A5A5u : Common::swap32(ConvertToSingleFTZ(doubles[k + 1]));
      u32 got0, got1;
      std::memcpy(&got0, ram + off, 4);
      std::memcpy(&got1, ram + off + 4, 4);
      if ((got0 != w0 || got1 != w1) && bad++ < 5)
        ADD_FAILURE() << fmt::format("psq_st w={} {:016x}/{:016x}: got {:08x} {:08x} want {:08x} {:08x}",
                                     w, doubles[k], doubles[k + 1], got0, got1, w0, w1);
      // load back the raw words
      const u32 r0 = static_cast<u32>(doubles[k] >> 32), r1 = static_cast<u32>(doubles[k + 1]);
      const u32 be0 = Common::swap32(r0), be1 = Common::swap32(r1);
      std::memcpy(ram + off, &be0, 4);
      std::memcpy(ram + off + 4, &be1, 4);
      s.ps[6].ps0 = s.ps[6].ps1 = 0;
      aot_psq_l_fast(&s, &fm, 6, 1, ea, gqr, w, 0);
      const u64 want0 = ConvertToDouble(r0);
      const u64 want1 = w ? 0x3FF0'0000'0000'0000ULL : ConvertToDouble(r1);
      if ((s.ps[6].ps0 != want0 || s.ps[6].ps1 != want1) && bad++ < 5)
        ADD_FAILURE() << fmt::format("psq_l w={} {:08x}/{:08x}: got {:016x}/{:016x} want {:016x}/{:016x}",
                                     w, r0, r1, s.ps[6].ps0, s.ps[6].ps1, want0, want1);
    }
  }
  EXPECT_EQ(bad, 0);
}

// The `_fm` memory helpers (descriptor passed by pointer, what generated
// blocks call) must resolve every address exactly like the global-descriptor
// helpers, and their fast paths must move the same bytes. Slow paths need a
// booted system, so loads/stores are only issued at addresses that resolve.
TEST(AotFpFastMemTest, FmHelpersMatchGlobalHelpers)
{
  alignas(8) static u8 mem1[0x2000];
  alignas(8) static u8 mem2[0x1000];
  const AotFastMem fm{mem1, sizeof(mem1), mem2, sizeof(mem2)};
  const AotFastMem saved_mem = aot_fast_mem;
  aot_fast_mem = fm;
  Common::ScopeGuard restore{[&] { aot_fast_mem = saved_mem; }};

  std::mt19937_64 rng(0x5EED'AD07'F00D'0005ULL);
  // Edges of both windows (cached/uncached), plus random addresses.
  std::vector<u32> addrs;
  for (const u32 base : {0x8000'0000u, 0xC000'0000u, 0x9000'0000u, 0xD000'0000u})
    for (const u32 d : {0u, 1u, 7u, 0xFF8u, 0xFFFu, 0x1000u, 0x1FF8u, 0x1FFFu, 0x2000u,
                        0xFFFF'FFFFu, 0xFFFF'FFF8u})
      addrs.push_back(base + d);
  for (const u32 a : {0u, 0x0000'1000u, 0x4000'0000u, 0x7FFF'FFFFu, 0xFFFF'FFFFu})
    addrs.push_back(a);
  for (int i = 0; i < 200000; ++i)
  {
    const u64 r = rng();
    // Half anywhere, half biased into the two windows.
    const u32 a = static_cast<u32>(r);
    addrs.push_back((r >> 63) ? a : ((a & 0xD000'3FFFu) | 0x8000'0000u));
  }

  AOTState s{};
  int bad = 0;
  for (const u32 a : addrs)
  {
    u8* const p = aot_host_ptr_fm(&fm, a);
    if ((p != aot_host_ptr(a) || aot_is_ram_fm(&fm, a) != aot_is_ram(a)) && bad++ < 5)
      ADD_FAILURE() << fmt::format("host_ptr {:08x}: fm {} global {}", a, fmt::ptr(p),
                                   fmt::ptr(aot_host_ptr(a)));
    if (!p || aot_host_ptr_fm(&fm, a + 7) != p + 7)
      continue;
    const u64 v = rng();
    // Stores: _fm variant and global variant must leave identical bytes.
    u8 want[8], got[8];
    const auto check = [&](const char* what, auto&& store_fm, auto&& store_global) {
      std::memset(p, 0x5A, 8);
      store_global();
      std::memcpy(want, p, 8);
      std::memset(p, 0x5A, 8);
      store_fm();
      std::memcpy(got, p, 8);
      if (std::memcmp(want, got, 8) != 0 && bad++ < 5)
        ADD_FAILURE() << fmt::format("{} {:08x} {:016x}", what, a, v);
    };
    check("w8", [&] { aot_write_u8_fm(&s, &fm, static_cast<u32>(v), a); },
          [&] { aot_write_u8(&s, static_cast<u32>(v), a); });
    check("w16", [&] { aot_write_u16_fm(&s, &fm, static_cast<u32>(v), a); },
          [&] { aot_write_u16(&s, static_cast<u32>(v), a); });
    check("w16br", [&] { aot_write_u16_br_fm(&s, &fm, static_cast<u32>(v), a); },
          [&] { aot_write_u16_br(&s, static_cast<u32>(v), a); });
    check("w32", [&] { aot_write_u32_fm(&s, &fm, static_cast<u32>(v), a); },
          [&] { aot_write_u32(&s, static_cast<u32>(v), a); });
    check("w64", [&] { aot_write_u64_fm(&s, &fm, v, a); }, [&] { aot_write_u64(&s, v, a); });
    // Loads.
    std::memcpy(p, &v, 8);
    if ((aot_read_u8_fm(&s, &fm, a) != aot_read_u8(&s, a) ||
         aot_read_u16_fm(&s, &fm, a) != aot_read_u16(&s, a) ||
         aot_read_u16_se_fm(&s, &fm, a) != aot_read_u16_se(&s, a) ||
         aot_read_u32_fm(&s, &fm, a) != aot_read_u32(&s, a) ||
         aot_read_u64_fm(&s, &fm, a) != aot_read_u64(&s, a)) &&
        bad++ < 5)
      ADD_FAILURE() << fmt::format("read {:08x} {:016x}", a, v);
  }
  std::printf("AotFpFastMemTest: %zu addresses\n", addrs.size());
  EXPECT_EQ(bad, 0);
}
#endif  // DOLPHIN_HAS_AOT
