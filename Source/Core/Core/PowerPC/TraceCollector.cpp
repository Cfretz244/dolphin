// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/TraceCollector.h"

#include <algorithm>

#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace Core
{

void TraceCollector::LogDynamicBranch(TraceCollector* collector, u32 origin, u32 destination)
{
  const EdgeType type = EdgeType::Dynamic;
  const u64 key = MakeEdgeKey(origin, destination, type);
  if (!collector->m_edges.contains(key))
  {
    collector->m_edges[key] = {origin, destination, type};
  }
}

void TraceCollector::RecordStaticEdge(u32 from_addr, u32 to_addr, bool is_call)
{
  const EdgeType type = is_call ? EdgeType::Call : EdgeType::Static;
  const u64 key = MakeEdgeKey(from_addr, to_addr, type);
  if (!m_edges.contains(key))
  {
    m_edges[key] = {from_addr, to_addr, type};
  }
}

void TraceCollector::OnICacheInvalidation(u32 address, u32 length)
{
  m_smc_events.push_back({address, length, m_event_counter++});
}

void TraceCollector::RecordBlock(u32 ppc_addr, u32 block_size, const u32* instruction_words)
{
  const u32 crc = Common::ComputeCRC32(reinterpret_cast<const u8*>(instruction_words),
                                       block_size * sizeof(u32));
  const u64 epoch = m_event_counter++;

  auto& block = m_blocks[ppc_addr];
  block.ppc_addr = ppc_addr;
  block.block_size = block_size;  // recompilation keeps latest size

  for (auto& existing : block.snapshots)
  {
    if (existing.crc32 == crc && existing.instructions.size() == block_size)
    {
      existing.epochs.push_back(epoch);
      return;
    }
  }

  InstructionSnapshot snap;
  snap.instructions.assign(instruction_words, instruction_words + block_size);
  snap.crc32 = crc;
  snap.epochs.push_back(epoch);
  block.snapshots.push_back(std::move(snap));
}

void TraceCollector::RecordVertexFormat(u32 vtx_desc_low, u32 vtx_desc_high, u32 vat_g0,
                                        u32 vat_g1, u32 vat_g2)
{
  m_vertex_formats.insert({vtx_desc_low, vtx_desc_high, vat_g0, vat_g1, vat_g2});
}

void TraceCollector::FlushToDisk(const std::string& path) const
{
  std::lock_guard lock(m_mutex);

  File::CreateFullPath(path);
  File::IOFile file(path, "wb");
  if (!file.IsOpen())
  {
    ERROR_LOG_FMT(DYNA_REC, "TraceCollector: Failed to open {} for writing", path);
    return;
  }

  FileHeader header{};
  header.magic = MAGIC;
  header.version = FORMAT_VERSION;
  header.block_count = static_cast<u32>(m_blocks.size());
  header.edge_count = static_cast<u32>(m_edges.size());
  header.smc_count = static_cast<u32>(m_smc_events.size());
  header.vtx_format_count = static_cast<u32>(m_vertex_formats.size());
  header.snapshot_section_offset = 0;  // patched after the fixed-size sections
  file.WriteBytes(&header, sizeof(header));

  // Write blocks (8 bytes each: u32 addr + u32 size)
  for (const auto& [addr, block] : m_blocks)
  {
    file.WriteBytes(&block.ppc_addr, sizeof(u32));
    file.WriteBytes(&block.block_size, sizeof(u32));
  }

  // Write edges (12 bytes each: u32 from + u32 to + u8 type + u8[3] padding)
  for (const auto& [key, edge] : m_edges)
  {
    file.WriteBytes(&edge.from_addr, sizeof(u32));
    file.WriteBytes(&edge.to_addr, sizeof(u32));
    u8 type_val = static_cast<u8>(edge.type);
    file.WriteBytes(&type_val, sizeof(u8));
    u8 padding[3] = {};
    file.WriteBytes(padding, sizeof(padding));
  }

  // Write SMC events (16 bytes each)
  for (const auto& smc : m_smc_events)
  {
    file.WriteBytes(&smc.addr, sizeof(u32));
    file.WriteBytes(&smc.length, sizeof(u32));
    file.WriteBytes(&smc.event_counter, sizeof(u64));
  }

  // Write vertex format records (20 bytes each: 5 x u32)
  for (const auto& fmt : m_vertex_formats)
  {
    for (u32 val : fmt)
      file.WriteBytes(&val, sizeof(u32));
  }

  // Snapshot section (v4, variable length):
  //   { u32 block_addr, u32 num_snapshots }
  //     x { u32 num_instructions, u32 crc32, u32 num_epochs, u64 epochs[], u32 words[] }
  header.snapshot_section_offset = static_cast<u32>(file.Tell());
  u32 blocks_with_snapshots = 0;
  u64 total_snapshots = 0;
  for (const auto& [addr, block] : m_blocks)
  {
    if (block.snapshots.empty())
      continue;
    blocks_with_snapshots++;
    total_snapshots += block.snapshots.size();
    const u32 num_snapshots = static_cast<u32>(block.snapshots.size());
    file.WriteBytes(&block.ppc_addr, sizeof(u32));
    file.WriteBytes(&num_snapshots, sizeof(u32));
    for (const auto& snap : block.snapshots)
    {
      const u32 num_instructions = static_cast<u32>(snap.instructions.size());
      const u32 num_epochs = static_cast<u32>(snap.epochs.size());
      file.WriteBytes(&num_instructions, sizeof(u32));
      file.WriteBytes(&snap.crc32, sizeof(u32));
      file.WriteBytes(&num_epochs, sizeof(u32));
      file.WriteBytes(snap.epochs.data(), num_epochs * sizeof(u64));
      file.WriteBytes(snap.instructions.data(), num_instructions * sizeof(u32));
    }
  }

  file.Seek(0, File::SeekOrigin::Begin);
  file.WriteBytes(&header, sizeof(header));

  INFO_LOG_FMT(DYNA_REC,
               "TraceCollector: Flushed {} blocks ({} with snapshots, {} snapshots), {} edges, "
               "{} SMC events, {} vertex formats to {}",
               header.block_count, blocks_with_snapshots, total_snapshots, header.edge_count,
               header.smc_count, header.vtx_format_count, path);
}

void TraceCollector::MergeFromDisk(const std::string& path)
{
  std::lock_guard lock(m_mutex);

  File::IOFile file(path, "rb");
  if (!file.IsOpen())
    return;

  // Read the header field-by-field to handle v2 (20B) / v3 (24B) / v4 (28B) sizes.
  FileHeader header{};
  if (!file.ReadBytes(&header.magic, sizeof(u32)) ||
      !file.ReadBytes(&header.version, sizeof(u32)) ||
      !file.ReadBytes(&header.block_count, sizeof(u32)) ||
      !file.ReadBytes(&header.edge_count, sizeof(u32)) ||
      !file.ReadBytes(&header.smc_count, sizeof(u32)))
  {
    return;
  }

  if (header.magic != MAGIC || header.version < 2 || header.version > FORMAT_VERSION)
  {
    WARN_LOG_FMT(DYNA_REC, "TraceCollector: Incompatible trace file {} (version {}), skipping merge",
                 path, header.version);
    return;
  }

  if (header.version >= 3 && !file.ReadBytes(&header.vtx_format_count, sizeof(u32)))
    return;
  if (header.version >= 4 && !file.ReadBytes(&header.snapshot_section_offset, sizeof(u32)))
    return;

  // Read all file records into locals first: the in-memory (current-session) event
  // counters start at 0 every session, so before merging they must be rebased past the
  // file's counter range to keep sessions on disjoint epoch ranges (offline clustering
  // uses epoch adjacency as co-residency evidence, which must never span sessions).
  struct FileSnapshotBlock
  {
    u32 addr;
    u32 block_size;
    std::vector<InstructionSnapshot> snapshots;
  };
  std::vector<BlockRecord> file_blocks;
  std::vector<EdgeRecord> file_edges;
  std::vector<SMCRecord> file_smc;
  std::vector<std::array<u32, 5>> file_vtx;
  std::vector<FileSnapshotBlock> file_snapshot_blocks;

  file_blocks.reserve(header.block_count);
  for (u32 i = 0; i < header.block_count; i++)
  {
    u32 ppc_addr, block_size;
    if (!file.ReadBytes(&ppc_addr, sizeof(u32)) || !file.ReadBytes(&block_size, sizeof(u32)))
      return;
    file_blocks.push_back({ppc_addr, block_size, {}});
  }

  file_edges.reserve(header.edge_count);
  for (u32 i = 0; i < header.edge_count; i++)
  {
    u32 from_addr, to_addr;
    u8 type_val;
    u8 padding[3];
    if (!file.ReadBytes(&from_addr, sizeof(u32)) || !file.ReadBytes(&to_addr, sizeof(u32)) ||
        !file.ReadBytes(&type_val, sizeof(u8)) || !file.ReadBytes(padding, sizeof(padding)))
    {
      return;
    }
    file_edges.push_back({from_addr, to_addr, static_cast<EdgeType>(type_val)});
  }

  u64 file_max_counter = 0;
  file_smc.reserve(header.smc_count);
  for (u32 i = 0; i < header.smc_count; i++)
  {
    u32 addr, length;
    u64 event_counter;
    if (!file.ReadBytes(&addr, sizeof(u32)) || !file.ReadBytes(&length, sizeof(u32)) ||
        !file.ReadBytes(&event_counter, sizeof(u64)))
    {
      return;
    }
    file_smc.push_back({addr, length, event_counter});
    file_max_counter = std::max(file_max_counter, event_counter);
  }

  file_vtx.reserve(header.vtx_format_count);
  for (u32 i = 0; i < header.vtx_format_count; i++)
  {
    std::array<u32, 5> fmt;
    for (u32& val : fmt)
    {
      if (!file.ReadBytes(&val, sizeof(u32)))
        return;
    }
    file_vtx.push_back(fmt);
  }

  // Snapshot section (v4+)
  if (header.version >= 4 && header.snapshot_section_offset > 0)
  {
    file.Seek(header.snapshot_section_offset, File::SeekOrigin::Begin);
    const u64 file_size = file.GetSize();
    while (file.Tell() < file_size)
    {
      FileSnapshotBlock sb{};
      u32 num_snapshots;
      if (!file.ReadBytes(&sb.addr, sizeof(u32)) || !file.ReadBytes(&num_snapshots, sizeof(u32)))
        break;
      for (u32 si = 0; si < num_snapshots; si++)
      {
        InstructionSnapshot snap;
        u32 num_instructions, num_epochs;
        if (!file.ReadBytes(&num_instructions, sizeof(u32)) ||
            !file.ReadBytes(&snap.crc32, sizeof(u32)) ||
            !file.ReadBytes(&num_epochs, sizeof(u32)))
        {
          return;
        }
        snap.epochs.resize(num_epochs);
        if (!file.ReadBytes(snap.epochs.data(), num_epochs * sizeof(u64)))
          return;
        snap.instructions.resize(num_instructions);
        if (!file.ReadBytes(snap.instructions.data(), num_instructions * sizeof(u32)))
          return;
        for (u64 e : snap.epochs)
          file_max_counter = std::max(file_max_counter, e);
        sb.block_size = num_instructions;
        sb.snapshots.push_back(std::move(snap));
      }
      file_snapshot_blocks.push_back(std::move(sb));
    }
  }

  // Rebase the current session's counters past the file's range.
  const u64 rebase = file_max_counter + 1;
  for (auto& smc : m_smc_events)
    smc.event_counter += rebase;
  for (auto& [addr, block] : m_blocks)
  {
    for (auto& snap : block.snapshots)
    {
      for (u64& e : snap.epochs)
        e += rebase;
    }
  }
  m_event_counter += rebase;

  // Merge file records (their counters are already in-range).
  for (const auto& fb : file_blocks)
  {
    if (!m_blocks.contains(fb.ppc_addr))
      m_blocks[fb.ppc_addr] = {fb.ppc_addr, fb.block_size, {}};
  }

  for (const auto& fe : file_edges)
  {
    const u64 key = MakeEdgeKey(fe.from_addr, fe.to_addr, fe.type);
    if (!m_edges.contains(key))
      m_edges[key] = fe;
  }

  for (const auto& smc : file_smc)
    m_smc_events.push_back(smc);

  for (const auto& fmt : file_vtx)
    m_vertex_formats.insert(fmt);

  u64 merged_snapshots = 0;
  for (auto& sb : file_snapshot_blocks)
  {
    auto& block = m_blocks[sb.addr];
    if (block.ppc_addr == 0)
      block = {sb.addr, sb.block_size, {}};
    for (auto& snap : sb.snapshots)
    {
      InstructionSnapshot* match = nullptr;
      for (auto& existing : block.snapshots)
      {
        if (existing.crc32 == snap.crc32 &&
            existing.instructions.size() == snap.instructions.size())
        {
          match = &existing;
          break;
        }
      }
      if (match)
      {
        match->epochs.insert(match->epochs.end(), snap.epochs.begin(), snap.epochs.end());
      }
      else
      {
        block.snapshots.push_back(std::move(snap));
        merged_snapshots++;
      }
    }
  }

  INFO_LOG_FMT(DYNA_REC,
               "TraceCollector: Merged {} blocks, {} edges, {} SMC events, {} vertex formats, "
               "{} new snapshots from {} (session epochs rebased by {})",
               header.block_count, header.edge_count, header.smc_count, header.vtx_format_count,
               merged_snapshots, path, rebase);
}

void TraceCollector::Clear()
{
  m_blocks.clear();
  m_edges.clear();
  m_smc_events.clear();
  m_vertex_formats.clear();
  m_event_counter = 0;
}

}  // namespace Core
