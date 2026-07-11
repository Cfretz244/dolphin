// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/MeleeRollbackState.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
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

// Payload-journal registration target (see NotifyPayloadWrite). Delivery
// sites and Capture/Restore all run on the CPU thread — no locking.
MeleeRollbackState* s_active_rollback_state = nullptr;
}  // namespace

MeleeRollbackState::~MeleeRollbackState()
{
  if (s_active_rollback_state == this)
    s_active_rollback_state = nullptr;
}

void MeleeRollbackState::NotifyPayloadWrite(Core::System& system, u32 addr, u32 len,
                                            bool from_dvd)
{
  if (s_active_rollback_state != nullptr)
    s_active_rollback_state->NotePayloadWrite(system, addr, len, from_dvd);
}

void MeleeRollbackState::NotePayloadWrite(Core::System& system, u32 addr, u32 len, bool from_dvd)
{
  // DVD payloads only. ARAM->MRAM downloads (fighter animation streaming)
  // are TIME-EVOLVED sim-read state: at the restore target tick the buffer
  // legitimately held the PREVIOUS contents, and the replay must re-read
  // those — stomping the future payload over the restored past corrupts the
  // rolling peer only (v17d q1: 1317 desyncs, every rollback crossing an
  // anim load). The game's own replay re-requests ARAM loads it needs;
  // thousands of clean rollbacks ran with no ARAM re-delivery at all.
  if (!from_dvd)
    return;
  if (m_regions.empty() || !m_heap_resolved || len == 0)
    return;
  // Physical (0x00xxxxxx) delivery addresses alias the cached mirror the
  // region table uses.
  if (addr < MEM1_BASE)
    addr |= MEM1_BASE;
  const u32 addr_end = addr + len;
  auto& memory = system.GetMemory();
  // Journal only the spans overlapping RESTORED regions: excluded regions
  // keep live bytes that may already have advanced past this delivery.
  for (const Region& r : m_regions)
  {
    const u32 lo = std::max(addr, r.start);
    const u32 hi = std::min(addr_end, r.end);
    if (lo >= hi)
      continue;
    Delivery d;
    d.seq = ++m_delivery_seq;
    d.addr = lo;
    d.bytes.resize(hi - lo);
    memory.CopyFromEmu(d.bytes.data(), lo, hi - lo);
    m_delivery_bytes += d.bytes.size();
    m_deliveries.push_back(std::move(d));
    if (from_dvd)
      m_dvd_delivery_seq++;
  }
  while (m_delivery_bytes > DELIVERY_BYTES_CAP && !m_deliveries.empty())
  {
    m_delivery_bytes -= m_deliveries.front().bytes.size();
    m_deliveries.pop_front();
  }
}

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
  m_hash_spans.clear();
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

    if (kind == "region" || kind == "exclude" || kind == "hash")
    {
      if (!SaneRange(start, end))
      {
        *error = "line " + std::to_string(lineno) + ": range outside MEM1";
        return false;
      }
      if (kind == "hash")
        m_hash_spans.push_back({start, end, label});
      else
        (kind == "region" ? m_raw_regions : m_excludes).push_back({start, end, label});
    }
    else if (kind == "heapptr")
    {
      m_heapptr_lo = start;
      m_heapptr_hi = end;
    }
    else if (kind == "heapbase")
    {
      // Fixed LOWER bound for the heap region (static image end, build-time).
      // The runtime hsd_heap_next_arena_lo read happens at first capture —
      // AFTER boot-era arenas were carved — so every ObjAlloc pool page and
      // OS heap header allocated during engine init sits BELOW that
      // watermark. HSD_ObjAlloc conditions success on OSCheckHeap() (live
      // heap headers) and pool free lists thread through those pages:
      // restoring the pool heads (static bss) while the pages stay live is
      // the rumble mixed-structure bug for every boot-era pool — the
      // in-fight RNG bundle fork (disc10 / rf2-5).
      m_heap_base = start;
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
  if (!m_hash_spans.empty())
  {
    size_t hash_bytes = 0;
    for (const Region& h : m_hash_spans)
      hash_bytes += h.end - h.start;
    INFO_LOG_FMT(EXPANSIONINTERFACE,
                 "MeleeRollback: device-side sync oracle: {} hash spans, {} bytes",
                 m_hash_spans.size(), hash_bytes);
  }
  // Arm the payload journal (see NotifyPayloadWrite) on a fresh run.
  m_deliveries.clear();
  m_delivery_seq = 0;
  m_delivery_bytes = 0;
  s_active_rollback_state = this;
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
  u32 heap_start = (u32(raw[0]) << 24) | (u32(raw[1]) << 16) | (u32(raw[2]) << 8) | raw[3];
  const u32 heap_end = (u32(raw[4]) << 24) | (u32(raw[5]) << 16) | (u32(raw[6]) << 8) | raw[7];
  if (m_heap_base != 0 && m_heap_base < heap_start)
    heap_start = m_heap_base;  // see the heapbase comment in LoadRegionTable

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
  slot.timebase = system.GetSystemTimers().GetFakeTimeBase();
  slot.io_epoch = AsyncIOEpoch(system);
  slot.epoch_dvd = system.GetDVDThread().GetNonDTKReadsCompleted() +
                   system.GetDVDInterface().GetNonDTKCommandsCompleted();
  slot.aram_int_pending = system.GetDSP().IsARAMDMAInProgress();
  slot.delivery_seq = m_delivery_seq;
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

  // Re-deliver async payloads written into restored memory since this slot's
  // capture (see NotifyPayloadWrite): the copy above rolled those bytes back
  // to pre-payload while the excluded (live) driver/queue bookkeeping says
  // the transfers are done — nobody will write them again. Payload sources
  // are pure, so replaying the journal in delivery order makes restored
  // memory match what the live bookkeeping believes.
  {
    const u64 first_retained =
        m_deliveries.empty() ? m_delivery_seq + 1 : m_deliveries.front().seq;
    if (slot.delivery_seq + 1 < first_retained)
    {
      m_redelivery_gaps++;
      ERROR_LOG_FMT(EXPANSIONINTERFACE,
                    "MeleeRollback: payload journal gap on restore to tick {} (need seq > {}, "
                    "oldest retained {}) — re-delivery incomplete",
                    tick, slot.delivery_seq, first_retained);
    }
    u32 spans = 0;
    size_t bytes = 0;
    for (const Delivery& d : m_deliveries)
    {
      if (d.seq <= slot.delivery_seq)
        continue;
      memory.CopyToEmu(d.addr, d.bytes.data(), d.bytes.size());
      spans++;
      bytes += d.bytes.size();
    }
    if (spans != 0)
    {
      m_redelivered += spans;
      INFO_LOG_FMT(EXPANSIONINTERFACE,
                   "MeleeRollback: re-delivered {} payload spans ({} bytes) after restore to {}",
                   spans, bytes, tick);
    }
  }

  // Rewind the GAME-VISIBLE time base to the snapshot's value, so replayed
  // ticks re-read the same mftb/OSGetTime values they saw originally and the
  // replay ends with tick<->TB in the original relationship. Without this,
  // every replay leaves the peer's TB permanently ahead (replayed ticks
  // execute real cycles), and anything the game times against the clock --
  // scene-load completion, title attract countdowns, the RNG stir -- lands
  // on a DIFFERENT tick than on the non-rolling peer. Observed as demo
  // fights starting one tick apart => checksum desync at the fight boundary
  // with a perfectly synchronized RNG orbit. Emulator-internal scheduling
  // (CoreTiming events, DVD/ARAM completions) is deliberately NOT rewound.
  auto& core_timing = system.GetCoreTiming();
  core_timing.SetFakeTBStartValue(slot.timebase);
  core_timing.SetFakeTBStartTicks(core_timing.GetTicks());
  system.GetPowerPC().WriteFullTimeBaseValue(slot.timebase);

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

u64 MeleeRollbackState::AsyncIOEpoch(Core::System& system)
{
  return system.GetDSP().GetARAMDMACompletionCount() +
         system.GetDVDThread().GetNonDTKReadsCompleted() +
         system.GetDVDInterface().GetNonDTKCommandsCompleted();
}

bool MeleeRollbackState::IOEpochUnchanged(Core::System& system, u32 tick) const
{
  const Slot& slot = m_ring[tick % RING_SIZE];
  if (!slot.valid || slot.tick != tick)
    return false;
  return slot.io_epoch == AsyncIOEpoch(system);
}

bool MeleeRollbackState::DVDEpochUnchanged(Core::System& system, u32 tick) const
{
  const Slot& slot = m_ring[tick % RING_SIZE];
  if (!slot.valid || slot.tick != tick)
    return false;
  return slot.epoch_dvd == system.GetDVDThread().GetNonDTKReadsCompleted() +
                               system.GetDVDInterface().GetNonDTKCommandsCompleted();
}

bool MeleeRollbackState::ARAMIntPending(u32 tick) const
{
  const Slot& slot = m_ring[tick % RING_SIZE];
  return slot.valid && slot.tick == tick && slot.aram_int_pending;
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

int MeleeRollbackState::VerifyAgainstRing(Core::System& system, u32 tick) const
{
  const Slot& slot = m_ring[tick % RING_SIZE];
  if (!slot.valid || slot.tick != tick || slot.data.size() != m_snapshot_bytes)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "MeleeRollback: verify: no ring entry for tick {}", tick);
    return -1;
  }

  auto& memory = system.GetMemory();
  std::vector<u8> live;
  size_t off = 0;
  int spans = 0;
  for (const Region& r : m_regions)
  {
    const size_t len = r.end - r.start;
    live.resize(len);
    memory.CopyFromEmu(live.data(), r.start, len);
    const u8* want = slot.data.data() + off;

    size_t i = 0;
    while (i < len && spans < 24)  // cap the log noise per verification
    {
      if (live[i] == want[i])
      {
        i++;
        continue;
      }
      size_t j = i;
      while (j < len && live[j] != want[j])
        j++;
      spans++;
      ERROR_LOG_FMT(EXPANSIONINTERFACE,
                    "MeleeRollback: VERIFY DIFF tick={} {}+0x{:x} (addr {:08x}) len {} "
                    "live={:02x}{:02x}{:02x}{:02x} want={:02x}{:02x}{:02x}{:02x}",
                    tick, r.label, i, r.start + i, j - i, live[i], i + 1 < len ? live[i + 1] : 0,
                    i + 2 < len ? live[i + 2] : 0, i + 3 < len ? live[i + 3] : 0, want[i],
                    i + 1 < len ? want[i + 1] : 0, i + 2 < len ? want[i + 2] : 0,
                    i + 3 < len ? want[i + 3] : 0);
      i = j;
    }
    off += len;
  }
  if (spans == 0)
    INFO_LOG_FMT(EXPANSIONINTERFACE, "MeleeRollback: verify tick {}: replay byte-exact", tick);
  return spans;
}

bool MeleeRollbackState::SnapshotChecksum(u32 tick, u32* out) const
{
  if (m_hash_spans.empty())
    return false;
  const Slot& slot = m_ring[tick % RING_SIZE];
  if (!slot.valid || slot.tick != tick || slot.data.size() != m_snapshot_bytes)
    return false;

  // Walk the copy plan once per hash span to find the captured bytes it
  // overlaps. Plan order (and thus hash order) is deterministic and identical
  // across peers for the static regions the hash spans live in; the heap
  // region joins the plan at the END, so a runtime heap-bound difference can
  // never reorder the spans hashed here.
  u32 hash = 0x811C9DC5;
  for (const Region& h : m_hash_spans)
  {
    size_t off = 0;
    for (const Region& r : m_regions)
    {
      const u32 lo = std::max(h.start, r.start);
      const u32 hi = std::min(h.end, r.end);
      if (lo < hi)
      {
        const u8* p = slot.data.data() + off + (lo - r.start);
        for (u32 i = 0; i < hi - lo; i++)
          hash = (hash ^ p[i]) * 0x01000193;
      }
      off += r.end - r.start;
    }
  }
  *out = hash;
  return true;
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

bool MeleeRollbackState::DumpLive(Core::System& system, const std::string& path) const
{
  auto& memory = system.GetMemory();
  std::ofstream out(path, std::ios::binary);
  if (!out)
    return false;
  // Text header (one region per line), then a blank line, then the raw
  // concatenated region bytes. The offline differ re-derives offsets from
  // the header, so dumps remain self-describing even if the table changes.
  std::ostringstream hdr;
  for (const Region& r : m_regions)
    hdr << fmt::format("{:08x} {:08x} {}\n", r.start, r.end, r.label);
  hdr << "\n";
  const std::string h = hdr.str();
  out.write(h.data(), h.size());
  std::vector<u8> buf;
  for (const Region& r : m_regions)
  {
    buf.resize(r.end - r.start);
    memory.CopyFromEmu(buf.data(), r.start, r.end - r.start);
    out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
  }
  return out.good();
}

bool MeleeRollbackState::DumpSnapshot(u32 tick, const std::string& path) const
{
  const Slot& slot = m_ring[tick % RING_SIZE];
  if (!slot.valid || slot.tick != tick || slot.data.size() != m_snapshot_bytes)
    return false;
  std::ofstream out(path, std::ios::binary);
  if (!out)
    return false;
  std::ostringstream hdr;
  for (const Region& r : m_regions)
    hdr << fmt::format("{:08x} {:08x} {}\n", r.start, r.end, r.label);
  hdr << "\n";
  const std::string h = hdr.str();
  out.write(h.data(), h.size());
  out.write(reinterpret_cast<const char*>(slot.data.data()), slot.data.size());
  return out.good();
}
}  // namespace ExpansionInterface
