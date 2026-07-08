// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{

// Collects PPC execution trace data for AOT (Ahead-of-Time) static recompilation.
// Records block entries, branch edges (static and dynamic), and self-modifying code events.
// Data accumulates across play sessions via merge-on-flush.
class TraceCollector final
{
public:
  enum class EdgeType : u8
  {
    Static = 0,
    Dynamic = 1,
    Call = 2,
  };

  // Static function callable from JIT-emitted code via ABI_CallFunction.
  // Follows the same pattern as BranchWatch::HitVirtualTrue_fk.
  static void LogDynamicBranch(TraceCollector* collector, u32 origin, u32 destination);

  // Called from C++ code paths (not from JIT-emitted code)
  void RecordStaticEdge(u32 from_addr, u32 to_addr, bool is_call);
  void OnICacheInvalidation(u32 address, u32 length);

  // Record a block's metadata and instruction bytes. Called at JIT compile time
  // (FinalizeBlock). instruction_words points to block_size PPC instruction words as
  // fetched from guest memory. Snapshots are CRC32-deduplicated per address; every call
  // (including dedup hits) appends the current event counter to the matching snapshot's
  // epoch list, giving offline clustering co-residency evidence on the same timeline as
  // SMC events.
  void RecordBlock(u32 ppc_addr, u32 block_size, const u32* instruction_words);

  // Record a unique vertex loader format configuration.
  // Called from VertexLoaderManager when a new format is first seen during tracing.
  void RecordVertexFormat(u32 vtx_desc_low, u32 vtx_desc_high, u32 vat_g0, u32 vat_g1, u32 vat_g2);

  bool IsActive() const { return m_active; }
  void SetActive(bool active) { m_active = active; }

  void FlushToDisk(const std::string& path) const;
  void MergeFromDisk(const std::string& path);
  void Clear();

private:
  struct InstructionSnapshot
  {
    std::vector<u32> instructions;
    u32 crc32;
    // Event-counter values at each (re)compile that observed this content. Shares the
    // timeline with SMCRecord::event_counter; sessions are rebased to disjoint ranges
    // on merge so co-residency evidence never spans sessions.
    std::vector<u64> epochs;
  };

  struct BlockRecord
  {
    u32 ppc_addr;
    u32 block_size;
    std::vector<InstructionSnapshot> snapshots;
  };

  struct EdgeRecord
  {
    u32 from_addr;
    u32 to_addr;
    EdgeType type;
  };

  struct SMCRecord
  {
    u32 addr;
    u32 length;
    u64 event_counter;
  };

  // Binary file format constants
  static constexpr u32 MAGIC = 0x54485044;  // "DPHT" little-endian
  static constexpr u32 FORMAT_VERSION = 4;

  struct FileHeader
  {
    u32 magic;
    u32 version;
    u32 block_count;
    u32 edge_count;
    u32 smc_count;
    u32 vtx_format_count;         // v3+
    u32 snapshot_section_offset;  // v4+: byte offset from file start to snapshot section
  };

  // Block addresses are always 4-byte aligned, so the low two bits of `to` are free to
  // carry the edge type (three values). The previous scheme XORed type<<62 into the low
  // half, which lands in `from`'s bits and let distinct edge types collide on one key.
  static u64 MakeEdgeKey(u32 from, u32 to, EdgeType type)
  {
    return (u64(from) << 32) | u64(to) | u64(type);
  }

  bool m_active = false;
  std::unordered_map<u32, BlockRecord> m_blocks;
  std::unordered_map<u64, EdgeRecord> m_edges;
  std::vector<SMCRecord> m_smc_events;
  std::set<std::array<u32, 5>> m_vertex_formats;
  u64 m_event_counter = 0;
  mutable std::mutex m_mutex;
};

}  // namespace Core
