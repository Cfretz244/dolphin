// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <vector>

#include "Common/CommonTypes.h"

namespace DolphinTool
{

// Offline clustering of trace-time instruction snapshots into overlay images.
//
// Camelot-style overlay engines decompress code from disc archives into a RAM arena at
// runtime, with different overlays occupying the same addresses at different times. The
// v4 trace format captures each compiled block's bytes plus the epochs (shared event
// counter with SMC/invalidation events) at which that content was observed. This module
// replays that timeline to determine which snapshots were simultaneously resident
// ("co-residency"), clusters spatially-close co-resident snapshots into images, and
// materializes each image's byte ranges for translation and runtime hash verification.
//
// Soundness note: invalidation events under-report (JitCache skips lines that never held
// compiled code), so byte CONFLICT between a new compile and live content is the
// authoritative generation boundary; SMC events are corroborating hints. A mis-grouped
// image can only fail runtime hash verification (coverage loss), never activate wrong
// code.

// One content version of one traced block, parsed from the v4 snapshot section.
struct TraceSnapshot
{
  u32 addr = 0;
  u32 crc32 = 0;                // dedup identity from the collector (host-endian words)
  std::vector<u64> epochs;      // event-counter values at each (re)compile
  std::vector<u32> words;       // host-endian instruction words
};

struct TraceSnapshotBlock
{
  u32 addr = 0;
  std::vector<TraceSnapshot> snapshots;
};

struct TraceSmcEvent
{
  u32 addr = 0;
  u32 length = 0;
  u64 counter = 0;
};

// A coalesced, byte-consistent captured range. `bytes` are BIG-ENDIAN, exactly as guest
// RAM stores them — the same byte order the runtime hashes at activation time and the
// same order PPCMemoryImage/translate expect.
struct OverlayRange
{
  u32 addr = 0;
  std::vector<u8> bytes;
};

struct OverlayImage
{
  u32 base = 0;  // first captured byte
  u32 end = 0;   // one past last captured byte
  u32 full_crc32 = 0;    // over all range bytes concatenated in address order
  u32 prefix_crc32 = 0;  // over the first min(64, ranges[0].size) bytes at base
  std::vector<OverlayRange> ranges;  // sorted by addr, non-overlapping
  std::set<u32> member_blocks;       // traced block start addrs belonging to this image
};

struct OverlayBuildStats
{
  u32 snapshot_blocks = 0;      // arena blocks considered
  u32 snapshots = 0;            // content versions considered
  u32 hyper_mutable_blocks = 0; // excluded: too many content versions (genuine SMC)
  u32 conflict_kills = 0;       // live intervals killed by byte conflict (missed invalidation)
  u32 smc_kills = 0;            // live intervals killed by recorded invalidation events
  // Configurations captured because of a byte conflict rather than a recorded
  // invalidation. These can include transitional mixes (part-old, part-new content that
  // was never truly co-resident); such images are runtime-harmless — they simply never
  // hash-verify — but a high count here means table bloat and warrants pruning.
  u32 conflict_captures = 0;
  u32 multi_base_duplicates = 0;  // identical content observed at 2+ bases
};

// Cluster arena snapshots into images. `gap_threshold` is the maximum address gap (in
// bytes) between co-resident snapshots that still counts as "same overlay".
// `max_versions_per_block` excludes hyper-mutable addresses (genuine self-modifying
// code) from all images. Returned images are sorted by (base, full_crc32).
std::vector<OverlayImage> BuildOverlayImages(const std::vector<TraceSnapshotBlock>& blocks,
                                             const std::vector<TraceSmcEvent>& smc_events,
                                             u32 gap_threshold, u32 max_versions_per_block,
                                             bool verbose, OverlayBuildStats* stats);

}  // namespace DolphinTool
