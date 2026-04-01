// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/AotCommand.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <OptionParser.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <sqlite3.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "Core/Boot/DolReader.h"

#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"

#include "DolphinTool/AotCEmitter.h"
#include "DolphinTool/PPCMemoryImage.h"

namespace DolphinTool
{

// Runtime header template emitted into the output directory
static const char* AOT_RUNTIME_HEADER = R"(
#ifndef AOT_RUNTIME_H
#define AOT_RUNTIME_H

#include <stdint.h>

// AOTState is layout-compatible with Dolphin's PowerPCState.
// At runtime, a PowerPCState* is cast to AOTState*.
typedef struct AOTState {
    uint32_t pc;
    uint32_t npc;
    void* stored_stack_pointer;
    void* gather_pipe_ptr;
    void* gather_pipe_base_ptr;
    uint32_t gpr[32];

    // Paired singles (ps0 and ps1 as raw uint64_t)
    struct { uint64_t ps0; uint64_t ps1; } ps[32] __attribute__((aligned(16)));

    // CR: 8 x uint64_t in Dolphin's optimized internal representation
    uint64_t cr_fields[8];

    uint32_t msr;
    uint32_t fpscr;
    uint32_t feature_flags;
    uint32_t exceptions;
    int32_t downcount;
    uint8_t xer_ca;
    uint8_t xer_so_ov;  // format: (SO << 1) | OV
    uint16_t xer_stringctrl;
    uint32_t reserve_address;
    uint8_t reserve;
    uint8_t pagetable_update_pending;
    uint8_t m_enable_dcache;
    uint8_t _pad0;

    uint32_t sr[16];
    uint32_t spr[1024] __attribute__((aligned(8)));
} AOTState;

// Runtime helpers (implemented in the Dolphin runtime harness)
extern uint32_t aot_read_u8(AOTState* s, uint32_t addr);
extern uint32_t aot_read_u16(AOTState* s, uint32_t addr);
extern uint32_t aot_read_u32(AOTState* s, uint32_t addr);
extern uint64_t aot_read_u64(AOTState* s, uint32_t addr);
extern void aot_write_u8(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u16(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u32(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u64(AOTState* s, uint64_t val, uint32_t addr);
extern void aot_interpreter_single_step(AOTState* s);
extern void aot_sc(AOTState* s);
extern void aot_rfi(AOTState* s);

// CR helpers (inline for performance)
static const uint64_t aot_cr_table[16] = {
    0x100000008ULL, 0x100000009ULL, 0x100000000ULL, 0x100000001ULL,
    0x900000008ULL, 0x900000009ULL, 0x900000000ULL, 0x900000001ULL,
    0x500000008ULL, 0x500000009ULL, 0x500000000ULL, 0x500000001ULL,
    0xD00000008ULL, 0xD00000009ULL, 0xD00000000ULL, 0xD00000001ULL,
};

static inline void aot_cr_set_field(AOTState* s, int field, uint32_t value) {
    s->cr_fields[field] = aot_cr_table[value & 0xF];
}

static inline uint32_t aot_cr_get_bit(AOTState* s, int bit) {
    int field = bit >> 2;
    int bit_in_field = 3 - (bit & 3);
    uint64_t cr = s->cr_fields[field];
    uint32_t ppc_cr = 0;
    // Reconstruct PPC CR field from internal representation
    ppc_cr |= (cr >> 59) & 0x9;  // LT (bit 62->bit 3) and SO (bit 59->bit 0)
    ppc_cr |= ((cr & 0xFFFFFFFF) == 0) << 1;  // EQ
    ppc_cr |= ((int64_t)cr > 0) << 2;  // GT
    return (ppc_cr >> bit_in_field) & 1;
}

static inline void aot_cmp_signed(AOTState* s, int crfd, int32_t a, int32_t b) {
    uint32_t cr_field;
    if (a < b) cr_field = 8;       // CR_LT
    else if (a > b) cr_field = 4;  // CR_GT
    else cr_field = 2;             // CR_EQ
    if (s->xer_so_ov >> 1) cr_field |= 1; // CR_SO
    aot_cr_set_field(s, crfd, cr_field);
}

static inline void aot_cmp_unsigned(AOTState* s, int crfd, uint32_t a, uint32_t b) {
    uint32_t cr_field;
    if (a < b) cr_field = 8;
    else if (a > b) cr_field = 4;
    else cr_field = 2;
    if (s->xer_so_ov >> 1) cr_field |= 1;
    aot_cr_set_field(s, crfd, cr_field);
}

static inline uint32_t aot_rotl(uint32_t val, uint32_t shift) {
    shift &= 31;
    return (val << shift) | (val >> (32 - shift));
}

static inline uint32_t aot_rotation_mask(int mb, int me) {
    uint32_t begin_mask = 0xFFFFFFFFu >> mb;
    uint32_t end_mask = (me < 31) ? (0xFFFFFFFFu >> (me + 1)) : 0;
    uint32_t mask = begin_mask ^ end_mask;
    return (me >= mb) ? mask : ~mask;
}

#endif // AOT_RUNTIME_H
)";

struct CFGBlockInfo
{
  u32 ppc_addr;
  u32 num_instructions;
  u32 function_addr;
  bool is_translatable;
};

static bool ReadCFGBlocks(const std::string& db_path, std::vector<CFGBlockInfo>& blocks)
{
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
  {
    fmt::println(std::cerr, "Error: Cannot open CFG database: {}", db_path);
    return false;
  }

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db,
                     "SELECT ppc_addr, num_instructions, function_addr, is_translatable "
                     "FROM blocks ORDER BY ppc_addr",
                     -1, &stmt, nullptr);

  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    CFGBlockInfo block{};
    block.ppc_addr = static_cast<u32>(sqlite3_column_int64(stmt, 0));
    block.num_instructions = static_cast<u32>(sqlite3_column_int(stmt, 1));
    // num_instructions is stored as size_bytes in Phase 2, convert
    if (block.num_instructions > 4)
      block.num_instructions /= 4;  // convert bytes to instruction count
    block.function_addr =
        sqlite3_column_type(stmt, 2) != SQLITE_NULL ? static_cast<u32>(sqlite3_column_int64(stmt, 2)) : 0;
    block.is_translatable = sqlite3_column_int(stmt, 3) != 0;
    blocks.push_back(block);
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

int AotCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: dolphin-tool translate [options]");

  parser.add_option("--cfg").action("store").help("Path to CFG database (from dolphin-tool cfg)");
  parser.add_option("--iso").action("store").help("Path to GameCube/Wii disc image");
  parser.add_option("--output").action("store").help("Path to output directory");
  parser.add_option("--prefix")
      .action("store")
      .help("Symbol prefix (default: game ID from disc)");
  parser.add_option("-v", "--verbose").action("store_true").help("Print detailed progress");

  const optparse::Values options = parser.parse_args(args);

  if (!options.is_set("cfg") || !options.is_set("iso") || !options.is_set("output"))
  {
    parser.print_help();
    return EXIT_FAILURE;
  }

  const std::string cfg_path = options["cfg"];
  const std::string iso_path = options["iso"];
  const std::string output_dir = options["output"];
  const bool verbose = options.is_set("verbose");

  // 1. Open disc and extract DOL
  auto volume = DiscIO::CreateDisc(iso_path);
  if (!volume)
  {
    fmt::println(std::cerr, "Error: Cannot open disc image: {}", iso_path);
    return EXIT_FAILURE;
  }

  std::string prefix;
  if (options.is_set("prefix"))
    prefix = options["prefix"];
  else
    prefix = volume->GetGameID();

  auto dol_offset = DiscIO::GetBootDOLOffset(*volume, DiscIO::PARTITION_NONE);
  auto dol_size = dol_offset ? DiscIO::GetBootDOLSize(*volume, DiscIO::PARTITION_NONE, *dol_offset)
                             : std::nullopt;
  if (!dol_offset || !dol_size)
  {
    fmt::println(std::cerr, "Error: Cannot find/read DOL");
    return EXIT_FAILURE;
  }

  std::vector<u8> dol_buffer(*dol_size);
  volume->Read(*dol_offset, *dol_size, dol_buffer.data(), DiscIO::PARTITION_NONE);

  DolReader dol(std::move(dol_buffer));
  if (!dol.IsValid())
  {
    fmt::println(std::cerr, "Error: Invalid DOL");
    return EXIT_FAILURE;
  }

  PPCMemoryImage memory;
  for (int i = 0; i < dol.GetNumTextSections(); i++)
  {
    const auto& section = dol.GetTextSection(i);
    if (!section.empty())
      memory.AddSection(dol.GetTextSectionAddress(i), section.data(), dol.GetTextSectionSize(i));
  }

  // 2. Read CFG database
  std::vector<CFGBlockInfo> cfg_blocks;
  if (!ReadCFGBlocks(cfg_path, cfg_blocks))
    return EXIT_FAILURE;

  fmt::println(std::cerr, "Prefix: {}", prefix);
  fmt::println(std::cerr, "DOL entry: {:#010x}", dol.GetEntryPoint());
  fmt::println(std::cerr, "CFG blocks: {}", cfg_blocks.size());

  // Build set of known block addresses for the emitter
  std::set<u32> known_blocks;
  for (const auto& b : cfg_blocks)
    known_blocks.insert(b.ppc_addr);

  // 3. Create output directory and write runtime header
  File::CreateFullPath(output_dir + "/");

  {
    std::ofstream header(output_dir + "/aot_runtime.h");
    header << AOT_RUNTIME_HEADER;
  }

  // 4. Translate blocks, split into files by address range (64KB granularity)
  AOTCEmitter emitter(memory, known_blocks, prefix);

  // Group blocks by high 16 bits of address
  std::map<u32, std::vector<const CFGBlockInfo*>> groups;
  for (const auto& b : cfg_blocks)
    groups[b.ppc_addr >> 16].push_back(&b);

  // Write forward declarations header
  {
    std::ofstream fwd(output_dir + "/" + prefix + "_forward_decls.h");
    fwd << "#ifndef " << prefix << "_FORWARD_DECLS_H\n";
    fwd << "#define " << prefix << "_FORWARD_DECLS_H\n";
    fwd << "#include \"aot_runtime.h\"\n\n";
    for (const auto& b : cfg_blocks)
    {
      if (b.is_translatable)
        fwd << fmt::format("void {}_block_{:08x}(AOTState* s);\n", prefix, b.ppc_addr);
    }
    fwd << fmt::format("void {}_dispatch(AOTState* s);\n", prefix);
    fwd << "#endif\n";
  }

  u32 translated = 0, skipped = 0;

  for (const auto& [group_key, group_blocks] : groups)
  {
    std::string filename =
        fmt::format("{}/{}_blocks_{:04x}.c", output_dir, prefix, group_key);
    std::ofstream file(filename);
    file << "#include \"aot_runtime.h\"\n";
    file << fmt::format("#include \"{}_forward_decls.h\"\n\n", prefix);

    for (const auto* b : group_blocks)
    {
      if (!b->is_translatable)
      {
        skipped++;
        continue;
      }

      std::string block_code = emitter.TranslateBlock(b->ppc_addr, b->num_instructions);
      file << block_code << "\n";
      translated++;
    }
  }

  // 5. Write dispatch table
  {
    std::string dispatch_file = fmt::format("{}/{}_dispatch.c", output_dir, prefix);
    std::ofstream file(dispatch_file);
    file << "#include \"aot_runtime.h\"\n";
    file << fmt::format("#include \"{}_forward_decls.h\"\n\n", prefix);

    file << "typedef void (*AOTBlockFunc)(AOTState*);\n\n";
    file << "typedef struct { uint32_t addr; AOTBlockFunc func; } AOTDispatchEntry;\n\n";

    file << fmt::format("static const AOTDispatchEntry {}_table[] = {{\n", prefix);
    for (const auto& b : cfg_blocks)
    {
      if (b.is_translatable)
        file << fmt::format("    {{ {:#010x}u, {}_block_{:08x} }},\n", b.ppc_addr, prefix,
                            b.ppc_addr);
    }
    file << "};\n\n";

    file << fmt::format(
        "static const uint32_t {}_table_size = sizeof({}_table) / sizeof({}_table[0]);\n\n",
        prefix, prefix, prefix);

    // Binary search dispatcher
    file << fmt::format("void {}_dispatch(AOTState* s) {{\n", prefix);
    file << "    uint32_t pc = s->pc;\n";
    file << fmt::format("    int lo = 0, hi = (int){}_table_size - 1;\n", prefix);
    file << "    while (lo <= hi) {\n";
    file << "        int mid = (lo + hi) / 2;\n";
    file << fmt::format("        if ({}_table[mid].addr == pc) {{ {}_table[mid].func(s); return; "
                        "}}\n",
                        prefix, prefix);
    file << fmt::format(
        "        if ({}_table[mid].addr < pc) lo = mid + 1; else hi = mid - 1;\n", prefix);
    file << "    }\n";
    file << "    aot_interpreter_single_step(s);\n";
    file << "}\n";
  }

  // 6. Report results
  fmt::println(std::cerr, "Results:");
  fmt::println(std::cerr, "  Translated: {} blocks", translated);
  fmt::println(std::cerr, "  Skipped (SMC): {} blocks", skipped);
  fmt::println(std::cerr, "  Output files: {} block files + dispatch + headers",
               groups.size());

  // Report unhandled opcodes
  const auto& unhandled = emitter.GetUnhandledOpcodes();
  if (!unhandled.empty())
  {
    fmt::println(std::cerr, "  Unhandled opcodes ({} types, falling back to interpreter):",
                 unhandled.size());
    // Sort by frequency
    std::vector<std::pair<std::string, u32>> sorted(unhandled.begin(), unhandled.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [name, count] : sorted)
      fmt::println(std::cerr, "    {}: {} occurrences", name, count);
  }

  fmt::println(std::cerr, "Output written to {}/", output_dir);
  return EXIT_SUCCESS;
}

}  // namespace DolphinTool
