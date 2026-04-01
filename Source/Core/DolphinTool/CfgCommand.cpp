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

#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"

namespace DolphinTool
{

// ============================================================================
// PPCMemoryImage: maps DOL text sections for instruction reads
// ============================================================================

class PPCMemoryImage
{
public:
  void AddSection(u32 virtual_addr, const u8* data, u32 size)
  {
    if (size == 0 || data == nullptr)
      return;
    m_sections.push_back({virtual_addr, size, data});
    std::sort(m_sections.begin(), m_sections.end(),
              [](const Section& a, const Section& b) { return a.base < b.base; });
  }

  std::optional<u32> ReadInstruction(u32 addr) const
  {
    for (const auto& sec : m_sections)
    {
      if (addr >= sec.base && addr < sec.base + sec.size)
      {
        u32 offset = addr - sec.base;
        if (offset + 4 > sec.size)
          return std::nullopt;
        u32 raw;
        std::memcpy(&raw, sec.data + offset, sizeof(u32));
        return Common::swap32(raw);
      }
    }
    return std::nullopt;
  }

  bool IsCodeAddress(u32 addr) const
  {
    for (const auto& sec : m_sections)
    {
      if (addr >= sec.base && addr < sec.base + sec.size)
        return true;
    }
    return false;
  }

private:
  struct Section
  {
    u32 base;
    u32 size;
    const u8* data;
  };
  std::vector<Section> m_sections;
};

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
  std::unordered_multimap<u32, u32> dynamic_targets;  // from_block -> to_addr
};

static bool ReadTraceFile(const std::string& path, TraceData& out)
{
  File::IOFile file(path, "rb");
  if (!file.IsOpen())
  {
    fmt::println(std::cerr, "Error: Cannot open trace file: {}", path);
    return false;
  }

  struct
  {
    u32 magic, version, block_count, edge_count, smc_count;
  } header{};
  if (!file.ReadBytes(&header, sizeof(header)))
    return false;

  if (header.magic != 0x54485044 || header.version != 2)
  {
    fmt::println(std::cerr, "Error: Invalid trace file (magic={:#x}, version={})", header.magic,
                 header.version);
    return false;
  }

  out.blocks.resize(header.block_count);
  for (u32 i = 0; i < header.block_count; i++)
  {
    if (!file.ReadBytes(&out.blocks[i].addr, 4) || !file.ReadBytes(&out.blocks[i].size, 4))
      return false;
    out.seed_addresses.insert(out.blocks[i].addr);
  }

  out.edges.resize(header.edge_count);
  for (u32 i = 0; i < header.edge_count; i++)
  {
    u8 pad[3];
    if (!file.ReadBytes(&out.edges[i].from, 4) || !file.ReadBytes(&out.edges[i].to, 4) ||
        !file.ReadBytes(&out.edges[i].type, 1) || !file.ReadBytes(pad, 3))
      return false;
    if (out.edges[i].type == 1)  // dynamic
      out.dynamic_targets.emplace(out.edges[i].from, out.edges[i].to);
  }

  for (u32 i = 0; i < header.smc_count; i++)
  {
    TraceSMC smc{};
    u64 counter;
    if (!file.ReadBytes(&smc.addr, 4) || !file.ReadBytes(&smc.length, 4) ||
        !file.ReadBytes(&counter, 8))
      return false;
    out.smc_regions.push_back(smc);
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

static void RunDisassembly(const PPCMemoryImage& memory, const TraceData& trace,
                           std::map<u32, CFGBlock>& blocks, std::vector<CFGEdge>& edges,
                           bool verbose)
{
  std::set<u32> worklist;
  std::set<u32> visited;

  // Seed with all trace block addresses
  for (u32 addr : trace.seed_addresses)
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

        // Add fall-through edge from old block to new block
        edges.push_back({existing.start_addr, addr, CFGEdge::FallThrough, false});

        // Update any edges that referenced the old block's start for edge targets
        // that should now reference the split block (not needed since edges use to_addr)
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

        if (inst.OPCD == 18)  // b / bl
        {
          u32 target = ComputeBranchTarget(inst, current);
          if (inst.LK)
          {
            edges.push_back({block_start, target, CFGEdge::Call, false});
            worklist.insert(target);
            worklist.insert(current + 4);
          }
          else
          {
            edges.push_back({block_start, target, CFGEdge::Static, false});
            worklist.insert(target);
          }
        }
        else if (inst.OPCD == 16)  // bc / bcl
        {
          u32 target = ComputeBranchTarget(inst, current);
          edges.push_back({block_start, target, CFGEdge::Static, false});
          worklist.insert(target);

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
// SQLite output
// ============================================================================

static bool WriteCFGDatabase(const std::string& path, const std::map<u32, CFGBlock>& blocks,
                             const std::vector<CFGEdge>& edges,
                             const std::map<u32, CFGFunction>& functions,
                             const std::vector<TraceSMC>& smc_regions,
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

  // 2. Read trace data
  TraceData trace;
  if (!ReadTraceFile(trace_path, trace))
    return EXIT_FAILURE;

  u32 dynamic_count = 0;
  for (const auto& e : trace.edges)
  {
    if (e.type == 1)
      dynamic_count++;
  }
  fmt::println(std::cerr, "Trace: {} seed blocks, {} edges ({} dynamic)", trace.blocks.size(),
               trace.edges.size(), dynamic_count);

  // 3. Run recursive descent disassembly
  std::map<u32, CFGBlock> blocks;
  std::vector<CFGEdge> edges;

  // Add DOL entry point as a seed
  trace.seed_addresses.insert(dol.GetEntryPoint());

  RunDisassembly(memory, trace, blocks, edges, verbose);

  // 4. Identify functions
  auto functions = IdentifyFunctions(blocks, edges, dol.GetEntryPoint());

  // 5. Compute statistics
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

  // 6. Write output
  if (!WriteCFGDatabase(output_path, blocks, edges, functions, trace.smc_regions,
                        trace.seed_addresses, dol.GetEntryPoint()))
  {
    return EXIT_FAILURE;
  }

  fmt::println(std::cerr, "Output written to {}", output_path);
  return EXIT_SUCCESS;
}

}  // namespace DolphinTool
