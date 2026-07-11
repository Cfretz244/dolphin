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
#include <deque>
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
  ~MeleeRollbackState();

  // Ring depth bounds the rollback window; 16 ticks ≈ 267 ms at 60 Hz,
  // comfortably above any playable prediction window.
  static constexpr size_t RING_SIZE = 16;

  // Parses the region table (region/exclude/heapptr/hash/watch lines).
  // Returns false with *error set on malformed input. Exclusions are carved
  // out of the regions at load time, yielding a flat copy plan.
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

  // True when the game's scene state (first watch, the major-scene global)
  // is unchanged between the ring entry for `tick` and now. Restoring across
  // a scene transition is NEVER safe: scene loads (heap wipes, disc reads)
  // happen outside the replayable tick body, so a replay that straddles one
  // leaves the new scene's control flow running on pre-transition state —
  // observed as permanently wedged scene progression, not a crash.
  bool SameScene(Core::System& system, u32 tick) const;

  // Monotonic count of async I/O completions DELIVERED to the game (ARAM DMA
  // interrupts + non-DTK DVD read/command completions). Distinct from the
  // in-flight predicates (AsyncIOQuiescent): a completion that landed INSIDE
  // a rollback window is invisible to those — nothing is pending at restore
  // time — but restoring erases the game's record of a callback the emulator
  // will never re-fire, and Melee's HSD synth spin-waits on ARAM loads
  // forever (observed: both-peer stall right after a torture restore with
  // zero in-flight skips).
  static u64 AsyncIOEpoch(Core::System& system);

  // True when no async completion has been delivered since the ring entry
  // for `tick` was captured — i.e. restoring to it cannot swallow one.
  bool IOEpochUnchanged(Core::System& system, u32 tick) const;

  // DVD classes only (DVDThread non-DTK reads + DI non-DTK commands). A DVD
  // completion delivered after `tick`'s capture makes restoring that slot
  // unsafe: the restored dvd.o driver state machine livelocks re-issuing a
  // command the emulator considers done (v15 requal run 4, cbForStateBusy).
  // Fights issue ZERO non-DTK DVD traffic (v15q run 5 profile), so gating
  // rollback on this is free where it matters.
  bool DVDEpochUnchanged(Core::System& system, u32 tick) const;

  // True if an ARAM DMA completion interrupt was pending when `tick` was
  // captured (see Slot::aram_int_pending).
  bool ARAMIntPending(u32 tick) const;

  // Oldest restorable tick given the current ring contents, or -1 if none.
  s64 OldestTick() const;

  // FNV-1a over the LIVE contents of every region. Both peers run identical
  // region tables (same generated file for the same DOL), so cross-peer
  // equality is meaningful. Host-speed; cheap enough for every-60-ticks.
  u32 LiveChecksum(Core::System& system) const;

  // Cross-peer sync oracle over CONFIRMED state ("hash" lines): FNV-1a over
  // the hash spans as recorded in the ring entry for `tick`. Unlike a hash of
  // live memory (the game's CMD_CHECKSUM), a snapshot hash is immutable once
  // the tick is confirmed — it can never cover speculative or render-cadence-
  // dependent bytes, so it is safe to exchange under prediction without any
  // defer/park/discard protocol. Returns false if the ring no longer holds
  // `tick` (or the table has no hash spans). Bytes a hash span shares with an
  // exclusion are not captured and are silently skipped — identically on both
  // peers, since they run the same generated table.
  bool SnapshotChecksum(u32 tick, u32* out) const;
  bool HasHashSpans() const { return !m_hash_spans.empty(); }

  // Replay-fidelity oracle: memcmp the LIVE regions against the ring entry
  // for `tick` and log every differing span (region label + offset + first
  // bytes). At the exchange point of tick T, a perfect restore+replay must
  // reproduce ring[T] byte-for-byte — any diff is exactly the state the
  // region set failed to capture (or state mutated by non-replayable side
  // effects). Returns the number of differing spans.
  int VerifyAgainstRing(Core::System& system, u32 tick) const;

  // Capture-time statistics for perf logging.
  u64 TotalCaptures() const { return m_total_captures; }
  u64 LastCaptureMicros() const { return m_last_capture_us; }

  // ---- Async payload re-delivery -----------------------------------------
  // A DVD read (or ARAM->MRAM DMA) delivered between a slot's capture and a
  // restore of that slot writes payload into RESTORED memory: the restore
  // rolls the destination back to pre-payload bytes while the (excluded,
  // live) driver/queue bookkeeping says the transfer is done — nobody will
  // ever write it again, and the consumer reads garbage or spin-waits
  // (v17q1: both peers symmetrically wedged in the post-match scene load
  // after rollbacks straddled the GAME!-window preload reads). Payload
  // sources are PURE (disc reads, ARAM contents), so re-writing the
  // journaled bytes after the region copy makes the restored memory match
  // what the live bookkeeping believes. Only the spans overlapping restored
  // regions are journaled: excluded regions keep live bytes that may have
  // advanced PAST the delivery (e.g. streaming double-buffers) and must not
  // be rewound to an older payload.
  // Delivery sites (CPU thread, same as Capture/Restore — no locking):
  // DVDThread::FinishRead (non-DTK, copy_to_ram) and DSPManager::
  // Do_ARAM_DMA (ARAM->MRAM direction) call NotifyPayloadWrite.
  static void NotifyPayloadWrite(Core::System& system, u32 addr, u32 len);
  u64 RedeliveredSpans() const { return m_redelivered; }
  u64 RedeliveryGaps() const { return m_redelivery_gaps; }
  // Monotonic count of journaled restored-region delivery spans. The device
  // polls this to arm the payload fence (prediction quiesce): a delivery
  // into restored memory means async traffic is landing where a rollback
  // would orphan it — serve confirmed-only until the traffic stops.
  u64 DeliverySeq() const { return m_delivery_seq; }

  // Cross-peer divergence forensics: dump the LIVE region set (with a small
  // header naming each region's bounds) to a file. Both peers dump at the
  // same tick on a DESYNC; scripts/diff-desync-dumps.py names the exact
  // divergent bytes host-vs-client — the oracle VerifyAgainstRing cannot
  // provide, since it only sees one peer's memory.
  bool DumpLive(Core::System& system, const std::string& path) const;

  // Same file format as DumpLive, but from the ring entry for `tick` instead
  // of live memory: the PRE-state of the first divergent tick, byte-comparable
  // across peers by construction (both peers hold the same tick in their
  // rings). Returns false if the ring no longer holds `tick`.
  bool DumpSnapshot(u32 tick, const std::string& path) const;

  // Diagnostic watches ("watch" lines): u32 globals logged with checksums.
  struct Watch
  {
    u32 addr = 0;
    std::string label;
  };
  const std::vector<Watch>& Watches() const { return m_watches; }
  u32 ReadWatch(Core::System& system, const Watch& watch) const;

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
    u32 scene = 0;  // first watch (major-scene) at capture time
    u64 timebase = 0;  // game-visible TB at capture (see Restore)
    u64 io_epoch = 0;  // AsyncIOEpoch at capture (see IOEpochUnchanged)
    u64 epoch_dvd = 0;  // DVD part only (see DVDEpochUnchanged)
    // An ARAM DMA interrupt was pending at capture (copy already done, so
    // the snapshot holds the transferred data). If it fires before a restore
    // of this slot, the delivery must be re-raised (see the R1 restore path).
    bool aram_int_pending = false;
    u64 delivery_seq = 0;  // payload journal position at capture
    std::vector<u8> data;  // concatenated regions, m_snapshot_bytes long
  };

  struct Delivery
  {
    u64 seq = 0;
    u32 addr = 0;  // emu VA (cached mirror)
    std::vector<u8> bytes;
  };

  void ResolveHeapRegion(Core::System& system);
  void FinalizeCopyPlan();

  std::vector<Watch> m_watches;
  std::vector<Region> m_raw_regions;   // as parsed, pre-exclusion
  std::vector<Region> m_excludes;      // as parsed
  std::vector<Region> m_hash_spans;    // as parsed (SnapshotChecksum input)
  std::vector<Region> m_regions;       // carved, final copy plan
  u32 m_heapptr_lo = 0;                // emu addr holding heap start (u32be)
  u32 m_heapptr_hi = 0;                // emu addr holding heap end (u32be)
  u32 m_heap_base = 0;                 // fixed lower bound (static image end); 0 = runtime lo
  bool m_heap_resolved = false;
  size_t m_snapshot_bytes = 0;

  void NotePayloadWrite(Core::System& system, u32 addr, u32 len);

  std::array<Slot, RING_SIZE> m_ring;
  u64 m_total_captures = 0;
  u64 m_last_capture_us = 0;

  // Payload journal (see NotifyPayloadWrite). Byte-capped ring; a restore
  // needing evicted entries logs a gap (honest failure, counted).
  static constexpr size_t DELIVERY_BYTES_CAP = 16 * 1024 * 1024;
  std::deque<Delivery> m_deliveries;
  u64 m_delivery_seq = 0;
  size_t m_delivery_bytes = 0;
  u64 m_redelivered = 0;
  u64 m_redelivery_gaps = 0;
};
}  // namespace ExpansionInterface
