// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}
namespace WiimoteEmu
{
struct DesiredWiimoteState;
}
struct GCPadStatus;

namespace PrimeHack
{
#ifdef ENABLE_PRIMEHACK
// Called on the CPU thread at the normal frame-patch safe point.
void Update(const Core::CPUThreadGuard& guard);
void Reset();
// Image-specific CPU runtime; the caller supplies the sampled controller state.
void UpdatePrime1(const Core::CPUThreadGuard& guard, const GCPadStatus& pad, bool enabled);
// Called by Wiimote 1 before its input is recorded/serialized.
void PrepareInput(WiimoteEmu::DesiredWiimoteState* state, bool sensor_bar);
// Poll fresh pad input while Bluetooth is disconnected (PrepareInput is not called then).
u16 GetCurrentlyPressedButtons();
// Pure mapping shared by the frontend path and tests. Pointer axes are [-1, 1].
void MapPad(const GCPadStatus& pad, WiimoteEmu::DesiredWiimoteState* state, float pointer_x,
            float pointer_y, bool sensor_bar);
#else
inline void Update(const Core::CPUThreadGuard&) {}
inline void Reset() {}
inline void PrepareInput(WiimoteEmu::DesiredWiimoteState*, bool) {}
inline u16 GetCurrentlyPressedButtons() { return 0; }
#endif
} // namespace PrimeHack
