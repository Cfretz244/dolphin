// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/OverlayImages.h"

#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "Common/Hash.h"
#include "Common/Swap.h"

namespace DolphinTool
{

namespace
{

// Flattened snapshot instance used during the sweep.
struct Snap
{
  u32 addr;
  u32 size;  // bytes
  u32 crc;
  const std::vector<u32>* words;
  const std::vector<u64>* epochs;
};

struct Event
{
  u64 counter;
  bool is_kill;
  u32 snap_id;       // compile events
  u32 addr, length;  // kill events
};

// Returns true if the two snapshots' bytes agree everywhere their address ranges overlap.
bool BytesCompatible(const Snap& a, const Snap& b)
{
  const u32 lo = std::max(a.addr, b.addr);
  const u32 hi = std::min(a.addr + a.size, b.addr + b.size);
  for (u32 addr = lo; addr < hi; addr += 4)
  {
    if ((*a.words)[(addr - a.addr) / 4] != (*b.words)[(addr - b.addr) / 4])
      return false;
  }
  return true;
}

bool Overlaps(u32 a_lo, u32 a_hi, u32 b_lo, u32 b_hi)
{
  return a_lo < b_hi && b_lo < a_hi;
}

}  // namespace

// The sweep replays the shared compile/invalidation timeline, maintaining the set of
// live (resident) snapshots. Every time a kill is about to remove live content — a
// recorded invalidation or a byte-conflicting recompile (the authoritative signal, since
// invalidation events under-report) — the pre-kill live set is an OBSERVED overlay
// configuration; so is the final live set. Each observed configuration is split into
// spatially-connected clusters (gap threshold), and each unique cluster becomes an
// image. Images are therefore only ever code that was genuinely co-resident, so their
// bytes are consistent by construction and the runtime's whole-image hash can verify
// them. Repeated configurations dedup by member set; distinct-but-overlapping
// configurations (e.g. two swap regions changing independently within gap range) emit
// one image each, which trades table size for verifiability.
std::vector<OverlayImage> BuildOverlayImages(const std::vector<TraceSnapshotBlock>& blocks,
                                             const std::vector<TraceSmcEvent>& smc_events,
                                             u32 gap_threshold, u32 max_versions_per_block,
                                             bool verbose, OverlayBuildStats* stats)
{
  OverlayBuildStats local_stats{};

  // Flatten snapshots, excluding hyper-mutable addresses (genuine SMC sites whose
  // content churns; translating any one version would rarely verify at runtime).
  std::vector<Snap> snaps;
  for (const auto& block : blocks)
  {
    local_stats.snapshot_blocks++;
    if (block.snapshots.size() > max_versions_per_block)
    {
      local_stats.hyper_mutable_blocks++;
      continue;
    }
    for (const auto& s : block.snapshots)
    {
      local_stats.snapshots++;
      snaps.push_back(
          {s.addr, static_cast<u32>(s.words.size() * 4), s.crc32, &s.words, &s.epochs});
    }
  }

  // Timeline events: compiles (one per epoch per snapshot) and invalidations share the
  // collector's event counter, so a plain sort interleaves them correctly.
  std::vector<Event> events;
  for (u32 id = 0; id < static_cast<u32>(snaps.size()); id++)
  {
    for (u64 e : *snaps[id].epochs)
      events.push_back({e, false, id, 0, 0});
  }
  for (const auto& smc : smc_events)
    events.push_back({smc.counter, true, 0, smc.addr, smc.length});
  std::sort(events.begin(), events.end(),
            [](const Event& a, const Event& b) { return a.counter < b.counter; });

  std::map<u32, std::pair<u32, u32>> live;  // start -> (end, snap_id)
  bool dirty = false;  // compiles since the last configuration capture

  // Configuration capture: cluster the live set spatially; record each cluster's member
  // ids (sorted vector) deduped across captures.
  std::vector<std::vector<u32>> configurations;
  std::unordered_set<u64> seen_configs;
  auto capture = [&]() -> bool {
    if (!dirty)
      return false;
    dirty = false;
    std::vector<u32> cluster;
    u32 cluster_end = 0;
    auto flush_cluster = [&]() {
      if (cluster.empty())
        return;
      std::sort(cluster.begin(), cluster.end());
      u64 h = 14695981039346656037ULL;
      for (u32 id : cluster)
      {
        h ^= (u64(snaps[id].addr) << 32) | snaps[id].crc;
        h *= 1099511628211ULL;
      }
      if (seen_configs.insert(h).second)
        configurations.push_back(cluster);
      cluster.clear();
    };
    for (const auto& [start, val] : live)
    {
      if (!cluster.empty() && start > cluster_end + gap_threshold)
        flush_cluster();
      cluster.push_back(val.second);
      cluster_end = std::max(cluster_end, val.first);
    }
    flush_cluster();
    return true;
  };

  auto erase_overlapping = [&](u32 lo, u32 hi, u32* killed) {
    // Live intervals are bounded in size by the JIT's max block length; scan a window.
    constexpr u32 MAX_BLOCK_BYTES = 40000;
    const u32 scan_lo = lo > MAX_BLOCK_BYTES ? lo - MAX_BLOCK_BYTES : 0;
    for (auto it = live.lower_bound(scan_lo); it != live.end() && it->first < hi;)
    {
      if (Overlaps(it->first, it->second.first, lo, hi))
      {
        (*killed)++;
        it = live.erase(it);
      }
      else
      {
        ++it;
      }
    }
  };

  for (const Event& ev : events)
  {
    if (ev.is_kill)
    {
      // Only a kill that actually removes live content ends a configuration.
      u32 killed = 0;
      constexpr u32 MAX_BLOCK_BYTES = 40000;
      const u32 scan_lo = ev.addr > MAX_BLOCK_BYTES ? ev.addr - MAX_BLOCK_BYTES : 0;
      bool hits = false;
      for (auto it = live.lower_bound(scan_lo);
           it != live.end() && it->first < ev.addr + ev.length; ++it)
      {
        if (Overlaps(it->first, it->second.first, ev.addr, ev.addr + ev.length))
        {
          hits = true;
          break;
        }
      }
      if (hits)
      {
        capture();
        erase_overlapping(ev.addr, ev.addr + ev.length, &killed);
        local_stats.smc_kills += killed;
      }
      continue;
    }

    const Snap& s = snaps[ev.snap_id];
    const u32 s_end = s.addr + s.size;

    // A byte-conflicting overlap is a missed generation boundary: the prior
    // configuration ended here, even though no invalidation was recorded.
    bool conflict = false;
    {
      constexpr u32 MAX_BLOCK_BYTES = 40000;
      const u32 scan_lo = s.addr > MAX_BLOCK_BYTES ? s.addr - MAX_BLOCK_BYTES : 0;
      for (auto it = live.lower_bound(scan_lo); it != live.end() && it->first < s_end; ++it)
      {
        const u32 o_id = it->second.second;
        if (o_id != ev.snap_id && Overlaps(it->first, it->second.first, s.addr, s_end) &&
            !BytesCompatible(s, snaps[o_id]))
        {
          conflict = true;
          break;
        }
      }
    }
    if (conflict)
    {
      if (capture())
        local_stats.conflict_captures++;
      u32 killed = 0;
      erase_overlapping(s.addr, s_end, &killed);
      local_stats.conflict_kills += killed;
    }

    // Refresh: drop a stale live entry at this start address (same-content re-observation
    // or a compatible resize) before inserting.
    auto existing = live.find(s.addr);
    if (existing != live.end())
      live.erase(existing);
    live.emplace(s.addr, std::make_pair(s_end, ev.snap_id));
    dirty = true;
  }
  capture();  // final configuration

  // Materialize each unique configuration cluster into an image.
  std::vector<OverlayImage> images;
  for (const auto& members : configurations)
  {
    // Write every member's words into an address->word map. Members were simultaneously
    // live, so overlapping bytes agree; overlaps can still exist (e.g. a large block
    // superseded by a compatible smaller re-split while both entries were refreshed).
    std::map<u32, u32> word_at;
    OverlayImage img;
    for (u32 id : members)
    {
      const Snap& s = snaps[id];
      img.member_blocks.insert(s.addr);
      for (u32 i = 0; i < s.size / 4; i++)
        word_at[s.addr + i * 4] = (*s.words)[i];
    }

    OverlayRange range;
    u32 expected = 0;
    for (const auto& [addr, word] : word_at)
    {
      if (range.bytes.empty() || addr != expected)
      {
        if (!range.bytes.empty())
          img.ranges.push_back(std::move(range));
        range = OverlayRange{};
        range.addr = addr;
      }
      const u32 be = Common::swap32(word);  // guest RAM byte order
      const u8* p = reinterpret_cast<const u8*>(&be);
      range.bytes.insert(range.bytes.end(), p, p + 4);
      expected = addr + 4;
    }
    if (!range.bytes.empty())
      img.ranges.push_back(std::move(range));
    if (img.ranges.empty())
      continue;

    img.base = img.ranges.front().addr;
    img.end = img.ranges.back().addr + static_cast<u32>(img.ranges.back().bytes.size());

    std::vector<u8> all_bytes;
    for (const auto& r : img.ranges)
      all_bytes.insert(all_bytes.end(), r.bytes.begin(), r.bytes.end());
    img.full_crc32 = Common::ComputeCRC32(all_bytes.data(), all_bytes.size());
    const size_t prefix_len = std::min<size_t>(64, img.ranges.front().bytes.size());
    img.prefix_crc32 = Common::ComputeCRC32(img.ranges.front().bytes.data(), prefix_len);

    images.push_back(std::move(img));
  }

  // Deduplicate identical images (same base, extent, and content); merge member sets.
  // Different member partitions can materialize to the same bytes.
  std::sort(images.begin(), images.end(), [](const OverlayImage& a, const OverlayImage& b) {
    if (a.base != b.base)
      return a.base < b.base;
    if (a.end != b.end)
      return a.end < b.end;
    return a.full_crc32 < b.full_crc32;
  });
  std::vector<OverlayImage> deduped;
  for (auto& img : images)
  {
    if (!deduped.empty())
    {
      auto& prev = deduped.back();
      if (prev.base == img.base && prev.end == img.end && prev.full_crc32 == img.full_crc32 &&
          prev.ranges.size() == img.ranges.size())
      {
        prev.member_blocks.insert(img.member_blocks.begin(), img.member_blocks.end());
        continue;
      }
    }
    deduped.push_back(std::move(img));
  }

  // Warn on identical content at multiple bases (fixed-link-address assumption check).
  std::unordered_map<u64, u32> content_first_base;
  for (const auto& img : deduped)
  {
    const u64 key = (u64(img.full_crc32) << 32) | (img.end - img.base);
    auto [it, inserted] = content_first_base.emplace(key, img.base);
    if (!inserted && it->second != img.base)
    {
      local_stats.multi_base_duplicates++;
      if (verbose)
      {
        fmt::println(std::cerr,
                     "  overlay warning: identical content at {:#010x} and {:#010x} "
                     "({} bytes) — emitting both",
                     it->second, img.base, img.end - img.base);
      }
    }
  }

  if (stats)
    *stats = local_stats;
  return deduped;
}

}  // namespace DolphinTool
