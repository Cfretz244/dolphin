// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/TraceCollector.h"

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

  auto it = m_blocks.find(ppc_addr);
  if (it == m_blocks.end())
  {
    InstructionSnapshot snap;
    snap.instructions.assign(instruction_words, instruction_words + block_size);
    snap.crc32 = crc;
    m_blocks[ppc_addr] = {ppc_addr, block_size, {std::move(snap)}};
  }
  else
  {
    it->second.block_size = block_size;

    // Only store a new snapshot if the CRC32 doesn't match any existing one
    bool found = false;
    for (const auto& existing : it->second.snapshots)
    {
      if (existing.crc32 == crc && existing.instructions.size() == block_size)
      {
        found = true;
        break;
      }
    }
    if (!found)
    {
      InstructionSnapshot snap;
      snap.instructions.assign(instruction_words, instruction_words + block_size);
      snap.crc32 = crc;
      it->second.snapshots.push_back(std::move(snap));
    }
  }
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
  header.snapshot_section_offset = 0;  // will be patched below
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

  // Record the snapshot section offset and patch the header
  const u64 snapshot_offset = file.Tell();
  header.snapshot_section_offset = static_cast<u32>(snapshot_offset);

  // Write instruction snapshots (variable length)
  u32 blocks_with_snapshots = 0;
  for (const auto& [addr, block] : m_blocks)
  {
    if (block.snapshots.empty())
      continue;
    blocks_with_snapshots++;
    u32 block_addr = block.ppc_addr;
    u32 num_snapshots = static_cast<u32>(block.snapshots.size());
    file.WriteBytes(&block_addr, sizeof(u32));
    file.WriteBytes(&num_snapshots, sizeof(u32));
    for (const auto& snap : block.snapshots)
    {
      u32 num_instructions = static_cast<u32>(snap.instructions.size());
      file.WriteBytes(&num_instructions, sizeof(u32));
      file.WriteBytes(&snap.crc32, sizeof(u32));
      file.WriteBytes(snap.instructions.data(), num_instructions * sizeof(u32));
    }
  }

  // Patch the header with the snapshot section offset
  file.Seek(0, File::SeekOrigin::Begin);
  file.WriteBytes(&header, sizeof(header));

  INFO_LOG_FMT(DYNA_REC,
               "TraceCollector: Flushed {} blocks ({} with snapshots), {} edges, "
               "{} SMC events, {} vertex formats to {}",
               header.block_count, blocks_with_snapshots, header.edge_count, header.smc_count,
               header.vtx_format_count, path);
}

void TraceCollector::MergeFromDisk(const std::string& path)
{
  std::lock_guard lock(m_mutex);

  File::IOFile file(path, "rb");
  if (!file.IsOpen())
    return;

  // Read header field-by-field to handle v2/v3/v4 size differences
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

  // v3+: read vtx_format_count
  if (header.version >= 3)
  {
    if (!file.ReadBytes(&header.vtx_format_count, sizeof(u32)))
      return;
  }

  // v4+: read snapshot_section_offset
  if (header.version >= 4)
  {
    if (!file.ReadBytes(&header.snapshot_section_offset, sizeof(u32)))
      return;
  }

  // Merge blocks
  for (u32 i = 0; i < header.block_count; i++)
  {
    u32 ppc_addr, block_size;
    if (!file.ReadBytes(&ppc_addr, sizeof(u32)) || !file.ReadBytes(&block_size, sizeof(u32)))
      return;

    if (!m_blocks.contains(ppc_addr))
      m_blocks[ppc_addr] = {ppc_addr, block_size, {}};
  }

  // Merge edges
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

    EdgeType type = static_cast<EdgeType>(type_val);
    u64 key = MakeEdgeKey(from_addr, to_addr, type);
    if (!m_edges.contains(key))
      m_edges[key] = {from_addr, to_addr, type};
  }

  // Merge SMC events (append)
  for (u32 i = 0; i < header.smc_count; i++)
  {
    u32 addr, length;
    u64 event_counter;
    if (!file.ReadBytes(&addr, sizeof(u32)) || !file.ReadBytes(&length, sizeof(u32)) ||
        !file.ReadBytes(&event_counter, sizeof(u64)))
    {
      return;
    }
    m_smc_events.push_back({addr, length, event_counter});
  }

  // Merge vertex format records (v3+)
  for (u32 i = 0; i < header.vtx_format_count; i++)
  {
    std::array<u32, 5> fmt;
    for (u32& val : fmt)
    {
      if (!file.ReadBytes(&val, sizeof(u32)))
        return;
    }
    m_vertex_formats.insert(fmt);
  }

  // Merge instruction snapshots (v4+)
  u32 merged_snapshots = 0;
  if (header.version >= 4 && header.snapshot_section_offset > 0)
  {
    file.Seek(header.snapshot_section_offset, File::SeekOrigin::Begin);

    while (file.Tell() < file.GetSize())
    {
      u32 block_addr, num_snapshots;
      if (!file.ReadBytes(&block_addr, sizeof(u32)) ||
          !file.ReadBytes(&num_snapshots, sizeof(u32)))
      {
        break;
      }

      for (u32 si = 0; si < num_snapshots; si++)
      {
        u32 num_instructions, crc32;
        if (!file.ReadBytes(&num_instructions, sizeof(u32)) ||
            !file.ReadBytes(&crc32, sizeof(u32)))
        {
          break;
        }

        std::vector<u32> instructions(num_instructions);
        if (!file.ReadBytes(instructions.data(), num_instructions * sizeof(u32)))
          break;

        // Merge into existing block (create if needed)
        auto& block = m_blocks[block_addr];
        if (block.ppc_addr == 0)
        {
          block.ppc_addr = block_addr;
          block.block_size = num_instructions;
        }

        // Deduplicate by CRC32
        bool found = false;
        for (const auto& existing : block.snapshots)
        {
          if (existing.crc32 == crc32 && existing.instructions.size() == num_instructions)
          {
            found = true;
            break;
          }
        }
        if (!found)
        {
          InstructionSnapshot snap;
          snap.instructions = std::move(instructions);
          snap.crc32 = crc32;
          block.snapshots.push_back(std::move(snap));
          merged_snapshots++;
        }
      }
    }
  }

  INFO_LOG_FMT(DYNA_REC,
               "TraceCollector: Merged {} blocks, {} edges, {} SMC events, "
               "{} vertex formats, {} snapshots from {}",
               header.block_count, header.edge_count, header.smc_count, header.vtx_format_count,
               merged_snapshots, path);
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
