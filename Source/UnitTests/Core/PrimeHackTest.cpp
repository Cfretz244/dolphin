// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef ENABLE_PRIMEHACK
#include <array>
#include <bit>
#include <gtest/gtest.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PrimeHack/Pointer.h"
#include "Core/PrimeHack/PrimeHack.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"
#include "UICommon/UICommon.h"

#ifdef DOLPHIN_HAS_AOT
extern "C" void aot_init_fast_mem();
extern "C" void aot_shutdown();
extern "C" int aot_match_code(u32, const u32*, u32);
#endif

TEST(PrimeHackInput, ButtonsStickAndExtension)
{
  GCPadStatus pad;
  pad.button = PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y | PAD_TRIGGER_L;
  pad.stickX = 228;
  pad.stickY = 28;
  WiimoteEmu::DesiredWiimoteState state;
  PrimeHack::MapPad(pad, &state, 0, 0, true);
  using W = WiimoteEmu::Wiimote;
  using N = WiimoteEmu::Nunchuk;
  EXPECT_EQ(state.buttons.hex, W::BUTTON_A | W::BUTTON_B | W::PAD_DOWN);
  ASSERT_TRUE(std::holds_alternative<N::DataFormat>(state.extension.data));
  const auto& nc = std::get<N::DataFormat>(state.extension.data);
  EXPECT_EQ(nc.jx, 228);
  EXPECT_EQ(nc.jy, 28);
  EXPECT_EQ(nc.GetButtons(), N::BUTTON_C | N::BUTTON_Z);
  EXPECT_EQ(nc.GetAccelZ(), N::ACCEL_ONE_G << 2);
  EXPECT_FALSE(state.motion_plus.has_value());
  EXPECT_NE(state.camera_points[0].position.x, 0xffff);
}

TEST(PrimeHackInput, DisconnectReleasesAndSensorBarOff)
{
  GCPadStatus pad;
  pad.button = 0xffff;
  pad.stickX = 255;
  pad.triggerLeft = pad.triggerRight = 255;
  pad.isConnected = false;
  WiimoteEmu::DesiredWiimoteState state;
  PrimeHack::MapPad(pad, &state, 1, -1, false);
  EXPECT_EQ(state.buttons.hex, 0);
  const auto& nc = std::get<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data);
  EXPECT_EQ(nc.jx, 128);
  EXPECT_EQ(nc.jy, 128);
  EXPECT_EQ(nc.GetButtons(), 0);
  EXPECT_EQ(state.camera_points, WiimoteEmu::DesiredWiimoteState::DEFAULT_CAMERA);
}

class PrimeHackRuntime : public testing::Test
{
protected:
  Core::System& system = Core::System::GetInstance();
  PowerPC::BatTable ibats{}, dbats{};
  u32 msr = 0, hid0 = 0;
  bool wii = false;
  constexpr static u32 player = 0x80100000;
  constexpr static std::array<std::pair<u32, u32>, 8> originals{{{0x80098ee4, 0xec000072},
                                                                 {0x80099138, 0x4bffe6dd},
                                                                 {0x80183a8c, 0xd03f03dc},
                                                                 {0x80183a64, 0xd03f03dc},
                                                                 {0x8017661c, 0x901f0118},
                                                                 {0x802fb5b4, 0xd03f009c},
                                                                 {0x8019fbcc, 0x4bea3ca9},
                                                                 {0x8018b8d4, 0x41820014}}};
  void Put(u32 address, u32 value) { system.GetMemory().Write_U32(value, address & 0x1fffffff); }
  u32 Get(u32 address) { return system.GetMemory().Read_U32(address & 0x1fffffff); }
  void Float(u32 address, float value) { Put(address, std::bit_cast<u32>(value)); }
  void SetUp() override
  {
    wii = system.IsWii();
    system.SetIsWii(true);
    system.GetMemory().Init();
    Core::DeclareAsCPUThread();
    auto& state = system.GetPPCState();
    msr = state.msr.Hex;
    hid0 = HID0(state).Hex;
    state.msr.Hex = 0;
    state.msr.DR = state.msr.IR = 1;
    HID0(state).ICE = 1;
    static_cast<PowerPC::Cache&>(state.iCache).Init(system.GetMemory());
    ibats = system.GetMMU().GetIBATTable();
    dbats = system.GetMMU().GetDBATTable();
    for (u32 address = 0x80000000; address < 0x81800000; address += 1 << PowerPC::BAT_INDEX_SHIFT)
    {
      const u32 index = address >> PowerPC::BAT_INDEX_SHIFT;
      const u32 entry = (address & 0x1fffffff) | PowerPC::BAT_MAPPED_BIT;
      system.GetMMU().GetIBATTable()[index] = entry;
      system.GetMMU().GetDBATTable()[index] = entry;
    }
    for (auto [address, word] : originals)
      Put(address, word);
    Put(0x8046d340, 0x4e800020);
    PrimeHack::Reset();
  }
  void TearDown() override
  {
    PrimeHack::Reset();
    system.GetPPCState().msr.Hex = msr;
    HID0(system.GetPPCState()).Hex = hid0;
    system.GetMMU().GetIBATTable() = ibats;
    system.GetMMU().GetDBATTable() = dbats;
    static_cast<PowerPC::Cache&>(system.GetPPCState().iCache).Reset();
    Core::UndeclareAsCPUThread();
    system.GetMemory().Shutdown();
    system.SetIsWii(wii);
  }
  void Update(bool enabled = true, const GCPadStatus& pad = {})
  {
    Core::CPUThreadGuard guard(system);
    PrimeHack::UpdatePrime1(guard, pad, enabled);
  }
  void Player()
  {
    Put(0x804bf420 + 0x84c, player);
    Float(player + 0x2c, 1);
    Float(player + 0x40, 1);
    Float(player + 0x54, 1);
    Float(player + 0x38, 123);
  }
};

TEST_F(PrimeHackRuntime, ReconnectPollReadsFreshPadWithoutReports)
{
  const auto profile = File::CreateTempDir();
  ASSERT_FALSE(profile.empty());
  UICommon::SetUserDirectory(profile);
  Config::Init();
  SConfig::Init();
  system.SetIsWii(true);
  Pad::GetConfig()->CreateController<GCPad>(0);
  const Common::ScopeGuard cleanup(
      [&]
      {
        PrimeHack::Reset();
        Pad::ClearExternalProvider();
        Pad::GetConfig()->ClearControllers();
        SConfig::Shutdown();
        Config::Shutdown();
        File::DeleteDirRecursively(profile);
      });
  GCPadStatus pad;
  int polls = 0;
  Pad::SetExternalProvider(
      [&](int index)
      {
        EXPECT_EQ(index, 0);
        ++polls;
        return pad;
      });
  SConfig::GetInstance().SetRunningGameMetadata("R3ME01");
  Config::SetCurrent(Config::MAIN_PRIMEHACK_ENABLED, true);
  Core::CPUThreadGuard guard(system);
  PrimeHack::Update(guard);
  WiimoteEmu::Wiimote remote(0);
  WiimoteEmu::Wiimote other_remote(1);
  pad.button = PAD_BUTTON_A;
  pad.isConnected = false;
  EXPECT_EQ(remote.GetCurrentlyPressedButtons().hex, 0);
  pad.isConnected = true;
  EXPECT_EQ(remote.GetCurrentlyPressedButtons().hex, WiimoteEmu::Wiimote::BUTTON_A);
  EXPECT_EQ(other_remote.GetCurrentlyPressedButtons().hex, 0);
  pad.button = 0;
  EXPECT_EQ(remote.GetCurrentlyPressedButtons().hex, 0);
  EXPECT_EQ(polls, 3);
  PrimeHack::Reset();
  pad.button = PAD_BUTTON_A;
  EXPECT_EQ(remote.GetCurrentlyPressedButtons().hex, 0);
  EXPECT_EQ(polls, 3);
}

TEST_F(PrimeHackRuntime, PatchesInvalidateInstructionCacheAndRestore)
{
  auto& cache = system.GetPPCState().iCache;
  EXPECT_EQ(cache.ReadInstruction(system.GetMemory(), system.GetPPCState(), 0x98ee4), 0xec000072);
#ifdef DOLPHIN_HAS_AOT
  aot_init_fast_mem();
  const u32 original = 0xec000072;
  const u32 patched = 0xec010072;
  EXPECT_EQ(aot_match_code(0x80098ee4, &original, 1), 1);
  EXPECT_EQ(aot_match_code(0x80098ee4, &original, 1), 1);
#endif
  Update();
  EXPECT_EQ(Get(0x80098ee4), 0xec010072);
#ifdef DOLPHIN_HAS_AOT
  EXPECT_EQ(aot_match_code(0x80098ee4, &original, 1), 0);
  EXPECT_EQ(aot_match_code(0x80098ee4, &patched, 1), 1);
#endif
  EXPECT_EQ(cache.ReadInstruction(system.GetMemory(), system.GetPPCState(), 0x98ee4), 0xec010072);
  Update(false);
  for (auto [address, word] : originals)
    EXPECT_EQ(Get(address), word);
  EXPECT_EQ(cache.ReadInstruction(system.GetMemory(), system.GetPPCState(), 0x98ee4), 0xec000072);
#ifdef DOLPHIN_HAS_AOT
  EXPECT_EQ(aot_match_code(0x80098ee4, &original, 1), 1);
  aot_shutdown();
#endif
}

TEST_F(PrimeHackRuntime, RejectsWrongImageAndPartialPatchSet)
{
  Put(0x8046d340, 0x38000018); // Launcher, at overlapping addresses.
  Update();
  EXPECT_EQ(Get(0x80098ee4), 0xec000072);
  Put(0x8046d340, 0x4e800020);
  Put(0x8019fbcc, 0xdeadbeef);
  Update();
  EXPECT_EQ(Get(0x80098ee4), 0xec000072);
  EXPECT_EQ(Get(0x8019fbcc), 0xdeadbeef);
}

TEST_F(PrimeHackRuntime, CameraMovesWithoutChangingPosition)
{
  Player();
  GCPadStatus pad;
  pad.substickX = pad.substickY = 228;
  Update(true, pad);
  EXPECT_GT(std::bit_cast<float>(Get(player + 0x3dc)), 0);
  EXPECT_NE(Get(player + 0x30), 0u);
  EXPECT_FLOAT_EQ(std::bit_cast<float>(Get(player + 0x38)), 123);
}

TEST_F(PrimeHackRuntime, PauseAndMorphKeepCamera)
{
  Player();
  GCPadStatus pad;
  pad.substickY = 228;
  Put(player + 0x2f0, 1);
  Update(true, pad);
  EXPECT_EQ(Get(player + 0x3dc), 0u);
  Put(player + 0x2f0, 0);
  Put(0x804bf420 + 0x117c, 1);
  Update(true, pad);
  EXPECT_EQ(Get(player + 0x3dc), 0u);
}

TEST_F(PrimeHackRuntime, LockOnRestoresGunMovement)
{
  Player();
  Update();
  EXPECT_EQ(Get(0x8018b8d4), 0x48000354);
  system.GetMemory().Write_U8(1, (0x804bf420 + 0xc93) & 0x1fffffff);
  Update();
  EXPECT_EQ(Get(0x8018b8d4), 0x41820014);
}

TEST(PrimeHackInput, CameraNeverAccumulatesIRDrift)
{
  PrimeHack::PointerState pointer;
  GCPadStatus pad;
  pad.substickX = 228;
  for (int i = 0; i < 1000; ++i)
    pointer.Update(pad, PrimeHack::PointerMode::FreeLook);
  EXPECT_EQ(pointer.x, 0);
  EXPECT_EQ(pointer.y, 0);
  pad.substickX = 128;
  pointer.Update(pad, PrimeHack::PointerMode::Wheel);
  EXPECT_EQ(pointer.x, 0);
}

TEST(PrimeHackInput, WheelDirectionIsImmediateAndHeldUntilClose)
{
  PrimeHack::PointerState pointer;
  GCPadStatus pad;
  for (auto [x, y] :
       std::array<std::pair<int, int>, 4>{{{228, 228}, {28, 228}, {28, 28}, {228, 28}}})
  {
    pad.substickX = x;
    pad.substickY = y;
    pointer.Update(pad, PrimeHack::PointerMode::Wheel);
    EXPECT_NEAR(pointer.x, x > 128 ? 0.85f : -0.85f, 0.001f);
    EXPECT_NEAR(pointer.y, y > 128 ? 0.85f : -0.85f, 0.001f);
  }
  const float last_x = pointer.x, last_y = pointer.y;
  pad.substickX = pad.substickY = 100; // Weak opposite input during release.
  pointer.Update(pad, PrimeHack::PointerMode::Wheel);
  EXPECT_EQ(pointer.x, last_x);
  EXPECT_EQ(pointer.y, last_y);
  pad.substickX = pad.substickY = 128;
  pointer.Update(pad, PrimeHack::PointerMode::Wheel);
  EXPECT_EQ(pointer.x, last_x);
  EXPECT_EQ(pointer.y, last_y);
  pointer.Update(pad, PrimeHack::PointerMode::FreeLook);
  EXPECT_EQ(pointer.x, 0);
  EXPECT_EQ(pointer.y, 0);
}

TEST_F(PrimeHackRuntime, WheelReleasesHorizontalReticleAndOwnsStick)
{
  Player();
  GCPadStatus pad;
  pad.substickX = pad.substickY = 228;
  pad.button = PAD_TRIGGER_Z; // Minus, even before the game's wheel flag updates.
  Update(true, pad);
  EXPECT_EQ(Get(0x802fb5b4), 0xd03f009c);
  EXPECT_EQ(Get(0x8019fbcc), 0x4bea3ca9);
  EXPECT_EQ(Get(player + 0x3dc), 0u);
  EXPECT_EQ(Get(player + 0x30), 0u);
  // The game can keep the wheel open after the physical button is released.
  Put(0x805c28b0, 0x80200000);
  Put(0x8020032c, 1);
  pad.button = 0;
  Update(true, pad);
  EXPECT_EQ(Get(0x802fb5b4), 0xd03f009c);
  EXPECT_EQ(Get(0x8019fbcc), 0x4bea3ca9);
  EXPECT_EQ(Get(player + 0x3dc), 0u);
  Put(0x8020032c, 0);
  Update(true, pad);
  EXPECT_EQ(Get(0x802fb5b4), 0xd23f009c);
  EXPECT_GT(std::bit_cast<float>(Get(player + 0x3dc)), 0);
}
#endif
