// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Common/CommonTypes.h"
#include <array>

namespace PrimeHack
{
struct Patch
{
  u32 address, original, replacement;
};

// Shared by the runtime and offline AOT image builder. Original words must all
// match before this patch set is applied to NTSC-U Trilogy MP1 revision 0.
inline constexpr std::array<Patch, 8> MP1_PATCHES{{
    {0x80098ee4, 0xec000072, 0xec010072}, // Pitch interpolation.
    {0x80099138, 0x4bffe6dd, 0x60000000}, // Floor-driven pitch.
    {0x80183a8c, 0xd03f03dc, 0x60000000},
    {0x80183a64, 0xd03f03dc, 0x60000000},
    {0x8017661c, 0x901f0118, 0x60000000},
    {0x802fb5b4, 0xd03f009c, 0xd23f009c}, // Horizontal reticle store.
    {0x8019fbcc, 0x4bea3ca9, 0x60000000},
    {0x8018b8d4, 0x41820014, 0x48000354}, // Arm-cannon movement.
}};

constexpr u32 MP1PatchMask(bool enabled, bool wheel, bool paused, bool locked)
{
  return enabled ? 0x1f | (!wheel && !paused ? 0x60 : 0) | (!locked ? 0x80 : 0) : 0;
}

// Original instructions remain a separate image. These cover all runtime modes.
inline constexpr std::array<u32, 4> MP1_PATCH_MASKS{0x1f, 0x7f, 0x9f, 0xff};
} // namespace PrimeHack
