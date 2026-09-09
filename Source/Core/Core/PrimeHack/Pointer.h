// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "InputCommon/GCPadStatus.h"
#include <algorithm>
#include <cmath>

namespace PrimeHack
{
enum class PointerMode
{
  Menu,
  FreeLook,
  Wheel
};

inline float StickAxis(u8 value)
{
  const float v = std::clamp((int(value) - 128) / 100.f, -1.f, 1.f);
  constexpr float deadzone = 0.15f;
  return std::abs(v) <= deadzone ? 0.f
                                 : std::copysign((std::abs(v) - deadzone) / (1 - deadzone), v);
}

inline bool WheelButton(const GCPadStatus& pad)
{
  const bool lock = pad.button & PAD_TRIGGER_L || pad.triggerLeft > 127;
  return pad.isConnected &&
         ((pad.button & PAD_TRIGGER_Z) || ((pad.button & PAD_BUTTON_START) && !lock));
}

struct PointerState
{
  float x = 0, y = 0;
  PointerMode previous = PointerMode::Menu;

  // Called at the emulated Wiimote's 200 Hz. Wheel coordinates are absolute,
  // held until the wheel closes, so releasing the stick/button can commit them.
  void Update(const GCPadStatus& pad, PointerMode mode)
  {
    if (mode != previous || !pad.isConnected)
      x = y = 0;
    previous = mode;
    if (!pad.isConnected)
      return;
    const float sx = StickAxis(pad.substickX), sy = StickAxis(pad.substickY);
    if (mode == PointerMode::FreeLook)
      x = y = 0;
    else if (mode == PointerMode::Wheel)
    {
      // Do not change selection as a released stick springs through center.
      if (std::hypot(sx, sy) >= 0.5f)
      {
        // Use direction rather than deflection magnitude to reach the quadrant.
        const float scale = 0.85f / std::max(std::abs(sx), std::abs(sy));
        x = sx * scale;
        y = sy * scale;
      }
    }
    else
    {
      x = std::clamp(x + sx * 1.5f / 200, -1.f, 1.f);
      y = std::clamp(y + sy * 1.5f / 200, -1.f, 1.f);
    }
  }
};
} // namespace PrimeHack
