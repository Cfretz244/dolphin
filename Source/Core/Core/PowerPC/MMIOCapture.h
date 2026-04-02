// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Global MMIO write capture for AOT diff harness.
// When enabled, records all writes to the hardware register range (0x0C000000+)
// so they can be compared between AOT and interpreter execution.

#pragma once

#include <cstdint>
#include <vector>

struct MMIOWrite
{
  uint32_t address;  // Physical address
  uint32_t value;
  uint32_t size;
};

// Global capture state — controlled by the diff harness
inline bool g_mmio_capture_active = false;
inline std::vector<MMIOWrite> g_mmio_capture_log;

inline void MMIOCaptureReset()
{
  g_mmio_capture_log.clear();
}

inline void MMIOCaptureRecord(uint32_t addr, uint32_t value, uint32_t size)
{
  if (g_mmio_capture_active)
    g_mmio_capture_log.push_back({addr, value, size});
}
