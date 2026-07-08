// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/CfgCommand.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <OptionParser.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <sqlite3.h>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Swap.h"

#include "Core/Boot/DolReader.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCTables.h"

#include "DolphinTool/OverlayImages.h"
#include "DolphinTool/PPCMemoryImage.h"
#include "DolphinTool/RelModules.h"

#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"

namespace DolphinTool
{

// ============================================================================
// Trace data reader
// ============================================================================

struct TraceBlock
{
  u32 addr;
  u32 size;
};
struct TraceEdge
{
  u32 from;
  u32 to;
  u8 type;  // 0=static, 1=dynamic, 2=call
};
struct TraceSMC
{
  u32 addr;
  u32 length;
};

struct TraceData
{
  std::vector<TraceBlock> blocks;
  std::vector<TraceEdge> edges;
  std::vector<TraceSMC> smc_regions;
  std::set<u32> seed_addresses;
  // Statically-derived seeds (e.g. DOL code referenced by REL module relocations).
  // Worklist-seeded like trace blocks but never marked as trace-hot.
  std::set<u32> static_seeds;
  std::unordered_multimap<u32, u32> dynamic_targets;  // from_block -> to_addr

  // Vertex format configurations recorded during tracing (v3+)
  struct VertexFormat
  {
    u32 vtx_desc_low, vtx_desc_high, vat_g0, vat_g1, vat_g2;
  };
  std::vector<VertexFormat> vertex_formats;

  // Instruction snapshots + counter-stamped invalidation events (v4+), consumed by the
  // overlay-image clustering.
  std::vector<TraceSnapshotBlock> snapshot_blocks;
  std::vector<TraceSmcEvent> smc_events;
};

static bool ReadTraceFile(const std::string& path, TraceData& out)
{
  File::IOFile file(path, "rb");
  if (!file.IsOpen())
  {
    fmt::println(std::cerr, "Error: Cannot open trace file: {}", path);
    return false;
  }

  // Read the common header fields first (v2 has 5 fields, v3 has 6, v4 has 7)
  u32 magic, version, block_count, edge_count, smc_count;
  if (!file.ReadBytes(&magic, 4) || !file.ReadBytes(&version, 4) ||
      !file.ReadBytes(&block_count, 4) || !file.ReadBytes(&edge_count, 4) ||
      !file.ReadBytes(&smc_count, 4))
    return false;

  if (magic != 0x54485044 || version < 2 || version > 4)
  {
    fmt::println(std::cerr, "Error: Invalid trace file (magic={:#x}, version={})", magic, version);
    return false;
  }

  u32 vtx_format_count = 0;
  if (version >= 3)
  {
    if (!file.ReadBytes(&vtx_format_count, 4))
      return false;
  }

  u32 snapshot_section_offset = 0;
  if (version >= 4)
  {
    if (!file.ReadBytes(&snapshot_section_offset, 4))
      return false;
  }

  out.blocks.resize(block_count);
  for (u32 i = 0; i < block_count; i++)
  {
    if (!file.ReadBytes(&out.blocks[i].addr, 4) || !file.ReadBytes(&out.blocks[i].size, 4))
      return false;
    out.seed_addresses.insert(out.blocks[i].addr);
  }

  out.edges.resize(edge_count);
  for (u32 i = 0; i < edge_count; i++)
  {
    u8 pad[3];
    if (!file.ReadBytes(&out.edges[i].from, 4) || !file.ReadBytes(&out.edges[i].to, 4) ||
        !file.ReadBytes(&out.edges[i].type, 1) || !file.ReadBytes(pad, 3))
      return false;
    if (out.edges[i].type == 1)  // dynamic
      out.dynamic_targets.emplace(out.edges[i].from, out.edges[i].to);
  }

  for (u32 i = 0; i < smc_count; i++)
  {
    TraceSMC smc{};
    u64 counter;
    if (!file.ReadBytes(&smc.addr, 4) || !file.ReadBytes(&smc.length, 4) ||
        !file.ReadBytes(&counter, 8))
      return false;
    out.smc_regions.push_back(smc);
    out.smc_events.push_back({smc.addr, smc.length, counter});
  }

  // Read vertex format records (v3+)
  out.vertex_formats.resize(vtx_format_count);
  for (u32 i = 0; i < vtx_format_count; i++)
  {
    auto& fmt = out.vertex_formats[i];
    if (!file.ReadBytes(&fmt.vtx_desc_low, 4) || !file.ReadBytes(&fmt.vtx_desc_high, 4) ||
        !file.ReadBytes(&fmt.vat_g0, 4) || !file.ReadBytes(&fmt.vat_g1, 4) ||
        !file.ReadBytes(&fmt.vat_g2, 4))
      return false;
  }

  // Read instruction snapshots (v4+)
  if (version >= 4 && snapshot_section_offset > 0)
  {
    file.Seek(snapshot_section_offset, File::SeekOrigin::Begin);
    const u64 file_size = file.GetSize();
    while (file.Tell() < file_size)
    {
      TraceSnapshotBlock sb{};
      u32 num_snapshots;
      if (!file.ReadBytes(&sb.addr, 4) || !file.ReadBytes(&num_snapshots, 4))
        break;
      for (u32 si = 0; si < num_snapshots; si++)
      {
        TraceSnapshot snap{};
        snap.addr = sb.addr;
        u32 num_instructions, num_epochs;
        if (!file.ReadBytes(&num_instructions, 4) || !file.ReadBytes(&snap.crc32, 4) ||
            !file.ReadBytes(&num_epochs, 4))
          return false;
        snap.epochs.resize(num_epochs);
        if (!file.ReadBytes(snap.epochs.data(), num_epochs * sizeof(u64)))
          return false;
        snap.words.resize(num_instructions);
        if (!file.ReadBytes(snap.words.data(), num_instructions * sizeof(u32)))
          return false;
        sb.snapshots.push_back(std::move(snap));
      }
      out.snapshot_blocks.push_back(std::move(sb));
    }
  }

  return true;
}

// ============================================================================
// CFG data structures
// ============================================================================

struct CFGBlock
{
  u32 start_addr;
  u32 end_addr;  // address of last instruction (inclusive)
  u32 num_instructions;
  bool from_trace;
  bool has_indirect_exit;
};

struct CFGEdge
{
  u32 from_block;  // start addr of source block
  u32 to_addr;     // target address
  enum Type
  {
    Static,
    Dynamic,
    Call,
    FallThrough
  };
  Type type;
  bool from_trace;
};

struct CFGFunction
{
  u32 entry_addr;
  std::string name;
  std::set<u32> block_addrs;
};

// ============================================================================
// Recursive descent disassembler
// ============================================================================

static u32 ComputeBranchTarget(UGeckoInstruction inst, u32 pc)
{
  if (inst.OPCD == 18)
  {
    u32 target = static_cast<u32>(SignExt26(inst.LI << 2));
    if (!inst.AA)
      target += pc;
    return target;
  }
  if (inst.OPCD == 16)
  {
    u32 target = static_cast<u32>(SignExt16(static_cast<s16>(inst.BD << 2)));
    if (!inst.AA)
      target += pc;
    return target;
  }
  return 0;
}

static bool IsBranchUnconditional(UGeckoInstruction inst)
{
  return (inst.BO & BO_DONT_DECREMENT_FLAG) && (inst.BO & BO_DONT_CHECK_CONDITION);
}

// In relocatable modules, branch instructions at relocation sites carry a
// meaningless encoded displacement (the field is patched at load time) — the
// real target comes from the relocation table. Sites mapping to
// RELOC_TARGET_EXTERNAL branch out of the address space being disassembled
// (another module, or the DOL).
constexpr u32 RELOC_TARGET_EXTERNAL = 0xFFFFFFFF;
using RelocBranchTargets = std::unordered_map<u32, u32>;  // site addr -> local target

static void RunDisassembly(const PPCMemoryImage& memory, const TraceData& trace,
                           std::map<u32, CFGBlock>& blocks, std::vector<CFGEdge>& edges,
                           bool verbose, const RelocBranchTargets* reloc_branches = nullptr)
{
  std::set<u32> worklist;
  std::set<u32> visited;

  // Seed with all trace block addresses
  for (u32 addr : trace.seed_addresses)
    worklist.insert(addr);

  // Statically-derived seeds (relocation targets, module entry points)
  for (u32 addr : trace.static_seeds)
    worklist.insert(addr);

  // Also seed with all dynamic branch targets
  for (const auto& [from, to] : trace.dynamic_targets)
    worklist.insert(to);

  // Also seed with all trace edge targets
  for (const auto& edge : trace.edges)
    worklist.insert(edge.to);

  u32 initial_seeds = static_cast<u32>(worklist.size());

  while (!worklist.empty())
  {
    u32 addr = *worklist.begin();
    worklist.erase(worklist.begin());

    if (visited.contains(addr))
      continue;
    visited.insert(addr);

    if (!memory.IsCodeAddress(addr))
      continue;

    // Check if this address is already inside an existing block (not at start)
    // If so, we need to split that block
    for (auto it = blocks.begin(); it != blocks.end(); ++it)
    {
      auto& existing = it->second;
      if (addr > existing.start_addr && addr <= existing.end_addr)
      {
        // Split: truncate existing block to end before addr
        u32 old_end = existing.end_addr;
        u32 old_num = existing.num_instructions;
        bool old_indirect = existing.has_indirect_exit;

        existing.end_addr = addr - 4;
        existing.num_instructions = (existing.end_addr - existing.start_addr) / 4 + 1;
        existing.has_indirect_exit = false;

        // Create new block for the split-off portion
        CFGBlock split_block{};
        split_block.start_addr = addr;
        split_block.end_addr = old_end;
        split_block.num_instructions = old_num - existing.num_instructions;
        split_block.from_trace = existing.from_trace;
        split_block.has_indirect_exit = old_indirect;
        blocks[addr] = split_block;

        // Move old edges from truncated block to split block — the branch
        // instruction that generated them is now in the split-off portion.
        for (auto& e : edges)
        {
          if (e.from_block == existing.start_addr)
            e.from_block = addr;
        }

        // Add fall-through edge from truncated block to split block
        edges.push_back({existing.start_addr, addr, CFGEdge::FallThrough, false});
        break;
      }
    }

    if (blocks.contains(addr))
      continue;  // already have a block at this exact address

    u32 block_start = addr;
    u32 num_instructions = 0;
    bool from_trace = trace.seed_addresses.contains(addr);
    u32 current = addr;

    while (true)
    {
      auto inst_word = memory.ReadInstruction(current);
      if (!inst_word)
        break;

      UGeckoInstruction inst(*inst_word);

      if (!PPCTables::IsValidInstruction(inst, current))
        break;

      const GekkoOPInfo* info = PPCTables::GetOpInfo(inst, current);
      num_instructions++;

      // Check if we're about to run into an existing block
      if (current != block_start && blocks.contains(current))
      {
        // End current block before this address, add fall-through
        CFGBlock block{};
        block.start_addr = block_start;
        block.end_addr = current - 4;
        block.num_instructions = num_instructions - 1;
        block.from_trace = from_trace;
        block.has_indirect_exit = false;
        if (block.num_instructions > 0)
        {
          blocks[block_start] = block;
          edges.push_back({block_start, current, CFGEdge::FallThrough, false});
        }
        break;
      }

      if (info->flags & FL_ENDBLOCK)
      {
        CFGBlock block{};
        block.start_addr = block_start;
        block.end_addr = current;
        block.num_instructions = num_instructions;
        block.from_trace = from_trace;
        block.has_indirect_exit = false;
        blocks[block_start] = block;

        // At a relocated branch site the encoded displacement is meaningless;
        // resolve the target from the relocation table instead (or mark it
        // external and only keep the local control-flow consequences).
        std::optional<u32> reloc_target;
        if (reloc_branches)
        {
          auto it = reloc_branches->find(current);
          if (it != reloc_branches->end())
            reloc_target = it->second;
        }

        if (inst.OPCD == 18)  // b / bl
        {
          u32 target = reloc_target ? *reloc_target : ComputeBranchTarget(inst, current);
          if (inst.LK)
          {
            if (target != RELOC_TARGET_EXTERNAL)
            {
              edges.push_back({block_start, target, CFGEdge::Call, false});
              worklist.insert(target);
            }
            worklist.insert(current + 4);
          }
          else if (target != RELOC_TARGET_EXTERNAL)
          {
            edges.push_back({block_start, target, CFGEdge::Static, false});
            worklist.insert(target);
          }
        }
        else if (inst.OPCD == 16)  // bc / bcl
        {
          u32 target = reloc_target ? *reloc_target : ComputeBranchTarget(inst, current);
          if (target != RELOC_TARGET_EXTERNAL)
          {
            edges.push_back({block_start, target, CFGEdge::Static, false});
            worklist.insert(target);
          }

          if (!IsBranchUnconditional(inst))
          {
            edges.push_back({block_start, current + 4, CFGEdge::FallThrough, false});
            worklist.insert(current + 4);
          }

          if (inst.LK)
            worklist.insert(current + 4);
        }
        else if (inst.OPCD == 19)
        {
          block.has_indirect_exit = true;
          blocks[block_start] = block;  // update

          if (inst.SUBOP10 == 528)  // bcctr
          {
            // Use dynamic targets from trace
            auto range = trace.dynamic_targets.equal_range(block_start);
            for (auto it = range.first; it != range.second; ++it)
            {
              edges.push_back({block_start, it->second, CFGEdge::Dynamic, true});
              worklist.insert(it->second);
            }

            if (!IsBranchUnconditional(inst))
            {
              edges.push_back({block_start, current + 4, CFGEdge::FallThrough, false});
              worklist.insert(current + 4);
            }
          }
          else if (inst.SUBOP10 == 16)  // bclr / blr
          {
            // blr = function return, no static target
            if (!IsBranchUnconditional(inst))
            {
              edges.push_back({block_start, current + 4, CFGEdge::FallThrough, false});
              worklist.insert(current + 4);
            }
          }
          else
          {
            // Other OPCD=19 endblock (e.g., rfi)
            worklist.insert(current + 4);
          }
        }
        else
        {
          // Other endblock instructions (sc, tw, etc.)
          worklist.insert(current + 4);
        }
        break;
      }

      current += 4;

      // Safety: don't let a single block grow unreasonably large
      if (num_instructions > 10000)
        break;
    }

    // If we fell through without finding an endblock, record what we have
    if (!blocks.contains(block_start) && num_instructions > 0)
    {
      CFGBlock block{};
      block.start_addr = block_start;
      block.end_addr = current;
      block.num_instructions = num_instructions;
      block.from_trace = from_trace;
      block.has_indirect_exit = false;
      blocks[block_start] = block;
    }
  }

  if (verbose)
  {
    fmt::println(std::cerr, "Disassembly: {} initial seeds -> {} blocks, {} edges",
                 initial_seeds, blocks.size(), edges.size());
  }
}

// ============================================================================
// Function identification
// ============================================================================

static std::map<u32, CFGFunction>
IdentifyFunctions(const std::map<u32, CFGBlock>& blocks, const std::vector<CFGEdge>& edges,
                  u32 entry_point)
{
  // Collect function entry points: targets of Call edges + DOL entry
  std::set<u32> entries;
  entries.insert(entry_point);
  for (const auto& edge : edges)
  {
    if (edge.type == CFGEdge::Call && blocks.contains(edge.to_addr))
      entries.insert(edge.to_addr);
  }

  // Build adjacency list (excluding Call edges)
  std::unordered_multimap<u32, u32> adj;
  for (const auto& edge : edges)
  {
    if (edge.type != CFGEdge::Call && blocks.contains(edge.from_block) &&
        blocks.contains(edge.to_addr))
    {
      adj.emplace(edge.from_block, edge.to_addr);
    }
  }

  // BFS from each function entry to assign blocks
  std::map<u32, CFGFunction> functions;
  std::unordered_map<u32, u32> block_to_function;

  for (u32 entry : entries)
  {
    if (!blocks.contains(entry))
      continue;

    CFGFunction func{};
    func.entry_addr = entry;
    func.name = fmt::format("sub_{:08X}", entry);

    std::queue<u32> queue;
    queue.push(entry);

    while (!queue.empty())
    {
      u32 addr = queue.front();
      queue.pop();

      if (block_to_function.contains(addr))
        continue;

      block_to_function[addr] = entry;
      func.block_addrs.insert(addr);

      auto range = adj.equal_range(addr);
      for (auto it = range.first; it != range.second; ++it)
      {
        if (!block_to_function.contains(it->second) && !entries.contains(it->second))
          queue.push(it->second);
      }
    }

    functions[entry] = std::move(func);
  }

  // Assign orphan blocks (not reachable from any function entry) to nearest function
  for (const auto& [addr, block] : blocks)
  {
    if (!block_to_function.contains(addr))
    {
      // Find nearest function entry before this block
      auto it = entries.upper_bound(addr);
      if (it != entries.begin())
      {
        --it;
        if (functions.contains(*it))
        {
          functions[*it].block_addrs.insert(addr);
          block_to_function[addr] = *it;
        }
      }
    }
  }

  return functions;
}

// ============================================================================
// REL module CFGs (section-relative, fully static — no trace input needed)
// ============================================================================

// Synthetic flat address for (section, offset) so the existing disassembly
// machinery can run unchanged on module code. Section sizes are far below 16MB.
static constexpr u32 SectAddr(u32 section, u32 offset)
{
  return (section << 24) | offset;
}
static constexpr u32 SectOf(u32 synth)
{
  return synth >> 24;
}
static constexpr u32 OffsetOf(u32 synth)
{
  return synth & 0x00FFFFFF;
}

static bool IsBranchRelocType(u8 type)
{
  switch (type)
  {
  case R_PPC_ADDR24:
  case R_PPC_ADDR14:
  case R_PPC_ADDR14_BRTAKEN:
  case R_PPC_ADDR14_BRNTAKEN:
  case R_PPC_REL24:
  case R_PPC_REL14:
  case R_PPC_REL14_BRTAKEN:
  case R_PPC_REL14_BRNTAKEN:
    return true;
  default:
    return false;
  }
}

struct ModuleCfg
{
  std::map<u32, CFGBlock> blocks;  // keyed by synthetic SectAddr
  std::vector<CFGEdge> edges;
  u32 executable_bytes = 0;
  u32 covered_bytes = 0;
};

// Every cross-reference into a module's code requires a relocation, so the
// relocation tables of ALL modules collectively mark nearly every code entry
// point (actor profile tables, vtables, call sites). Collect those seeds, plus
// DOL-code targets which feed the main DOL disassembly as extra static seeds.
static void CollectRelocationSeeds(const std::vector<RelFile>& modules,
                                   std::unordered_map<u32, std::set<u32>>& module_seeds,
                                   std::set<u32>& dol_seeds)
{
  for (const auto& m : modules)
  {
    for (const auto& r : m.relocs)
    {
      if ((r.addend & 3) != 0)
        continue;  // unaligned: a data reference, not a code entry
      if (r.target_module == 0)
        dol_seeds.insert(r.addend);
      else
        module_seeds[r.target_module].insert(SectAddr(r.target_section, r.addend));
    }
  }
}

static ModuleCfg BuildModuleCfg(const RelFile& rel, const std::set<u32>& reloc_seeds)
{
  ModuleCfg cfg;

  // Only executable sections enter the image: seeds landing elsewhere are
  // data references and get dropped by IsCodeAddress.
  PPCMemoryImage memory;
  for (size_t i = 0; i < rel.sections.size(); i++)
  {
    const auto& sec = rel.sections[i];
    if (sec.executable && !sec.data.empty())
    {
      memory.AddSection(SectAddr(static_cast<u32>(i), 0), sec.data.data(), sec.size);
      cfg.executable_bytes += sec.size;
    }
  }

  TraceData synth;
  synth.static_seeds = reloc_seeds;
  for (u32 entry : {SectAddr(rel.prolog_section, rel.prolog_offset),
                    SectAddr(rel.epilog_section, rel.epilog_offset),
                    SectAddr(rel.unresolved_section, rel.unresolved_offset)})
  {
    synth.static_seeds.insert(entry);
  }

  // Branch-site relocation map: sites whose target lives in this same module
  // resolve locally; everything else is external (followed at runtime via the
  // module base registry, and recorded in module_relocs for the translator).
  RelocBranchTargets branch_targets;
  for (const auto& r : rel.relocs)
  {
    if (!IsBranchRelocType(r.type))
      continue;
    if (!rel.sections[r.site_section].executable)
      continue;
    u32 local = RELOC_TARGET_EXTERNAL;
    if (r.target_module == rel.module_id && (r.addend & 3) == 0 &&
        r.target_section < rel.sections.size() && rel.sections[r.target_section].executable)
    {
      local = SectAddr(r.target_section, r.addend);
    }
    branch_targets[SectAddr(r.site_section, r.site_offset)] = local;
  }

  RunDisassembly(memory, synth, cfg.blocks, cfg.edges, false, &branch_targets);

  for (const auto& [addr, block] : cfg.blocks)
    cfg.covered_bytes += block.num_instructions * 4;
  return cfg;
}

static bool WriteModuleDatabase(const std::string& path, const std::vector<RelFile>& modules,
                                const std::map<u32, ModuleCfg>& cfgs)
{
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
  {
    fmt::println(std::cerr, "Error: Cannot open database: {}", path);
    return false;
  }

  const char* schema = R"(
    CREATE TABLE IF NOT EXISTS modules (
      module_id INTEGER PRIMARY KEY,
      dense_idx INTEGER NOT NULL,
      name TEXT NOT NULL,
      version INTEGER NOT NULL,
      num_sections INTEGER NOT NULL,
      bss_size INTEGER NOT NULL,
      prolog_section INTEGER, prolog_offset INTEGER,
      epilog_section INTEGER, epilog_offset INTEGER,
      unresolved_section INTEGER, unresolved_offset INTEGER
    );
    CREATE TABLE IF NOT EXISTS module_sections (
      module_id INTEGER NOT NULL,
      section_idx INTEGER NOT NULL,
      size INTEGER NOT NULL,
      executable INTEGER NOT NULL,
      is_bss INTEGER NOT NULL,
      PRIMARY KEY (module_id, section_idx)
    );
    CREATE TABLE IF NOT EXISTS module_blocks (
      module_id INTEGER NOT NULL,
      section_idx INTEGER NOT NULL,
      offset INTEGER NOT NULL,
      num_instructions INTEGER NOT NULL,
      is_translatable INTEGER NOT NULL DEFAULT 1,
      PRIMARY KEY (module_id, section_idx, offset)
    );
    CREATE TABLE IF NOT EXISTS module_edges (
      module_id INTEGER NOT NULL,
      from_section INTEGER NOT NULL,
      from_offset INTEGER NOT NULL,
      to_section INTEGER NOT NULL,
      to_offset INTEGER NOT NULL,
      edge_type TEXT NOT NULL,
      PRIMARY KEY (module_id, from_section, from_offset, to_section, to_offset, edge_type)
    );
    CREATE TABLE IF NOT EXISTS module_relocs (
      module_id INTEGER NOT NULL,
      site_section INTEGER NOT NULL,
      site_offset INTEGER NOT NULL,
      type INTEGER NOT NULL,
      target_module INTEGER NOT NULL,
      target_section INTEGER NOT NULL,
      addend INTEGER NOT NULL,
      PRIMARY KEY (module_id, site_section, site_offset)
    );
    CREATE TABLE IF NOT EXISTS metadata (
      key TEXT PRIMARY KEY,
      value TEXT
    );
  )";

  char* err = nullptr;
  sqlite3_exec(db, schema, nullptr, nullptr, &err);
  if (err)
  {
    fmt::println(std::cerr, "SQL error: {}", err);
    sqlite3_free(err);
    sqlite3_close(db);
    return false;
  }

  sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
  sqlite3_stmt* stmt = nullptr;

  sqlite3_prepare_v2(db,
                     "INSERT OR REPLACE INTO modules VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                     -1, &stmt, nullptr);
  // modules arrive sorted by module_id (sorted at discovery time); dense_idx
  // is the stable per-game codegen index.
  for (size_t dense = 0; dense < modules.size(); dense++)
  {
    const auto& m = modules[dense];
    sqlite3_bind_int64(stmt, 1, m.module_id);
    sqlite3_bind_int(stmt, 2, static_cast<int>(dense));
    sqlite3_bind_text(stmt, 3, m.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, m.version);
    sqlite3_bind_int(stmt, 5, static_cast<int>(m.sections.size()));
    sqlite3_bind_int64(stmt, 6, m.bss_size);
    sqlite3_bind_int(stmt, 7, m.prolog_section);
    sqlite3_bind_int64(stmt, 8, m.prolog_offset);
    sqlite3_bind_int(stmt, 9, m.epilog_section);
    sqlite3_bind_int64(stmt, 10, m.epilog_offset);
    sqlite3_bind_int(stmt, 11, m.unresolved_section);
    sqlite3_bind_int64(stmt, 12, m.unresolved_offset);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO module_sections VALUES (?, ?, ?, ?, ?)", -1,
                     &stmt, nullptr);
  for (const auto& m : modules)
  {
    for (size_t i = 0; i < m.sections.size(); i++)
    {
      const auto& sec = m.sections[i];
      sqlite3_bind_int64(stmt, 1, m.module_id);
      sqlite3_bind_int(stmt, 2, static_cast<int>(i));
      sqlite3_bind_int64(stmt, 3, sec.size);
      sqlite3_bind_int(stmt, 4, sec.executable ? 1 : 0);
      sqlite3_bind_int(stmt, 5, sec.IsBss() ? 1 : 0);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
    }
  }
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO module_blocks VALUES (?, ?, ?, ?, ?)", -1, &stmt,
                     nullptr);
  for (const auto& m : modules)
  {
    auto it = cfgs.find(m.module_id);
    if (it == cfgs.end())
      continue;
    for (const auto& [addr, block] : it->second.blocks)
    {
      sqlite3_bind_int64(stmt, 1, m.module_id);
      sqlite3_bind_int(stmt, 2, SectOf(addr));
      sqlite3_bind_int64(stmt, 3, OffsetOf(addr));
      sqlite3_bind_int(stmt, 4, block.num_instructions);
      sqlite3_bind_int(stmt, 5, 1);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
    }
  }
  sqlite3_finalize(stmt);

  static const char* edge_type_names[] = {"static", "dynamic", "call", "fallthrough"};
  sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO module_edges VALUES (?, ?, ?, ?, ?, ?)", -1, &stmt,
                     nullptr);
  for (const auto& m : modules)
  {
    auto it = cfgs.find(m.module_id);
    if (it == cfgs.end())
      continue;
    for (const auto& e : it->second.edges)
    {
      sqlite3_bind_int64(stmt, 1, m.module_id);
      sqlite3_bind_int(stmt, 2, SectOf(e.from_block));
      sqlite3_bind_int64(stmt, 3, OffsetOf(e.from_block));
      sqlite3_bind_int(stmt, 4, SectOf(e.to_addr));
      sqlite3_bind_int64(stmt, 5, OffsetOf(e.to_addr));
      sqlite3_bind_text(stmt, 6, edge_type_names[e.type], -1, SQLITE_STATIC);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
    }
  }
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO module_relocs VALUES (?, ?, ?, ?, ?, ?, ?)", -1,
                     &stmt, nullptr);
  for (const auto& m : modules)
  {
    for (const auto& r : m.relocs)
    {
      // Only executable-section sites matter for translation; data sections are
      // relocated in RAM by the game's own OSLink running under emulation.
      if (!m.sections[r.site_section].executable)
        continue;
      sqlite3_bind_int64(stmt, 1, m.module_id);
      sqlite3_bind_int(stmt, 2, r.site_section);
      sqlite3_bind_int64(stmt, 3, r.site_offset);
      sqlite3_bind_int(stmt, 4, r.type);
      sqlite3_bind_int64(stmt, 5, r.target_module);
      sqlite3_bind_int(stmt, 6, r.target_section);
      sqlite3_bind_int64(stmt, 7, r.addend);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
    }
  }
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO metadata VALUES (?, ?)", -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, "module_count", -1, SQLITE_STATIC);
  const std::string count = fmt::format("{}", modules.size());
  sqlite3_bind_text(stmt, 2, count.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  return true;
}

// ============================================================================
// Overlay-image CFGs (trace-captured code; absolute fixed addresses)
// ============================================================================

struct OverlayCfg
{
  std::map<u32, CFGBlock> blocks;  // keyed by absolute PPC address
  u32 captured_bytes = 0;
  u32 covered_bytes = 0;
};

// Overlay bytes are already-linked code at a fixed base: absolute addressing, ordinary
// branch decoding, no relocation map. Descent is bounded to the captured ranges by
// PPCMemoryImage::IsCodeAddress, so untraced neighborhoods simply stay uncovered.
static OverlayCfg BuildOverlayCfg(const OverlayImage& image, const TraceData& trace)
{
  OverlayCfg cfg;

  PPCMemoryImage memory;
  for (const auto& range : image.ranges)
  {
    memory.AddSection(range.addr, range.bytes.data(), static_cast<u32>(range.bytes.size()));
    cfg.captured_bytes += static_cast<u32>(range.bytes.size());
  }

  TraceData synth;
  synth.seed_addresses = image.member_blocks;
  for (const auto& edge : trace.edges)
  {
    if (edge.to >= image.base && edge.to < image.end)
      synth.edges.push_back(edge);
    if (edge.type == 1 && edge.from >= image.base && edge.from < image.end)
      synth.dynamic_targets.emplace(edge.from, edge.to);
  }

  std::vector<CFGEdge> edges;  // decoded again at translate time; not persisted
  RunDisassembly(memory, synth, cfg.blocks, edges, false);

  for (const auto& [addr, block] : cfg.blocks)
    cfg.covered_bytes += block.num_instructions * 4;
  return cfg;
}

static bool WriteOverlayDatabase(const std::string& path,
                                 const std::vector<OverlayImage>& images,
                                 const std::vector<OverlayCfg>& cfgs, u32 gap_threshold)
{
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
  {
    fmt::println(std::cerr, "Error: Cannot open database: {}", path);
    return false;
  }

  const char* schema = R"(
    CREATE TABLE IF NOT EXISTS overlay_images (
      image_id INTEGER PRIMARY KEY,
      base INTEGER NOT NULL,
      end INTEGER NOT NULL,
      full_crc32 INTEGER NOT NULL,
      prefix_crc32 INTEGER NOT NULL,
      num_ranges INTEGER NOT NULL,
      captured_bytes INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS overlay_ranges (
      image_id INTEGER NOT NULL,
      addr INTEGER NOT NULL,
      size INTEGER NOT NULL,
      bytes BLOB NOT NULL,
      PRIMARY KEY (image_id, addr)
    );
    CREATE TABLE IF NOT EXISTS overlay_blocks (
      image_id INTEGER NOT NULL,
      addr INTEGER NOT NULL,
      num_instructions INTEGER NOT NULL,
      is_translatable INTEGER NOT NULL DEFAULT 1,
      PRIMARY KEY (image_id, addr)
    );
  )";

  char* err = nullptr;
  sqlite3_exec(db, schema, nullptr, nullptr, &err);
  if (err)
  {
    fmt::println(std::cerr, "SQL error: {}", err);
    sqlite3_free(err);
    sqlite3_close(db);
    return false;
  }

  sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
  // Regenerating from a merged trace can reshape images entirely; replace wholesale.
  sqlite3_exec(db, "DELETE FROM overlay_images; DELETE FROM overlay_ranges; "
                   "DELETE FROM overlay_blocks;",
               nullptr, nullptr, nullptr);

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db, "INSERT INTO overlay_images VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &stmt,
                     nullptr);
  for (size_t i = 0; i < images.size(); i++)
  {
    const auto& img = images[i];
    u32 captured = 0;
    for (const auto& r : img.ranges)
      captured += static_cast<u32>(r.bytes.size());
    sqlite3_bind_int(stmt, 1, static_cast<int>(i));
    sqlite3_bind_int64(stmt, 2, img.base);
    sqlite3_bind_int64(stmt, 3, img.end);
    sqlite3_bind_int64(stmt, 4, img.full_crc32);
    sqlite3_bind_int64(stmt, 5, img.prefix_crc32);
    sqlite3_bind_int(stmt, 6, static_cast<int>(img.ranges.size()));
    sqlite3_bind_int64(stmt, 7, captured);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db, "INSERT INTO overlay_ranges VALUES (?, ?, ?, ?)", -1, &stmt, nullptr);
  for (size_t i = 0; i < images.size(); i++)
  {
    for (const auto& r : images[i].ranges)
    {
      sqlite3_bind_int(stmt, 1, static_cast<int>(i));
      sqlite3_bind_int64(stmt, 2, r.addr);
      sqlite3_bind_int64(stmt, 3, static_cast<s64>(r.bytes.size()));
      sqlite3_bind_blob(stmt, 4, r.bytes.data(), static_cast<int>(r.bytes.size()),
                        SQLITE_STATIC);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
    }
  }
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db, "INSERT INTO overlay_blocks VALUES (?, ?, ?, ?)", -1, &stmt, nullptr);
  for (size_t i = 0; i < images.size(); i++)
  {
    for (const auto& [addr, block] : cfgs[i].blocks)
    {
      sqlite3_bind_int(stmt, 1, static_cast<int>(i));
      sqlite3_bind_int64(stmt, 2, addr);
      sqlite3_bind_int(stmt, 3, block.num_instructions);
      sqlite3_bind_int(stmt, 4, 1);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
    }
  }
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO metadata VALUES (?, ?)", -1, &stmt, nullptr);
  auto insert_meta = [&](const char* key, const std::string& value) {
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  };
  insert_meta("overlay_image_count", fmt::format("{}", images.size()));
  insert_meta("overlay_gap", fmt::format("{}", gap_threshold));
  sqlite3_finalize(stmt);

  sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  return true;
}

// ============================================================================
// SQLite output
// ============================================================================

static bool WriteCFGDatabase(const std::string& path, const std::map<u32, CFGBlock>& blocks,
                             const std::vector<CFGEdge>& edges,
                             const std::map<u32, CFGFunction>& functions,
                             const std::vector<TraceSMC>& smc_regions,
                             const std::vector<TraceData::VertexFormat>& vertex_formats,
                             const std::set<u32>& trace_block_addrs, u32 entry_point)
{
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
  {
    fmt::println(std::cerr, "Error: Cannot create database: {}", path);
    return false;
  }

  // Create tables
  const char* schema = R"(
    CREATE TABLE IF NOT EXISTS blocks (
      ppc_addr INTEGER PRIMARY KEY,
      size INTEGER NOT NULL,
      num_instructions INTEGER NOT NULL,
      function_addr INTEGER,
      is_translatable INTEGER NOT NULL DEFAULT 1,
      source TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS edges (
      from_addr INTEGER NOT NULL,
      to_addr INTEGER NOT NULL,
      edge_type TEXT NOT NULL,
      source TEXT NOT NULL,
      PRIMARY KEY (from_addr, to_addr, edge_type)
    );
    CREATE TABLE IF NOT EXISTS functions (
      entry_addr INTEGER PRIMARY KEY,
      name TEXT,
      size INTEGER,
      block_count INTEGER
    );
    CREATE TABLE IF NOT EXISTS smc_regions (
      addr INTEGER NOT NULL,
      length INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS metadata (
      key TEXT PRIMARY KEY,
      value TEXT
    );
    CREATE TABLE IF NOT EXISTS vertex_formats (
      vtx_desc_low INTEGER NOT NULL,
      vtx_desc_high INTEGER NOT NULL,
      vat_g0 INTEGER NOT NULL,
      vat_g1 INTEGER NOT NULL,
      vat_g2 INTEGER NOT NULL,
      PRIMARY KEY (vtx_desc_low, vtx_desc_high, vat_g0, vat_g1, vat_g2)
    );
    CREATE INDEX IF NOT EXISTS idx_edges_from ON edges(from_addr);
    CREATE INDEX IF NOT EXISTS idx_edges_to ON edges(to_addr);
    CREATE INDEX IF NOT EXISTS idx_blocks_func ON blocks(function_addr);
  )";

  char* err = nullptr;
  sqlite3_exec(db, schema, nullptr, nullptr, &err);
  if (err)
  {
    fmt::println(std::cerr, "SQL error: {}", err);
    sqlite3_free(err);
    sqlite3_close(db);
    return false;
  }

  sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

  // Build SMC address set for overlap checking
  auto is_in_smc_region = [&](u32 addr, u32 size) -> bool {
    for (const auto& smc : smc_regions)
    {
      if (addr < smc.addr + smc.length && addr + size > smc.addr)
        return true;
    }
    return false;
  };

  // Build function assignment map
  std::unordered_map<u32, u32> block_to_func;
  for (const auto& [entry, func] : functions)
  {
    for (u32 baddr : func.block_addrs)
      block_to_func[baddr] = entry;
  }

  // Insert blocks
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(
      db,
      "INSERT OR REPLACE INTO blocks VALUES (?, ?, ?, ?, ?, ?)",
      -1, &stmt, nullptr);

  for (const auto& [addr, block] : blocks)
  {
    u32 size_bytes = block.num_instructions * 4;
    bool translatable = !is_in_smc_region(addr, size_bytes);
    const char* source;
    if (block.from_trace && trace_block_addrs.contains(addr))
      source = "both";
    else if (trace_block_addrs.contains(addr))
      source = "trace";
    else
      source = "static";

    auto func_it = block_to_func.find(addr);

    sqlite3_bind_int64(stmt, 1, addr);
    sqlite3_bind_int(stmt, 2, size_bytes);
    sqlite3_bind_int(stmt, 3, block.num_instructions);
    if (func_it != block_to_func.end())
      sqlite3_bind_int64(stmt, 4, func_it->second);
    else
      sqlite3_bind_null(stmt, 4);
    sqlite3_bind_int(stmt, 5, translatable ? 1 : 0);
    sqlite3_bind_text(stmt, 6, source, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  // Insert edges
  static const char* edge_type_names[] = {"static", "dynamic", "call", "fallthrough"};

  sqlite3_prepare_v2(
      db,
      "INSERT OR IGNORE INTO edges VALUES (?, ?, ?, ?)",
      -1, &stmt, nullptr);

  for (const auto& edge : edges)
  {
    sqlite3_bind_int64(stmt, 1, edge.from_block);
    sqlite3_bind_int64(stmt, 2, edge.to_addr);
    sqlite3_bind_text(stmt, 3, edge_type_names[edge.type], -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, edge.from_trace ? "trace" : "static", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  // Insert functions
  sqlite3_prepare_v2(
      db,
      "INSERT OR REPLACE INTO functions VALUES (?, ?, ?, ?)",
      -1, &stmt, nullptr);

  for (const auto& [entry, func] : functions)
  {
    // Compute function size (span from first to last block)
    u32 func_size = 0;
    for (u32 baddr : func.block_addrs)
    {
      auto it = blocks.find(baddr);
      if (it != blocks.end())
        func_size += it->second.num_instructions * 4;
    }

    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_bind_text(stmt, 2, func.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, func_size);
    sqlite3_bind_int(stmt, 4, static_cast<int>(func.block_addrs.size()));
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  // Insert SMC regions
  sqlite3_prepare_v2(db, "INSERT INTO smc_regions VALUES (?, ?)", -1, &stmt, nullptr);
  for (const auto& smc : smc_regions)
  {
    sqlite3_bind_int64(stmt, 1, smc.addr);
    sqlite3_bind_int(stmt, 2, smc.length);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  // Insert vertex formats
  sqlite3_prepare_v2(
      db,
      "INSERT OR REPLACE INTO vertex_formats VALUES (?, ?, ?, ?, ?)",
      -1, &stmt, nullptr);
  for (const auto& fmt : vertex_formats)
  {
    sqlite3_bind_int64(stmt, 1, fmt.vtx_desc_low);
    sqlite3_bind_int64(stmt, 2, fmt.vtx_desc_high);
    sqlite3_bind_int64(stmt, 3, fmt.vat_g0);
    sqlite3_bind_int64(stmt, 4, fmt.vat_g1);
    sqlite3_bind_int64(stmt, 5, fmt.vat_g2);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  // Insert metadata
  sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO metadata VALUES (?, ?)", -1, &stmt, nullptr);

  auto insert_meta = [&](const char* key, const std::string& value) {
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  };

  u32 trace_and_static = 0, static_only_count = 0;
  for (const auto& [addr, block] : blocks)
  {
    if (trace_block_addrs.contains(addr))
      trace_and_static++;
    else
      static_only_count++;
  }

  insert_meta("dol_entry_point", fmt::format("{:#010x}", entry_point));
  insert_meta("trace_block_count", fmt::format("{}", trace_block_addrs.size()));
  insert_meta("static_only_block_count", fmt::format("{}", static_only_count));
  insert_meta("total_block_count", fmt::format("{}", blocks.size()));
  insert_meta("total_edge_count", fmt::format("{}", edges.size()));
  insert_meta("function_count", fmt::format("{}", functions.size()));
  insert_meta("vertex_format_count", fmt::format("{}", vertex_formats.size()));

  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  return true;
}

// ============================================================================
// Main command
// ============================================================================

int CfgCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: dolphin-tool cfg [options]");

  parser.add_option("-i", "--iso").action("store").help("Path to GameCube/Wii disc image");
  parser.add_option("-t", "--trace").action("store").help("Path to .dpht trace file");
  parser.add_option("-o", "--output").action("store").help("Path to output SQLite database");
  parser.add_option("-v", "--verbose").action("store_true").help("Print detailed progress");
  parser.add_option("--no-rels")
      .action("store_true")
      .help("Skip discovery and CFG extraction of .rel relocatable modules");
  parser.add_option("--overlay-gap")
      .action("store")
      .set_default("65536")
      .help("Max address gap (bytes) between co-resident snapshots clustered into one "
            "overlay image (default: %default)");
  parser.add_option("--overlay-max-versions")
      .action("store")
      .set_default("8")
      .help("Exclude arena addresses with more than this many content versions as "
            "genuine self-modifying code (default: %default)");
  parser.add_option("--no-overlays")
      .action("store_true")
      .help("Skip overlay-image clustering of v4 instruction snapshots");

  const optparse::Values options = parser.parse_args(args);

  if (!options.is_set("iso") || !options.is_set("trace") || !options.is_set("output"))
  {
    parser.print_help();
    return EXIT_FAILURE;
  }

  const std::string iso_path = options["iso"];
  const std::string trace_path = options["trace"];
  const std::string output_path = options["output"];
  const bool verbose = options.is_set("verbose");
  const bool no_rels = options.is_set("no_rels");
  const bool no_overlays = options.is_set("no_overlays");
  const u32 overlay_gap = static_cast<u32>(std::strtoul(options["overlay_gap"].c_str(), nullptr, 0));
  const u32 overlay_max_versions =
      static_cast<u32>(std::strtoul(options["overlay_max_versions"].c_str(), nullptr, 0));

  // 1. Open disc and extract DOL
  auto volume = DiscIO::CreateDisc(iso_path);
  if (!volume)
  {
    fmt::println(std::cerr, "Error: Cannot open disc image: {}", iso_path);
    return EXIT_FAILURE;
  }

  auto dol_offset = DiscIO::GetBootDOLOffset(*volume, DiscIO::PARTITION_NONE);
  if (!dol_offset)
  {
    fmt::println(std::cerr, "Error: Cannot find DOL in disc image");
    return EXIT_FAILURE;
  }

  auto dol_size = DiscIO::GetBootDOLSize(*volume, DiscIO::PARTITION_NONE, *dol_offset);
  if (!dol_size)
  {
    fmt::println(std::cerr, "Error: Cannot determine DOL size");
    return EXIT_FAILURE;
  }

  std::vector<u8> dol_buffer(*dol_size);
  if (!volume->Read(*dol_offset, *dol_size, dol_buffer.data(), DiscIO::PARTITION_NONE))
  {
    fmt::println(std::cerr, "Error: Cannot read DOL data");
    return EXIT_FAILURE;
  }

  DolReader dol(std::move(dol_buffer));
  if (!dol.IsValid())
  {
    fmt::println(std::cerr, "Error: Invalid DOL");
    return EXIT_FAILURE;
  }

  // Build memory image from text sections
  PPCMemoryImage memory;
  u32 total_code_bytes = 0;

  fmt::println(std::cerr, "DOL: entry point {:#010x}", dol.GetEntryPoint());
  for (int i = 0; i < dol.GetNumTextSections(); i++)
  {
    const auto& section = dol.GetTextSection(i);
    if (section.empty())
      continue;
    u32 addr = dol.GetTextSectionAddress(i);
    u32 size = dol.GetTextSectionSize(i);
    memory.AddSection(addr, section.data(), size);
    total_code_bytes += size;
    fmt::println(std::cerr, "  .text{}: {:#010x} - {:#010x} ({:.1f} KB)", i, addr, addr + size,
                 size / 1024.0);
  }

  // 2. Discover REL modules (purely static: every cross-reference into module
  // code requires a relocation, so module CFGs need no trace input — and the
  // DOL-code targets of module relocations seed the main pass below).
  std::vector<RelFile> modules;
  if (!no_rels)
    modules = DiscoverRelModules(*volume, verbose);
  std::sort(modules.begin(), modules.end(),
            [](const RelFile& a, const RelFile& b) { return a.module_id < b.module_id; });

  std::unordered_map<u32, std::set<u32>> module_seeds;
  std::set<u32> dol_seeds;
  CollectRelocationSeeds(modules, module_seeds, dol_seeds);

  if (!modules.empty())
  {
    u32 total_relocs = 0;
    for (const auto& m : modules)
      total_relocs += static_cast<u32>(m.relocs.size());
    fmt::println(std::cerr, "Modules: {} RELs, {} relocations, {} DOL seed addresses",
                 modules.size(), total_relocs, dol_seeds.size());
  }

  // 3. Read trace data
  TraceData trace;
  if (!ReadTraceFile(trace_path, trace))
    return EXIT_FAILURE;
  trace.static_seeds = std::move(dol_seeds);

  u32 dynamic_count = 0;
  for (const auto& e : trace.edges)
  {
    if (e.type == 1)
      dynamic_count++;
  }
  fmt::println(std::cerr, "Trace: {} seed blocks, {} edges ({} dynamic)", trace.blocks.size(),
               trace.edges.size(), dynamic_count);

  // 4. Run recursive descent disassembly
  std::map<u32, CFGBlock> blocks;
  std::vector<CFGEdge> edges;

  // Add DOL entry point as a seed
  trace.seed_addresses.insert(dol.GetEntryPoint());

  RunDisassembly(memory, trace, blocks, edges, verbose);

  // 5. Identify functions
  auto functions = IdentifyFunctions(blocks, edges, dol.GetEntryPoint());

  // 6. Compute statistics
  u32 trace_only = 0, static_only = 0, both_count = 0;
  for (const auto& [addr, block] : blocks)
  {
    if (trace.seed_addresses.contains(addr))
      both_count++;
    else
      static_only++;
  }
  trace_only = 0;
  for (u32 addr : trace.seed_addresses)
  {
    if (!blocks.contains(addr))
      trace_only++;
  }

  fmt::println(std::cerr, "Results:");
  fmt::println(std::cerr, "  Total blocks: {}", blocks.size());
  fmt::println(std::cerr, "  From trace + static: {}", both_count);
  fmt::println(std::cerr, "  Static-only (newly discovered): {}", static_only);
  fmt::println(std::cerr, "  Trace-only (outside DOL): {}", trace_only);
  fmt::println(std::cerr, "  Total edges: {}", edges.size());
  fmt::println(std::cerr, "  Functions identified: {}", functions.size());
  fmt::println(std::cerr, "  SMC regions: {}", trace.smc_regions.size());

  // Compute code coverage
  u32 covered_bytes = 0;
  for (const auto& [addr, block] : blocks)
    covered_bytes += block.num_instructions * 4;
  if (total_code_bytes > 0)
  {
    fmt::println(std::cerr, "  Code coverage: {:.1f}% of {:.1f} KB text sections",
                 100.0 * covered_bytes / total_code_bytes, total_code_bytes / 1024.0);
  }

  if (!trace.vertex_formats.empty())
  {
    fmt::println(std::cerr, "  Vertex formats: {}", trace.vertex_formats.size());
  }

  // 6. Per-module CFGs (static, relocation-seeded)
  std::map<u32, ModuleCfg> module_cfgs;
  if (!modules.empty())
  {
    u32 total_blocks = 0;
    u64 total_exec = 0, total_covered = 0;
    u32 worst_coverage_id = 0;
    double worst_coverage = 100.0;
    for (const auto& m : modules)
    {
      static const std::set<u32> no_seeds;
      auto seed_it = module_seeds.find(m.module_id);
      ModuleCfg cfg = BuildModuleCfg(m, seed_it != module_seeds.end() ? seed_it->second : no_seeds);
      total_blocks += static_cast<u32>(cfg.blocks.size());
      total_exec += cfg.executable_bytes;
      total_covered += cfg.covered_bytes;
      if (cfg.executable_bytes > 0)
      {
        const double pct = 100.0 * cfg.covered_bytes / cfg.executable_bytes;
        if (pct < worst_coverage)
        {
          worst_coverage = pct;
          worst_coverage_id = m.module_id;
        }
        if (verbose)
        {
          fmt::println(std::cerr, "  module {:3d} {}: {} blocks, {:.1f}% of {:.1f} KB",
                       m.module_id, m.name, cfg.blocks.size(), pct,
                       cfg.executable_bytes / 1024.0);
        }
      }
      module_cfgs.emplace(m.module_id, std::move(cfg));
    }
    fmt::println(std::cerr, "Module CFGs: {} blocks across {} modules", total_blocks,
                 modules.size());
    if (total_exec > 0)
    {
      fmt::println(std::cerr, "  Module code coverage: {:.1f}% of {:.1f} KB (worst: {:.1f}% in "
                              "module {})",
                   100.0 * total_covered / total_exec, total_exec / 1024.0, worst_coverage,
                   worst_coverage_id);
    }
  }

  // 7. Overlay images from v4 instruction snapshots: cluster the arena (non-DOL)
  // captures into content-hashed images and build a CFG per image.
  std::vector<OverlayImage> overlay_images;
  std::vector<OverlayCfg> overlay_cfgs;
  if (!no_overlays && !trace.snapshot_blocks.empty())
  {
    std::vector<TraceSnapshotBlock> arena_blocks;
    for (auto& sb : trace.snapshot_blocks)
    {
      if (!memory.IsCodeAddress(sb.addr))
        arena_blocks.push_back(std::move(sb));
    }

    if (!arena_blocks.empty())
    {
      OverlayBuildStats ostats{};
      overlay_images = BuildOverlayImages(arena_blocks, trace.smc_events, overlay_gap,
                                          overlay_max_versions, verbose, &ostats);

      u64 total_captured = 0, total_covered = 0, total_extent = 0, total_blocks = 0;
      for (const auto& img : overlay_images)
      {
        OverlayCfg cfg = BuildOverlayCfg(img, trace);
        total_captured += cfg.captured_bytes;
        total_covered += cfg.covered_bytes;
        total_extent += img.end - img.base;
        total_blocks += cfg.blocks.size();
        if (verbose)
        {
          fmt::println(std::cerr,
                       "  overlay {:3d}: {:#010x}-{:#010x} ({:5.1f} KB captured, {} ranges, "
                       "{} blocks, crc {:08x})",
                       overlay_cfgs.size(), img.base, img.end, cfg.captured_bytes / 1024.0,
                       img.ranges.size(), cfg.blocks.size(), img.full_crc32);
        }
        overlay_cfgs.push_back(std::move(cfg));
      }

      fmt::println(std::cerr, "Overlay images: {} images, {} blocks", overlay_images.size(),
                   total_blocks);
      fmt::println(std::cerr,
                   "  Arena snapshots: {} blocks, {} versions ({} hyper-mutable excluded)",
                   ostats.snapshot_blocks, ostats.snapshots, ostats.hyper_mutable_blocks);
      fmt::println(std::cerr,
                   "  Generation kills: {} by invalidation, {} by byte conflict "
                   "({} conflict-triggered captures — may be transitional mixes); "
                   "{} multi-base duplicates",
                   ostats.smc_kills, ostats.conflict_kills, ostats.conflict_captures,
                   ostats.multi_base_duplicates);
      fmt::println(std::cerr,
                   "  Captured {:.1f} KB, CFG-covered {:.1f} KB; dispatch-table forecast "
                   "{:.1f} MB (2 B per extent byte per image)",
                   total_captured / 1024.0, total_covered / 1024.0,
                   total_extent * 2.0 / (1024.0 * 1024.0));
    }
  }

  // 8. Write output
  if (!WriteCFGDatabase(output_path, blocks, edges, functions, trace.smc_regions,
                        trace.vertex_formats, trace.seed_addresses, dol.GetEntryPoint()))
  {
    return EXIT_FAILURE;
  }
  if (!modules.empty() && !WriteModuleDatabase(output_path, modules, module_cfgs))
    return EXIT_FAILURE;
  if (!overlay_images.empty() &&
      !WriteOverlayDatabase(output_path, overlay_images, overlay_cfgs, overlay_gap))
  {
    return EXIT_FAILURE;
  }

  fmt::println(std::cerr, "Output written to {}", output_path);
  return EXIT_SUCCESS;
}

}  // namespace DolphinTool
