// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// NTSC-U Trilogy MP1 FPS controls adapted from shiiion/dolphin, revision
// 53f53e0f5bad27ad62a807cb93b136d84f68777f. See README.md for scope/provenance.

#include "Core/PrimeHack/PrimeHack.h"
#include "Core/PrimeHack/Pointer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"

namespace PrimeHack
{
namespace
{
struct Patch
{
  u32 address, original, replacement;
};
constexpr std::array<Patch, 8> PATCHES{{
    {0x80098ee4, 0xec000072, 0xec010072}, // Pitch interpolation.
    {0x80099138, 0x4bffe6dd, 0x60000000}, // Floor-driven pitch.
    {0x80183a8c, 0xd03f03dc, 0x60000000},
    {0x80183a64, 0xd03f03dc, 0x60000000},
    {0x8017661c, 0x901f0118, 0x60000000},
    {0x802fb5b4, 0xd03f009c, 0xd23f009c}, // Reticle horizontal store.
    {0x8019fbcc, 0x4bea3ca9, 0x60000000},
    {0x8018b8d4, 0x41820014, 0x48000354}, // Arm-cannon movement (off during lock-on).
}};
constexpr u32 STATE_MANAGER = 0x804bf420;
std::atomic<bool> s_input_enabled{false};
std::mutex s_input_mutex;
GCPadStatus s_pad;
PointerState s_pointer;
std::atomic<PointerMode> s_pointer_mode{PointerMode::Menu};
bool s_reported_active = false;

bool Ram(u32 address, u32 size = 4)
{
  // MP1's objects are in cached MEM1. Never chase uninitialized/transition pointers.
  return address >= 0x80000000 && address < 0x81800000 && size <= 0x81800000 - address &&
         (address & 3) == 0;
}

template <typename T = u32> T Read(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostRead<T>(guard, address);
}
template <typename T> void Write(const Core::CPUThreadGuard& guard, u32 address, T value)
{
  PowerPC::MMU::HostWrite<T>(guard, value, address);
}

void WriteCode(const Core::CPUThreadGuard& guard, const Patch& patch, bool enabled)
{
  const u32 value = enabled ? patch.replacement : patch.original;
  if (Read(guard, patch.address) != value)
  {
    Write(guard, patch.address, value);
    guard.GetSystem().GetPowerPC().ScheduleInvalidateCacheThreadSafe(patch.address);
  }
}
} // namespace

void MapPad(const GCPadStatus& input, WiimoteEmu::DesiredWiimoteState* state, float pointer_x,
            float pointer_y, bool sensor_bar)
{
  using W = WiimoteEmu::Wiimote;
  using N = WiimoteEmu::Nunchuk;
  GCPadStatus pad = input;
  if (!pad.isConnected)
    pad = GCPadStatus{};
  state->buttons.hex = 0;
  const auto button = [&](bool pressed, u16 mask)
  {
    if (pressed)
      state->buttons.hex |= mask;
  };
  button(pad.button & PAD_BUTTON_A || pad.button & PAD_TRIGGER_R || pad.triggerRight > 127,
         W::BUTTON_A);
  button(pad.button & PAD_BUTTON_B, W::BUTTON_B);
  button(pad.button & PAD_BUTTON_Y || pad.button & PAD_BUTTON_DOWN, W::PAD_DOWN);
  button(pad.button & PAD_BUTTON_UP, W::PAD_UP);
  button(pad.button & PAD_BUTTON_LEFT, W::PAD_LEFT);
  button(pad.button & PAD_BUTTON_RIGHT, W::PAD_RIGHT);
  button(pad.button & PAD_TRIGGER_Z, W::BUTTON_MINUS);
  const bool start = pad.button & PAD_BUTTON_START;
  const bool lock = pad.button & PAD_TRIGGER_L || pad.triggerLeft > 127;
  button(start && !lock, W::BUTTON_PLUS);
  button(start && lock, W::BUTTON_ONE);
  state->acceleration = WiimoteEmu::DesiredWiimoteState::DEFAULT_ACCELERATION;
  state->motion_plus.reset();
  state->camera_points = WiimoteEmu::DesiredWiimoteState::DEFAULT_CAMERA;
  if (sensor_bar)
  {
    const u16 x = static_cast<u16>(512 - std::clamp(pointer_x, -1.f, 1.f) * 360);
    const u16 y = static_cast<u16>(384 - std::clamp(pointer_y, -1.f, 1.f) * 280);
    state->camera_points[0] = {{static_cast<u16>(x - 50), y}, 8};
    state->camera_points[1] = {{static_cast<u16>(x + 50), y}, 8};
  }
  N::DataFormat nc{};
  nc.jx = static_cast<u8>(128 + StickAxis(pad.stickX) * 100);
  nc.jy = static_cast<u8>(128 + StickAxis(pad.stickY) * 100);
  nc.SetButtons((pad.button & PAD_BUTTON_X ? N::BUTTON_C : 0) |
                (pad.button & PAD_TRIGGER_L || pad.triggerLeft > 127 ? N::BUTTON_Z : 0));
  nc.SetAccel({N::ACCEL_ZERO_G << 2, N::ACCEL_ZERO_G << 2, N::ACCEL_ONE_G << 2});
  state->extension.data = nc;
}

void PrepareInput(WiimoteEmu::DesiredWiimoteState* state, bool sensor_bar)
{
  if (!s_input_enabled.load() || !Pad::IsInitialized())
    return;
  const GCPadStatus pad = Pad::GetStatus(0);
  std::lock_guard lock(s_input_mutex);
  s_pad = pad;
  auto mode = s_pointer_mode.load();
  // A wheel request must stop freelook immediately, before the game opens it.
  if (mode != PointerMode::Menu && WheelButton(pad))
    mode = PointerMode::Wheel;
  s_pointer.Update(pad, mode);
  MapPad(pad, state, s_pointer.x, s_pointer.y, sensor_bar);
}

u16 GetCurrentlyPressedButtons()
{
  if (!s_input_enabled.load() || !Pad::IsInitialized())
    return 0;

  // Bluetooth activation bypasses PrepareInput. Sampling its cached pad here would
  // prevent reconnecting, and could keep moving the camera with stale stick input.
  const GCPadStatus pad = Pad::GetStatus(0);
  std::lock_guard lock(s_input_mutex);
  s_pad = {};
  s_pointer = {};
  WiimoteEmu::DesiredWiimoteState state;
  MapPad(pad, &state, 0, 0, false);
  return state.buttons.hex;
}

void Reset()
{
  s_input_enabled = false;
  std::lock_guard lock(s_input_mutex);
  s_pad = {};
  s_pointer = {};
  s_pointer_mode = PointerMode::Menu;
  s_reported_active = false;
}

void Update(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  const bool supported = system.IsWii() && SConfig::GetInstance().GetGameID() == "R3ME01" &&
                         SConfig::GetInstance().GetRevision() == 0;
  const bool enabled = supported && Config::Get(Config::MAIN_PRIMEHACK_ENABLED) &&
                       !NetPlay::IsNetPlayRunning() && !system.GetMovie().IsMovieActive();
  s_input_enabled = enabled;
  if (!supported)
  {
    s_pointer_mode = PointerMode::Menu;
    return;
  }

  GCPadStatus pad;
  {
    std::lock_guard lock(s_input_mutex);
    pad = s_pad;
  }
  UpdatePrime1(guard, pad, enabled);
}

void UpdatePrime1(const Core::CPUThreadGuard& guard, const GCPadStatus& pad, bool enabled)
{
  // Upstream image discriminator plus all expected patch words: refuse partial
  // loads, another executable at the same addresses, and conflicting mods.
  if (Read(guard, 0x8046d340) != 0x4e800020)
  {
    s_reported_active = false;
    s_pointer_mode = PointerMode::Menu;
    return;
  }
  for (const auto& patch : PATCHES)
  {
    const u32 word = Read(guard, patch.address);
    if (word != patch.original && word != patch.replacement)
    {
      s_pointer_mode = PointerMode::Menu;
      return;
    }
  }
  const u32 player = Read(guard, STATE_MANAGER + 0x84c);
  const bool has_player = Ram(player, 0x500);
  const u32 menu_base = Read(guard, 0x805c28b0);
  const bool wheel = Ram(menu_base, 0x338) && Read(guard, menu_base + 0x32c) == 1;
  const bool wheel_input = wheel || WheelButton(pad);
  const bool paused = Read(guard, STATE_MANAGER + 0x117c) != 0;
  if (!enabled || !has_player)
    s_pointer_mode = PointerMode::Menu;
  else
    s_pointer_mode = wheel_input ? PointerMode::Wheel
                     : paused    ? PointerMode::Menu
                                 : PointerMode::FreeLook;
  const bool locked =
      has_player &&
      ((Read(guard, player + 0x300) != 5 && Read<u8>(guard, STATE_MANAGER + 0xc93)) || wheel);
  for (size_t i = 0; i < PATCHES.size(); ++i)
  {
    // Let the game's IR path place the reticle in both wheel axes. The initial
    // port incorrectly forced the horizontal store to zero during selection.
    const bool centered_reticle = (i != 5 && i != 6) || (!wheel_input && !paused);
    const bool gun_move = i != PATCHES.size() - 1 || !locked;
    WriteCode(guard, PATCHES[i], enabled && centered_reticle && gun_move);
  }
  if (!enabled || !has_player)
    return;
  if (!s_reported_active)
  {
    NOTICE_LOG_FMT(CORE,
                   "PrimeHack: NTSC-U Trilogy Prime 1 FPS controls active (8 guarded patches)");
    s_reported_active = true;
  }

  if (wheel_input)
    return; // Right stick belongs exclusively to selection, not camera rotation.

  // Keep pitch aligned with the actual first-person camera during lock-on.
  if (locked)
  {
    const u32 manager = Read(guard, STATE_MANAGER + 0x868);
    const u32 objects = Read(guard, STATE_MANAGER + 0x810);
    if (!Ram(manager) || !Ram(objects, 0x2004))
      return;
    const u16 uid = Read<u16>(guard, manager);
    if (uid == 0xffff)
      return;
    const u32 camera = Read(guard, objects + ((uid & 0x3ff) << 3) + 4);
    if (Ram(camera, 0x5c))
    {
      const float z = Read<float>(guard, camera + 0x2c + 36);
      if (std::isfinite(z))
        Write(guard, player + 0x3dc,
              std::clamp(std::asin(std::clamp(z, -1.f, 1.f)), -1.52f, 1.52f));
    }
    return;
  }
  if (Read(guard, player + 0x2f0) != 0 || Read(guard, STATE_MANAGER + 0x117c) != 0)
    return; // Morph ball/cutscene or pause menu owns the camera.

  if (!pad.isConnected)
    return;
  const float fx = Read<float>(guard, player + 0x30);
  const float fy = Read<float>(guard, player + 0x40);
  const float old_pitch = Read<float>(guard, player + 0x3dc);
  if (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(old_pitch) ||
      fx * fx + fy * fy < 0.0001f)
    return;
  constexpr float step = 2.5f / 60.f; // Radians per emulated NTSC frame.
  const float yaw = std::atan2(fy, fx) - StickAxis(pad.substickX) * step;
  const float pitch = std::clamp(old_pitch + StickAxis(pad.substickY) * step, -1.52f, 1.52f);
  const float rotation = yaw - 1.570796327f;
  const float sy = std::sin(rotation), cy = std::cos(rotation);
  const std::array<float, 9> basis{cy, -sy, 0, sy, cy, 0, 0, 0, 1};
  for (u32 row = 0; row < 3; ++row)
    for (u32 col = 0; col < 3; ++col)
      Write(guard, player + 0x2c + row * 16 + col * 4, basis[row * 3 + col]);
  Write(guard, player + 0x3dc, pitch);
  Write(guard, 0x804ddff8 + 0x134, 1.52f);
  const u32 cursor_root = Read(guard, 0x805c28a8);
  if (Ram(cursor_root, 0xc58))
  {
    const u32 cursor = Read(guard, cursor_root + 0xc54);
    if (Ram(cursor, 0x160))
    {
      Write(guard, cursor + 0x9c, 0u);
      Write(guard, cursor + 0x15c, 0u);
    }
  }
}
} // namespace PrimeHack
