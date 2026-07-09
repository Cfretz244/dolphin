// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/MeleeRollbackState.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

namespace ExpansionInterface
{
namespace
{
constexpr u32 MEM1_BASE = 0x80000000;
constexpr u32 MEM1_END = 0x81800000;

bool ParseHex(const std::string& tok, u32* out)
{
  if (tok.rfind("0x", 0) != 0)
    return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(tok.c_str() + 2, &end, 16);
  if (end == nullptr || *end != '\0')
    return false;
  *out = static_cast<u32>(v);
  return true;
}

bool SaneRange(u32 start, u32 end)
{
  return start >= MEM1_BASE && end <= MEM1_END && start < end;
}
}  // namespace

bool MeleeRollbackState::LoadRegionTable(const std::string& path, std::string* error)
{
  std::ifstream in(path);
  if (!in)
  {
    *error = "cannot open region table: " + path;
    return false;
  }

  m_raw_regions.clear();
  m_excludes.clear();
  m_regions.clear();
  m_watches.clear();
  m_heap_resolved = false;

  std::string line;
  int lineno = 0;
  while (std::getline(in, line))
  {
    lineno++;
    const auto hash = line.find('#');
    if (hash != std::string::npos)
      line.resize(hash);
    std::istringstream ss(line);
    std::string kind, start_tok, end_tok, label;
    if (!(ss >> kind))
      continue;  // blank/comment
    if (!(ss >> start_tok >> end_tok))
    {
      *error = "line " + std::to_string(lineno) + ": expected two addresses";
      return false;
    }
    ss >> label;  // optional

    u32 start = 0, end = 0;
    if (!ParseHex(start_tok, &start) || !ParseHex(end_tok, &end))
    {
      *error = "line " + std::to_string(lineno) + ": bad hex";
      return false;
    }

    if (kind == "region" || kind == "exclude")
    {
      if (!SaneRange(start, end))
      {
        *error = "line " + std::to_string(lineno) + ": range outside MEM1";
        return false;
      }
      (kind == "region" ? m_raw_regions : m_excludes).push_back({start, end, label});
    }
    else if (kind == "heapptr")
    {
      m_heapptr_lo = start;
      m_heapptr_hi = end;
    }
    else if (kind == "watch")
    {
      m_watches.push_back({start, label});
    }
    else
    {
      *error = "line " + std::to_string(lineno) + ": unknown kind '" + kind + "'";
      return false;
    }
  }

  if (m_raw_regions.empty())
  {
    *error = "region table has no regions";
    return false;
  }

  // The static copy plan (heap region joins at first capture).
  FinalizeCopyPlan();
  return true;
}

void MeleeRollbackState::FinalizeCopyPlan()
{
  // Carve every exclusion out of every region: classic interval subtraction
  // over sorted, non-overlapping inputs.
  std::vector<Region> excludes = m_excludes;
  std::sort(excludes.begin(), excludes.end(),
            [](const Region& a, const Region& b) { return a.start < b.start; });

  m_regions.clear();
  for (const Region& reg : m_raw_regions)
  {
    u32 cursor = reg.start;
    for (const Region& ex : excludes)
    {
      if (ex.end <= cursor || ex.start >= reg.end)
        continue;
      if (ex.start > cursor)
        m_regions.push_back({cursor, ex.start, reg.label});
      cursor = std::max(cursor, ex.end);
      if (cursor >= reg.end)
        break;
    }
    if (cursor < reg.end)
      m_regions.push_back({cursor, reg.end, reg.label});
  }

  m_snapshot_bytes = 0;
  for (const Region& r : m_regions)
    m_snapshot_bytes += r.end - r.start;

  // Invalidate the ring: the plan (and thus slot layout) changed.
  for (Slot& slot : m_ring)
    slot.valid = false;

  INFO_LOG_FMT(EXPANSIONINTERFACE,
               "MeleeRollback: copy plan {} spans, {:.2f} MB/snapshot, ring {} ({} MB host)",
               m_regions.size(), m_snapshot_bytes / (1024.0 * 1024.0), RING_SIZE,
               (m_snapshot_bytes * RING_SIZE) >> 20);
}

void MeleeRollbackState::ResolveHeapRegion(Core::System& system)
{
  m_heap_resolved = true;  // one attempt per session either way

  if (m_heapptr_lo == 0 || m_heapptr_hi == 0)
    return;

  auto& memory = system.GetMemory();
  u8 raw[8] = {};
  memory.CopyFromEmu(raw, m_heapptr_lo, 4);
  memory.CopyFromEmu(raw + 4, m_heapptr_hi, 4);
  const u32 heap_start = (u32(raw[0]) << 24) | (u32(raw[1]) << 16) | (u32(raw[2]) << 8) | raw[3];
  const u32 heap_end = (u32(raw[4]) << 24) | (u32(raw[5]) << 16) | (u32(raw[6]) << 8) | raw[7];

  if (!SaneRange(heap_start, heap_end))
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE,
                  "MeleeRollback: heap bounds insane ({:08x}..{:08x}) — heap NOT in snapshot; "
                  "expect R0 divergence if a match is running",
                  heap_start, heap_end);
    return;
  }

  INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeRollback: main heap {:08x}..{:08x} ({:.2f} MB)",
               heap_start, heap_end, (heap_end - heap_start) / (1024.0 * 1024.0));
  m_raw_regions.push_back({heap_start, heap_end, "main-heap"});
  FinalizeCopyPlan();
}

void MeleeRollbackState::Capture(Core::System& system, u32 tick)
{
  if (m_regions.empty())
    return;
  if (!m_heap_resolved)
    ResolveHeapRegion(system);

  const auto t0 = std::chrono::steady_clock::now();

  Slot& slot = m_ring[tick % RING_SIZE];
  slot.data.resize(m_snapshot_bytes);
  auto& memory = system.GetMemory();
  size_t off = 0;
  for (const Region& r : m_regions)
  {
    memory.CopyFromEmu(slot.data.data() + off, r.start, r.end - r.start);
    off += r.end - r.start;
  }
  slot.tick = tick;
  slot.scene = m_watches.empty() ? 0 : ReadWatch(system, m_watches.front());
  slot.valid = true;

  m_total_captures++;
  m_last_capture_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count());
}

bool MeleeRollbackState::Restore(Core::System& system, u32 tick)
{
  const Slot& slot = m_ring[tick % RING_SIZE];
  if (!slot.valid || slot.tick != tick || slot.data.size() != m_snapshot_bytes)
    return false;

  auto& memory = system.GetMemory();
  size_t off = 0;
  for (const Region& r : m_regions)
  {
    memory.CopyToEmu(r.start, slot.data.data() + off, r.end - r.start);
    off += r.end - r.start;
  }
  return true;
}

bool MeleeRollbackState::SameScene(Core::System& system, u32 tick) const
{
  if (m_watches.empty())
    return true;  // no watch configured: caller accepts the straddle risk
  const Slot& slot = m_ring[tick % RING_SIZE];
  if (!slot.valid || slot.tick != tick)
    return false;
  return slot.scene == ReadWatch(system, m_watches.front());
}

s64 MeleeRollbackState::OldestTick() const
{
  s64 oldest = -1;
  for (const Slot& slot : m_ring)
  {
    if (slot.valid && (oldest < 0 || slot.tick < static_cast<u32>(oldest)))
      oldest = slot.tick;
  }
  return oldest;
}

u32 MeleeRollbackState::ReadWatch(Core::System& system, const Watch& watch) const
{
  auto& memory = system.GetMemory();
  u8 raw[4] = {};
  memory.CopyFromEmu(raw, watch.addr, 4);
  return (u32(raw[0]) << 24) | (u32(raw[1]) << 16) | (u32(raw[2]) << 8) | raw[3];
}

u32 MeleeRollbackState::LiveChecksum(Core::System& system) const
{
  auto& memory = system.GetMemory();
  u32 hash = 0x811C9DC5;
  std::vector<u8> buf;
  for (const Region& r : m_regions)
  {
    buf.resize(r.end - r.start);
    memory.CopyFromEmu(buf.data(), r.start, r.end - r.start);
    for (const u8 b : buf)
      hash = (hash ^ b) * 0x01000193;
  }
  return hash;
}
}  // namespace ExpansionInterface
