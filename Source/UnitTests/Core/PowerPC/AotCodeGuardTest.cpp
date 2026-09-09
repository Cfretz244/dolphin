// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef DOLPHIN_HAS_AOT
#include <array>
#include <chrono>
#include <cstdio>

#include <gtest/gtest.h>

#include "Common/ChunkFile.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

extern "C" void aot_init_fast_mem();
extern "C" void aot_shutdown();
extern "C" int aot_match_code(u32 pc, const u32* words, u32 count);

class AotCodeGuardTest : public testing::Test
{
protected:
  Core::System& system = Core::System::GetInstance();
  PowerPC::BatTable saved_bats{};
  u32 saved_msr = 0, saved_hid0 = 0;
  bool saved_wii = false, saved_disable_icache = false;
  std::array<u32, 12> words{0x38600001, 0x38800002, 0x38a00003, 0x60000000,
                            0x60000000, 0x60000000, 0x60000000, 0x60000000,
                            0x60000000, 0x60000000, 0x60000000, 0x4e800020};

  void SetUp() override
  {
    saved_wii = system.IsWii();
    system.SetIsWii(true);
    system.GetMemory().Init();
    Core::DeclareAsCPUThread();
    auto& state = system.GetPPCState();
    saved_msr = state.msr.Hex;
    saved_hid0 = HID0(state).Hex;
    saved_bats = system.GetMMU().GetIBATTable();
    state.msr.Hex = 0;
    HID0(state).ICE = 1;
    HID0(state).ILOCK = 0;
    static_cast<PowerPC::Cache&>(state.iCache).Init(system.GetMemory());
    saved_disable_icache = state.iCache.m_disable_icache;
    state.iCache.m_disable_icache = false;
    aot_init_fast_mem();
  }
  void TearDown() override
  {
    aot_shutdown();
    auto& state = system.GetPPCState();
    state.msr.Hex = saved_msr;
    HID0(state).Hex = saved_hid0;
    state.iCache.m_disable_icache = saved_disable_icache;
    system.GetMMU().GetIBATTable() = saved_bats;
    static_cast<PowerPC::Cache&>(state.iCache).Reset();
    Core::UndeclareAsCPUThread();
    system.GetMemory().Shutdown();
    system.SetIsWii(saved_wii);
  }
  void Write(u32 physical)
  {
    for (u32 i = 0; i < words.size(); ++i)
      system.GetMemory().Write_U32(words[i], physical + 4 * i);
  }
  int Match(u32 pc) { return aot_match_code(pc, words.data(), words.size()); }
  void Warm(u32 pc)
  {
    ASSERT_EQ(Match(pc), 1);  // Fill cache.
    ASSERT_EQ(Match(pc), 1);  // Validate resident words.
    ASSERT_EQ(Match(pc), 1);  // Reuse validation.
  }
};

TEST_F(AotCodeGuardTest, WritesRemainInvisibleUntilIcbi)
{
  Write(0x101c);  // Three cache lines, including a partial first line.
  Warm(0x101c);  // Fill via MMU.
  Warm(0x101c);  // Resident fast path.
  system.GetMemory().Write_U32(0x38600002, 0x101c);
  EXPECT_EQ(Match(0x101c), 1);
  system.GetPPCState().iCache.Invalidate(system.GetMemory(), system.GetJitInterface(), 0x101c);
  EXPECT_EQ(Match(0x101c), 0);
  words[0] = 0x38600002;
  EXPECT_EQ(Match(0x101c), 1);
}

TEST_F(AotCodeGuardTest, BatRemappingSelectsNewPhysicalCode)
{
  Write(0x1000);
  words[0] = 0x38600002;
  Write(0x21000);
  words[0] = 0x38600001;
  auto& bat = system.GetMMU().GetIBATTable()[0x80001000 >> PowerPC::BAT_INDEX_SHIFT];
  bat = PowerPC::BAT_MAPPED_BIT;
  system.GetPPCState().msr.IR = 1;
  Warm(0x80001000);
  bat = 0x20000 | PowerPC::BAT_MAPPED_BIT;
  EXPECT_EQ(Match(0x80001000), 0);
  words[0] = 0x38600002;
  EXPECT_EQ(Match(0x80001000), 1);
}

TEST_F(AotCodeGuardTest, TranslationModeAndVirtualAddressArePartOfReuse)
{
  Write(0x1000);
  words[0] = 0x38600002;
  Write(0x21000);
  words[0] = 0x38600001;
  Warm(0x1000);
  // The same immutable expected array does not certify a different address.
  EXPECT_EQ(Match(0x21000), 0);
  auto& state = system.GetPPCState();
  auto& bat = system.GetMMU().GetIBATTable()[0];
  bat = 0x20000 | PowerPC::BAT_MAPPED_BIT;
  state.msr.IR = 1;
  EXPECT_EQ(Match(0x1000), 0);
  bat = 0;  // An unmapped BAT cannot reuse a translation-disabled memo.
  EXPECT_EQ(Match(0x1000), 0);
  state.msr.IR = 0;
  EXPECT_EQ(Match(0x1000), 1);
  // Bounds must still be rejected even with a populated memo for this array.
  EXPECT_EQ(aot_match_code(0x1001, words.data(), words.size()), 0);
  EXPECT_EQ(aot_match_code(0x1000, words.data(), 0), 0);
}

TEST_F(AotCodeGuardTest, EvictionExposesModifiedRam)
{
  Write(0x1000);
  Warm(0x1000);
  system.GetMemory().Write_U32(0x38600002, 0x1000);
  for (u32 i = 1; i <= 16; ++i)
    system.GetPPCState().iCache.ReadInstruction(system.GetMemory(), system.GetPPCState(),
                                               0x1000 + i * 4096);
  EXPECT_EQ(Match(0x1000), 0);
  words[0] = 0x38600002;
  EXPECT_EQ(Match(0x1000), 1);
}

TEST_F(AotCodeGuardTest, DisabledCacheReadsRam)
{
  Write(0x1000);
  Warm(0x1000);
  system.GetMemory().Write_U32(0x38600002, 0x1000);
  HID0(system.GetPPCState()).ICE = 0;
  EXPECT_EQ(Match(0x1000), 0);
  words[0] = 0x38600002;
  EXPECT_EQ(Match(0x1000), 1);
  HID0(system.GetPPCState()).ICE = 1;
  system.GetPPCState().iCache.m_disable_icache = true;
  EXPECT_EQ(Match(0x1000), 1);
}

TEST_F(AotCodeGuardTest, Mem2AndBatBoundary)
{
  Write(0x10002000);
  EXPECT_EQ(Match(0x10002000), 1);
  EXPECT_EQ(Match(0x10002000), 1);
  Write(0x1fff0);  // Crosses the boundary: use the full MMU path.
  EXPECT_EQ(Match(0x1fff0), 1);
  EXPECT_EQ(Match(0x1fff0), 1);
}

TEST_F(AotCodeGuardTest, RestoredCacheSelectsRestoredInstructions)
{
  Write(0x1000);
  Warm(0x1000);
  std::array<u8, 65536> saved{};
  u8* cursor = saved.data();
  PointerWrap writer(&cursor, saved.size(), PointerWrap::Mode::Write);
  auto& cache = system.GetPPCState().iCache;
  cache.DoState(system.GetMemory(), writer);
  cache.Invalidate(system.GetMemory(), system.GetJitInterface(), 0x1000);
  words[0] = 0x38600002;
  Write(0x1000);
  Warm(0x1000);
  cursor = saved.data();
  PointerWrap reader(&cursor, saved.size(), PointerWrap::Mode::Read);
  cache.DoState(system.GetMemory(), reader);
  EXPECT_EQ(Match(0x1000), 0);
  words[0] = 0x38600001;
  EXPECT_EQ(Match(0x1000), 1);
}

TEST_F(AotCodeGuardTest, LockedCacheMissDoesNotRememberRam)
{
  Write(0x1000);
  HID0(system.GetPPCState()).ILOCK = 1;
  Warm(0x1000);
  words[0] = 0x38600002;
  Write(0x1000);
  EXPECT_EQ(Match(0x1000), 1);
}

TEST_F(AotCodeGuardTest, ReusePreservesReplacementPolicy)
{
  auto& cache = system.GetPPCState().iCache;
  for (u32 way = 0; way < PowerPC::CACHE_WAYS; ++way)
  {
    const u32 pc = 0x101c + way * 4096;
    Write(pc);
    Warm(pc);  // Three lines in successively populated ways.
    for (u32 initial = 0; initial < 128; ++initial)
    {
      cache.plru.fill(initial);
      for (u32 i = 0; i < words.size(); ++i)
        cache.ReadInstruction(system.GetMemory(), system.GetPPCState(), pc + 4 * i);
      const auto expected = cache.plru;
      cache.plru.fill(initial);
      EXPECT_EQ(Match(pc), 1);
      EXPECT_EQ(cache.plru, expected);
    }
  }
}

TEST_F(AotCodeGuardTest, CacheWriteAndResetInvalidateReuse)
{
  Write(0x1000);
  Warm(0x1000);
  auto& cache = system.GetPPCState().iCache;
  const u32 replacement = Common::swap32(0x38600002u);
  cache.Write(system.GetMemory(), 0x1000, &replacement, sizeof(replacement), true);
  EXPECT_EQ(Match(0x1000), 0);
  static_cast<PowerPC::Cache&>(cache).Reset();
  Warm(0x1000);  // Original RAM contents, independent of the modified cache line.
  system.GetMemory().Write_U32(0x38600002, 0x1000);
  static_cast<PowerPC::Cache&>(cache).Reset();
  EXPECT_EQ(Match(0x1000), 0);
}

TEST_F(AotCodeGuardTest, ExpectedVariantsHaveIndependentResults)
{
  Write(0x1000);
  Warm(0x1000);
  auto other = words;
  other.back() = 0x38600002;
  EXPECT_EQ(aot_match_code(0x1000, other.data(), other.size()), 0);
  EXPECT_EQ(Match(0x1000), 1);
  EXPECT_EQ(aot_match_code(0x1000, other.data(), 1), 1);
  EXPECT_EQ(aot_match_code(0x1000, other.data(), other.size()), 0);
}

TEST_F(AotCodeGuardTest, LargeBlocksRemainFullyChecked)
{
  std::array<u32, 64> large;
  large.fill(0x60000000);
  for (u32 i = 0; i < large.size(); ++i)
    system.GetMemory().Write_U32(large[i], 0x101c + 4 * i);
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(aot_match_code(0x101c, large.data(), large.size()), 1);
  const u32 last = 0x101c + 4 * (large.size() - 1);
  system.GetMemory().Write_U32(0x38600002, last);
  system.GetPPCState().iCache.Invalidate(system.GetMemory(), system.GetJitInterface(), last);
  EXPECT_EQ(aot_match_code(0x101c, large.data(), large.size()), 0);
}

// Opt-in microbenchmark: no timing threshold on heterogeneous CI machines.
// Run with --gtest_also_run_disabled_tests --gtest_filter=*CachedGuardThroughput.
TEST_F(AotCodeGuardTest, DISABLED_CachedGuardThroughput)
{
  Write(0x101c);
  system.GetPPCState().msr.IR = 1;
  system.GetMMU().GetIBATTable()[0x8000101c >> PowerPC::BAT_INDEX_SHIFT] = PowerPC::BAT_MAPPED_BIT;
  Warm(0x8000101c);
  constexpr u32 iterations = 20000000;
  u32 matches = 0;
  const auto start = std::chrono::steady_clock::now();
  for (u32 i = 0; i < iterations; ++i)
    matches += Match(0x8000101c);
  const double ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
      iterations;
  EXPECT_EQ(matches, iterations);
  std::printf("Cached three-line BAT guard: %.2f ns/check (%u checks)\n", ns, iterations);
}

TEST_F(AotCodeGuardTest, RejectsInvalidBounds)
{
  EXPECT_EQ(aot_match_code(0x1000, nullptr, 1), 0);
  EXPECT_EQ(aot_match_code(0x1000, words.data(), 0), 0);
  EXPECT_EQ(aot_match_code(0x1001, words.data(), 1), 0);
  EXPECT_EQ(aot_match_code(0xfffffffc, words.data(), 2), 0);
}
#endif
