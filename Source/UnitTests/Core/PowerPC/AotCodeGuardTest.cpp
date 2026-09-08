// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef DOLPHIN_HAS_AOT
#include <array>

#include <gtest/gtest.h>

#include "Common/ChunkFile.h"
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
};

TEST_F(AotCodeGuardTest, WritesRemainInvisibleUntilIcbi)
{
  Write(0x101c);  // Three cache lines, including a partial first line.
  ASSERT_EQ(Match(0x101c), 1);  // Fill via MMU.
  ASSERT_EQ(Match(0x101c), 1);  // Resident fast path.
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
  ASSERT_EQ(Match(0x80001000), 1);
  bat = 0x20000 | PowerPC::BAT_MAPPED_BIT;
  EXPECT_EQ(Match(0x80001000), 0);
  words[0] = 0x38600002;
  EXPECT_EQ(Match(0x80001000), 1);
}

TEST_F(AotCodeGuardTest, EvictionExposesModifiedRam)
{
  Write(0x1000);
  ASSERT_EQ(Match(0x1000), 1);
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
  ASSERT_EQ(Match(0x1000), 1);
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
  ASSERT_EQ(Match(0x1000), 1);
  std::array<u8, 65536> saved{};
  u8* cursor = saved.data();
  PointerWrap writer(&cursor, saved.size(), PointerWrap::Mode::Write);
  auto& cache = system.GetPPCState().iCache;
  cache.DoState(system.GetMemory(), writer);
  cache.Invalidate(system.GetMemory(), system.GetJitInterface(), 0x1000);
  words[0] = 0x38600002;
  Write(0x1000);
  ASSERT_EQ(Match(0x1000), 1);
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
  ASSERT_EQ(Match(0x1000), 1);
  words[0] = 0x38600002;
  Write(0x1000);
  EXPECT_EQ(Match(0x1000), 1);
}

TEST_F(AotCodeGuardTest, RejectsInvalidBounds)
{
  EXPECT_EQ(aot_match_code(0x1000, nullptr, 1), 0);
  EXPECT_EQ(aot_match_code(0x1000, words.data(), 0), 0);
  EXPECT_EQ(aot_match_code(0x1001, words.data(), 1), 0);
  EXPECT_EQ(aot_match_code(0xfffffffc, words.data(), 2), 0);
}
#endif
