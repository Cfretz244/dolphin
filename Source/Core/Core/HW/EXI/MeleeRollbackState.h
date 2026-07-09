// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Host-side MEM1 snapshot ring for Melee rollback netplay (P4).
//
// Snapshots are captured at the lockstep exchange point: the game is parked
// inside a blocking EXI transaction, so the emulated CPU sits at a
// deterministic PC at a deterministic point of the frame loop and the
// captured regions are quiescent. Region set is data-driven — generated per
// DOL build from the linker map by scripts/gen-rollback-regions.py (the DOL
// is a shifted non-matching build; addresses move every rebuild).
//
// Design notes in aot-dolphin-helper/research/rollback-p4-plan.md.

#pragma once

#include <array>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace ExpansionInterface
{
class MeleeRollbackState
{
public:
  // Ring depth bounds the rollback window; 16 ticks ≈ 267 ms at 60 Hz,
  // comfortably above any playable prediction window.
  static constexpr size_t RING_SIZE = 16;

  // Parses the region table (region/exclude/heapptr lines). Returns false
  // with *error set on malformed input. Exclusions are carved out of the
  // regions at load time, yielding a flat copy plan.
  bool LoadRegionTable(const std::string& path, std::string* error);

  bool IsLoaded() const { return !m_regions.empty(); }
  size_t SnapshotBytes() const { return m_snapshot_bytes; }
  size_t RegionCount() const { return m_regions.size(); }

  // Captures the region set as the pre-state of `tick`. First call resolves
  // the runtime heap bounds (heapptr) and finalizes the copy plan.
  void Capture(Core::System& system, u32 tick);

  // Restores the ring entry captured for `tick` (pre-tick state).
  // Returns false if that tick is no longer (or not yet) in the ring.
  bool Restore(Core::System& system, u32 tick);

  // Oldest restorable tick given the current ring contents, or -1 if none.
  s64 OldestTick() const;

  // FNV-1a over the LIVE contents of every region. Both peers run identical
  // region tables (same generated file for the same DOL), so cross-peer
  // equality is meaningful. Host-speed; cheap enough for every-60-ticks.
  u32 LiveChecksum(Core::System& system) const;

  // Capture-time statistics for perf logging.
  u64 TotalCaptures() const { return m_total_captures; }
  u64 LastCaptureMicros() const { return m_last_capture_us; }

private:
  struct Region
  {
    u32 start = 0;
    u32 end = 0;
    std::string label;
  };

  struct Slot
  {
    bool valid = false;
    u32 tick = 0;
    std::vector<u8> data;  // concatenated regions, m_snapshot_bytes long
  };

  void ResolveHeapRegion(Core::System& system);
  void FinalizeCopyPlan();

  std::vector<Region> m_raw_regions;   // as parsed, pre-exclusion
  std::vector<Region> m_excludes;      // as parsed
  std::vector<Region> m_regions;       // carved, final copy plan
  u32 m_heapptr_lo = 0;                // emu addr holding heap start (u32be)
  u32 m_heapptr_hi = 0;                // emu addr holding heap end (u32be)
  bool m_heap_resolved = false;
  size_t m_snapshot_bytes = 0;

  std::array<Slot, RING_SIZE> m_ring;
  u64 m_total_captures = 0;
  u64 m_last_capture_us = 0;
};
}  // namespace ExpansionInterface
