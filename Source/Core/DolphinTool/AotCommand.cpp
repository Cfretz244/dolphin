// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/AotCommand.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <iostream>
#include <map>
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
#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "Core/Boot/DolReader.h"

#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"

#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCTables.h"

#include "DolphinTool/AotCEmitter.h"
#include "DolphinTool/PPCMemoryImage.h"
#include "DolphinTool/RelFile.h"
#include "DolphinTool/RelModules.h"

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

// ============================================================================
// Memory access — RAM fast path inlined into blocks; slow path (MMIO, EFB,
// locked cache, gather pipe) stays in the Dolphin runtime harness, which is the
// single authoritative implementation (PowerPC::ReadFromJit/WriteFromJit).
//
// The range check must match the runtime's IsRAMAddress exactly: cached RAM
// (0x80000000-) or the uncached mirror (0xC0000000-) only. Low-memory
// (0x0xxxxxxx) and 0x4xxxxxxx accesses must take the slow path — with MSR.DR=1
// the interpreter raises a DSI for them (BAT miss), so the fast path must not
// silently satisfy them from RAM.
// ============================================================================

typedef struct { uint8_t* ram; uint32_t size; } AotFastMem;
extern AotFastMem aot_fast_mem;  // filled by aot_init_fast_mem() before any block runs

extern uint32_t aot_read_u8_slow(AOTState* s, uint32_t addr);
extern uint32_t aot_read_u16_slow(AOTState* s, uint32_t addr);
extern uint32_t aot_read_u32_slow(AOTState* s, uint32_t addr);
extern uint64_t aot_read_u64_slow(AOTState* s, uint32_t addr);
extern void aot_write_u8_slow(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u16_slow(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u16_br_slow(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u32_slow(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u64_slow(AOTState* s, uint64_t val, uint32_t addr);

static inline int aot_is_ram(uint32_t addr) {
    return ((addr & ~0x40000000u) - 0x80000000u) < aot_fast_mem.size;
}

static inline uint32_t aot_read_u8(AOTState* s, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1))
        return aot_fast_mem.ram[addr & 0x3FFFFFFFu];
    return aot_read_u8_slow(s, addr);
}
static inline uint32_t aot_read_u16(AOTState* s, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint16_t v; __builtin_memcpy(&v, aot_fast_mem.ram + (addr & 0x3FFFFFFFu), 2);
        return __builtin_bswap16(v);
    }
    return aot_read_u16_slow(s, addr);
}
static inline uint32_t aot_read_u16_se(AOTState* s, uint32_t addr) {  // sign-extended half
    return (uint32_t)(int32_t)(int16_t)aot_read_u16(s, addr);
}
static inline uint32_t aot_read_u32(AOTState* s, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint32_t v; __builtin_memcpy(&v, aot_fast_mem.ram + (addr & 0x3FFFFFFFu), 4);
        return __builtin_bswap32(v);
    }
    return aot_read_u32_slow(s, addr);
}
static inline uint64_t aot_read_u64(AOTState* s, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint64_t v; __builtin_memcpy(&v, aot_fast_mem.ram + (addr & 0x3FFFFFFFu), 8);
        return __builtin_bswap64(v);
    }
    return aot_read_u64_slow(s, addr);
}
static inline void aot_write_u8(AOTState* s, uint32_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        aot_fast_mem.ram[addr & 0x3FFFFFFFu] = (uint8_t)val;
        return;
    }
    aot_write_u8_slow(s, val, addr);
}
static inline void aot_write_u16(AOTState* s, uint32_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint16_t v = __builtin_bswap16((uint16_t)val);
        __builtin_memcpy(aot_fast_mem.ram + (addr & 0x3FFFFFFFu), &v, 2);
        return;
    }
    aot_write_u16_slow(s, val, addr);
}
static inline void aot_write_u16_br(AOTState* s, uint32_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint16_t v = (uint16_t)val;  // no swap — byte-reversed store
        __builtin_memcpy(aot_fast_mem.ram + (addr & 0x3FFFFFFFu), &v, 2);
        return;
    }
    aot_write_u16_br_slow(s, val, addr);
}
static inline void aot_write_u32(AOTState* s, uint32_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint32_t v = __builtin_bswap32(val);
        __builtin_memcpy(aot_fast_mem.ram + (addr & 0x3FFFFFFFu), &v, 4);
        return;
    }
    aot_write_u32_slow(s, val, addr);
}
static inline void aot_write_u64(AOTState* s, uint64_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint64_t v = __builtin_bswap64(val);
        __builtin_memcpy(aot_fast_mem.ram + (addr & 0x3FFFFFFFu), &v, 8);
        return;
    }
    aot_write_u64_slow(s, val, addr);
}

// Runtime helpers (implemented in the Dolphin runtime harness)
extern void aot_interpreter_single_step(AOTState* s);
extern void aot_sc(AOTState* s);
extern int aot_check_fpu(AOTState* s, uint32_t pc);
extern void aot_rfi(AOTState* s);

// FP conversion helpers
extern uint64_t aot_convert_to_double(uint32_t single_bits);
extern uint32_t aot_convert_to_single(uint64_t double_bits);

// SPR/CR/MSR helpers
extern uint32_t aot_mfspr_special(AOTState* s, uint32_t spr);
extern void aot_mtspr_special(AOTState* s, uint32_t spr, uint32_t val);
extern uint32_t aot_mfcr(AOTState* s);
extern void aot_mtcrf(AOTState* s, uint32_t mask, uint32_t rs_reg);
extern void aot_msr_updated(AOTState* s);
extern void aot_mtmsr(AOTState* s, uint32_t val);
extern void aot_sr_updated(AOTState* s);
extern int aot_twi(AOTState* s, uint32_t TO, int32_t a, int32_t b);
extern void aot_cr_logical(AOTState* s, int crbD, int crbA, int crbB, const char* op);

// Cache ops
extern void aot_dcbz(AOTState* s, uint32_t addr);
extern void aot_dcbz_l(AOTState* s, uint32_t addr);
extern void aot_dcbt(AOTState* s, uint32_t addr);
extern void aot_icbi(AOTState* s, uint32_t addr);

// Timebase
extern uint32_t aot_mftb(AOTState* s, uint32_t spr);

// FP arithmetic (all via runtime for NaN/exception correctness)
extern void aot_faddx(AOTState* s, int fd, int fa, int fb);
extern void aot_fsubx(AOTState* s, int fd, int fa, int fb);
extern void aot_fmulx(AOTState* s, int fd, int fa, int fc);
extern void aot_fdivx(AOTState* s, int fd, int fa, int fb);
extern void aot_fmaddx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fmsubx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fnmsubx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fnmaddx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_faddsx(AOTState* s, int fd, int fa, int fb);
extern void aot_fsubsx(AOTState* s, int fd, int fa, int fb);
extern void aot_fmulsx(AOTState* s, int fd, int fa, int fc);
extern void aot_fdivsx(AOTState* s, int fd, int fa, int fb);
extern void aot_fmaddsx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fmsubsx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fnmsubsx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fnmaddsx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fcmpu(AOTState* s, int crfd, int fa, int fb);
extern void aot_fcmpo(AOTState* s, int crfd, int fa, int fb);
extern void aot_frspx(AOTState* s, int fd, int fb);
extern void aot_fctiwx(AOTState* s, int fd, int fb);
extern void aot_fctiwzx(AOTState* s, int fd, int fb);
extern void aot_fresx(AOTState* s, int fd, int fb);
extern void aot_frsqrtex(AOTState* s, int fd, int fb);
extern void aot_fselx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_mtfsf(AOTState* s, int fm, int fb);
extern void aot_mtfsfi(AOTState* s, int crfd, int imm);
extern void aot_mcrfs(AOTState* s, int crfd, int crfs);

// Paired singles (all via runtime)
extern void aot_ps_add(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_sub(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_mul(AOTState* s, int fd, int fa, int fc);
extern void aot_ps_div(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_madd(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_msub(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_nmadd(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_nmsub(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_sum0(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_sum1(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_muls0(AOTState* s, int fd, int fa, int fc);
extern void aot_ps_muls1(AOTState* s, int fd, int fa, int fc);
extern void aot_ps_madds0(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_madds1(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_sel(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_res(AOTState* s, int fd, int fb);
extern void aot_ps_rsqrte(AOTState* s, int fd, int fb);
extern void aot_ps_neg(AOTState* s, int fd, int fb);
extern void aot_ps_mr(AOTState* s, int fd, int fb);
extern void aot_ps_abs(AOTState* s, int fd, int fb);
extern void aot_ps_nabs(AOTState* s, int fd, int fb);
extern void aot_ps_merge00(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_merge01(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_merge10(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_merge11(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_cmpu0(AOTState* s, int crfd, int fa, int fb);
extern void aot_ps_cmpo0(AOTState* s, int crfd, int fa, int fb);
extern void aot_ps_cmpu1(AOTState* s, int crfd, int fa, int fb);
extern void aot_ps_cmpo1(AOTState* s, int crfd, int fa, int fb);
extern void aot_psq_l(AOTState* s, int fd, int ra, uint32_t inst);
extern void aot_psq_lu(AOTState* s, int fd, int ra, uint32_t inst);
extern void aot_psq_st(AOTState* s, int fs, int ra, uint32_t inst);
extern void aot_psq_stu(AOTState* s, int fs, int ra, uint32_t inst);
extern void aot_psq_lx(AOTState* s, uint32_t inst);
extern void aot_psq_stx(AOTState* s, uint32_t inst);
extern void aot_psq_lux(AOTState* s, uint32_t inst);
extern void aot_psq_stux(AOTState* s, uint32_t inst);

// CR helpers (inline for performance)
// Values from ConditionRegister::PPCToInternal() — Dolphin's optimized 64-bit CR encoding.
// Index = 4-bit PPC CR field value (LT=8, GT=4, EQ=2, SO=1).
// Internal: SO=bit59, EQ=(low32==0), GT=((s64)val>0), LT=bit62.
static const uint64_t aot_cr_table[16] = {
    0x8000000100000001ULL, 0x8800000100000001ULL, 0x8000000100000000ULL, 0x8800000100000000ULL,
    0x0000000100000001ULL, 0x0800000100000001ULL, 0x0000000100000000ULL, 0x0800000100000000ULL,
    0xC000000100000001ULL, 0xC800000100000001ULL, 0xC000000100000000ULL, 0xC800000100000000ULL,
    0x4000000100000001ULL, 0x4800000100000001ULL, 0x4000000100000000ULL, 0x4800000100000000ULL,
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

// Single-block mode flag (set by compare/diff harness to stop block chaining)
extern int aot_single_block_mode;

// Block edges test AOT_EDGE_STOP alongside the downcount check. In production the
// flag can never be set, so the load is compiled out; harness builds (macOS
// build.sh passes -DAOT_HARNESS=1) keep it so AOT_COMPARE can stop chaining and
// compare single blocks. The dispatch-entry check remains unconditional either way.
#ifndef AOT_HARNESS
#define AOT_HARNESS 0
#endif
#if AOT_HARNESS
#define AOT_EDGE_STOP || aot_single_block_mode
#else
#define AOT_EDGE_STOP
#endif

#endif // AOT_RUNTIME_H
)";

struct CFGBlockInfo
{
  u32 ppc_addr;
  u32 num_instructions;
  u32 function_addr;
  bool is_translatable;
  bool from_trace;  // true if observed during trace collection (hot)
};

struct CFGEdgeInfo
{
  u32 from_addr;
  u32 to_addr;
  std::string edge_type;  // "static", "fallthrough", "call", "dynamic"
};

static bool ReadCFGEdges(const std::string& db_path, std::vector<CFGEdgeInfo>& edges)
{
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    return false;

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db, "SELECT from_addr, to_addr, edge_type FROM edges", -1, &stmt, nullptr);

  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    CFGEdgeInfo edge{};
    edge.from_addr = static_cast<u32>(sqlite3_column_int64(stmt, 0));
    edge.to_addr = static_cast<u32>(sqlite3_column_int64(stmt, 1));
    edge.edge_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    edges.push_back(edge);
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

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
                     "SELECT ppc_addr, num_instructions, function_addr, is_translatable, source "
                     "FROM blocks ORDER BY ppc_addr",
                     -1, &stmt, nullptr);

  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    CFGBlockInfo block{};
    block.ppc_addr = static_cast<u32>(sqlite3_column_int64(stmt, 0));
    block.num_instructions = static_cast<u32>(sqlite3_column_int(stmt, 1));
    block.function_addr =
        sqlite3_column_type(stmt, 2) != SQLITE_NULL ? static_cast<u32>(sqlite3_column_int64(stmt, 2)) : 0;
    block.is_translatable = sqlite3_column_int(stmt, 3) != 0;
    const char* source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    block.from_trace = source && (std::strcmp(source, "trace") == 0 || std::strcmp(source, "both") == 0);
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

  std::vector<CFGEdgeInfo> cfg_edges;
  ReadCFGEdges(cfg_path, cfg_edges);

  fmt::println(std::cerr, "Prefix: {}", prefix);
  fmt::println(std::cerr, "DOL entry: {:#010x}", dol.GetEntryPoint());
  fmt::println(std::cerr, "CFG blocks: {}", cfg_blocks.size());

  // Validate DOL instructions match CFG block boundaries.
  // If a block's edges indicate a branch (non-sequential target) but the DOL's
  // last instruction is not a block-ending instruction, the game patched the code
  // at runtime (SMC not caught by icbi trace). Mark these blocks untranslatable.
  {
    // Build edge map: block_addr -> set of static branch targets only.
    // Fallthrough edges can have non-sequential targets due to CFG extraction
    // artifacts (block splitting) and must be excluded. Only "static" edges
    // (direct branches in the DOL) indicate the block must end with a branch.
    std::unordered_map<u32, std::vector<u32>> block_static_edges;
    for (const auto& e : cfg_edges)
    {
      if (e.edge_type == "static")
        block_static_edges[e.from_addr].push_back(e.to_addr);
    }

    u32 smc_skipped = 0;
    for (auto& b : cfg_blocks)
    {
      if (!b.is_translatable || b.num_instructions == 0)
        continue;

      u32 fall_through_addr = b.ppc_addr + b.num_instructions * 4;
      auto edge_it = block_static_edges.find(b.ppc_addr);

      // Check if any static edge targets a non-sequential address (i.e., a branch)
      bool has_non_sequential_edge = false;
      if (edge_it != block_static_edges.end())
      {
        for (u32 target : edge_it->second)
        {
          if (target != fall_through_addr)
          {
            has_non_sequential_edge = true;
            break;
          }
        }
      }

      if (!has_non_sequential_edge)
        continue;

      // The CFG says this block branches, so the DOL's last instruction should
      // be a block-ending instruction (branch/trap/etc.). Check it.
      u32 last_pc = b.ppc_addr + (b.num_instructions - 1) * 4;
      auto inst_word = memory.ReadInstruction(last_pc);
      if (!inst_word)
        continue;

      UGeckoInstruction inst(*inst_word);
      const GekkoOPInfo* info = PPCTables::GetOpInfo(inst, last_pc);
      if (info && (info->flags & FL_ENDBLOCK))
        continue;  // DOL agrees — last instruction is a branch

      // DOL disagrees: last instruction is not a branch but CFG says it should be.
      // The game patched this code at runtime. Mark untranslatable.
      b.is_translatable = false;
      smc_skipped++;
    }

    if (smc_skipped > 0)
      fmt::println(std::cerr, "  DOL/runtime mismatch: {} blocks marked untranslatable", smc_skipped);
  }

  // Build set of known block addresses for the emitter (only translatable blocks)
  std::set<u32> known_blocks;
  for (const auto& b : cfg_blocks)
  {
    if (b.is_translatable)
      known_blocks.insert(b.ppc_addr);
  }

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

  // Dispatch-table extent — needed up front: the forward-decls header exposes
  // the table to every block TU for per-site indirect (blr/bctr) dispatch.
  u32 table_base = UINT32_MAX, table_max_addr = 0;
  for (const auto& b : cfg_blocks)
  {
    if (b.is_translatable)
    {
      table_base = std::min(table_base, b.ppc_addr);
      table_max_addr = std::max(table_max_addr, b.ppc_addr);
    }
  }
  // Align base to 4-byte boundary
  table_base &= ~3u;
  const u32 table_entries = ((table_max_addr - table_base) >> 2) + 1;

  // Write forward declarations header
  {
    std::ofstream fwd(output_dir + "/" + prefix + "_forward_decls.h");
    fwd << "#ifndef " << prefix << "_FORWARD_DECLS_H\n";
    fwd << "#define " << prefix << "_FORWARD_DECLS_H\n";
    fwd << "#include \"aot_runtime.h\"\n\n";
    fwd << fmt::format("#define {}_TABLE_BASE {:#010x}u\n", prefix, table_base);
    fwd << fmt::format("#define {}_TABLE_SIZE {}u\n", prefix, table_entries);
    fwd << fmt::format("extern void (*{}_fast_table[])(AOTState*);\n\n", prefix);
    for (const auto& b : cfg_blocks)
    {
      if (b.is_translatable)
        fwd << fmt::format("__attribute__((noinline)) void {}_block_{:08x}(AOTState* s);\n",
                           prefix, b.ppc_addr);
    }
    fwd << fmt::format("__attribute__((noinline)) void {}_dispatch(AOTState* s);\n", prefix);
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

      std::string block_code = emitter.TranslateBlock(b->ppc_addr, b->num_instructions,
                                                      b->from_trace);
      file << block_code << "\n";
      translated++;
    }
  }

  // 4b. Translate REL modules if the CFG database carries module tables (built
  // by `dolphin-tool cfg` from purely static relocation-seeded discovery).
  struct ModuleSectionOut
  {
    u32 size = 0;
    bool executable = false;
    u32 block_count = 0;
  };
  struct ModuleOut
  {
    u32 module_id;
    u32 dense;
    const RelFile* rel;
    std::map<u32, u32> blocks;  // synthetic (section<<24)|offset -> num_instructions
    std::vector<ModuleSectionOut> sections;
  };
  std::vector<ModuleOut> mods;
  std::vector<RelFile> rel_files;
  {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(cfg_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK)
    {
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db, "SELECT module_id, dense_idx FROM modules ORDER BY dense_idx",
                             -1, &stmt, nullptr) == SQLITE_OK)
      {
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
          ModuleOut m{};
          m.module_id = static_cast<u32>(sqlite3_column_int64(stmt, 0));
          m.dense = static_cast<u32>(sqlite3_column_int(stmt, 1));
          mods.push_back(m);
        }
        sqlite3_finalize(stmt);

        sqlite3_prepare_v2(db,
                           "SELECT module_id, section_idx, offset, num_instructions FROM "
                           "module_blocks WHERE is_translatable=1",
                           -1, &stmt, nullptr);
        std::unordered_map<u32, ModuleOut*> by_id;
        for (auto& m : mods)
          by_id[m.module_id] = &m;
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
          u32 mid = static_cast<u32>(sqlite3_column_int64(stmt, 0));
          u32 sect = static_cast<u32>(sqlite3_column_int(stmt, 1));
          u32 off = static_cast<u32>(sqlite3_column_int64(stmt, 2));
          u32 ni = static_cast<u32>(sqlite3_column_int(stmt, 3));
          auto it = by_id.find(mid);
          if (it != by_id.end())
            it->second->blocks[(sect << 24) | off] = ni;
        }
        sqlite3_finalize(stmt);
      }
      sqlite3_close(db);
    }
  }

  std::unordered_map<u32, u32> module_dense;  // module_id -> dense idx
  for (const auto& m : mods)
    module_dense[m.module_id] = m.dense;

  u64 module_table_bytes = 0;
  u32 module_blocks_translated = 0;
  std::map<std::string, u32> module_unhandled;

  if (!mods.empty())
  {
    // The DB stores no section content — re-discover the RELs from the
    // (sha256-pinned) disc image for instruction bytes and relocations.
    rel_files = DiscoverRelModules(*volume, false);
    std::unordered_map<u32, const RelFile*> rel_by_id;
    for (const auto& r : rel_files)
      rel_by_id[r.module_id] = &r;
    for (auto& m : mods)
    {
      auto it = rel_by_id.find(m.module_id);
      m.rel = it != rel_by_id.end() ? it->second : nullptr;
      if (!m.rel)
        fmt::println(std::cerr, "Warning: module {} in CFG DB but not on disc", m.module_id);
    }
    std::erase_if(mods, [](const ModuleOut& m) { return m.rel == nullptr; });

    auto field16 = [](u8 type, u32 t) -> u16 {
      switch (type)
      {
      case R_PPC_ADDR16_LO:
        return static_cast<u16>(t);
      case R_PPC_ADDR16_HI:
        return static_cast<u16>(t >> 16);
      default:  // R_PPC_ADDR16_HA
        return static_cast<u16>((t + 0x8000) >> 16);
      }
    };
    auto is_imm_type = [](u8 type) {
      return type == R_PPC_ADDR16_LO || type == R_PPC_ADDR16_HI || type == R_PPC_ADDR16_HA;
    };
    auto is_branch_type = [](u8 type) {
      return type == R_PPC_REL24 || type == R_PPC_REL14 || type == R_PPC_REL14_BRTAKEN ||
             type == R_PPC_REL14_BRNTAKEN || type == R_PPC_ADDR24 || type == R_PPC_ADDR14 ||
             type == R_PPC_ADDR14_BRTAKEN || type == R_PPC_ADDR14_BRNTAKEN;
    };

    for (auto& m : mods)
    {
      const RelFile& rel = *m.rel;
      const std::string mp = fmt::format("{}_m{:03d}", prefix, m.dense);

      m.sections.resize(rel.sections.size());
      for (size_t i = 0; i < rel.sections.size(); i++)
      {
        m.sections[i].size = rel.sections[i].size;
        m.sections[i].executable = rel.sections[i].executable;
      }

      // Pre-patch DOL-target immediates into copies of the executable sections:
      // the absolute address is known, so the patched word translates through
      // the normal emitter path with no special handling.
      std::vector<std::vector<u8>> patched(rel.sections.size());
      for (size_t i = 0; i < rel.sections.size(); i++)
      {
        if (rel.sections[i].executable)
          patched[i] = rel.sections[i].data;
      }

      ModuleMode mode;
      mode.fn_prefix = mp;
      mode.base_array = mp + "_base";
      mode.dol_blocks = &known_blocks;
      for (const auto& sec : rel.sections)
        mode.section_sizes.push_back(sec.size);

      for (const auto& r : rel.relocs)
      {
        if (r.site_section >= rel.sections.size() || !rel.sections[r.site_section].executable)
          continue;
        const u32 site = (u32(r.site_section) << 24) | r.site_offset;

        if (is_imm_type(r.type))
        {
          // ELF semantics: ADDR16_* relocations point at the 16-bit immediate
          // FIELD (instruction + 2), not the instruction word. The emitter is
          // keyed by instruction pc, so mask down.
          const u32 inst_site = (u32(r.site_section) << 24) | (r.site_offset & ~3u);
          if (r.target_module == 0)
          {
            const u16 f = field16(r.type, r.addend);
            auto& data = patched[r.site_section];
            data[r.site_offset] = static_cast<u8>(f >> 8);
            data[r.site_offset + 1] = static_cast<u8>(f);
          }
          else if (r.target_module == rel.module_id)
          {
            mode.imm_relocs[inst_site] = ModuleImmReloc{
                r.type,
                fmt::format("({}_base[{}]+{:#x}u)", mp, r.target_section, r.addend)};
          }
          else
          {
            // Cross-module immediate (a handful per game): the in-RAM
            // instruction was relocated by the game's own OSLink — single-step.
            mode.force_fallback.insert(inst_site);
          }
        }
        else if (is_branch_type(r.type))
        {
          ModuleBranchOverride ov{};
          if (r.target_module == 0)
          {
            ov.kind = ModuleBranchOverride::Absolute;
            ov.target = r.addend;
          }
          else if (r.target_module == rel.module_id && (r.addend & 3) == 0 &&
                   r.target_section < rel.sections.size() &&
                   rel.sections[r.target_section].executable)
          {
            ov.kind = ModuleBranchOverride::Local;
            ov.target = (u32(r.target_section) << 24) | r.addend;
          }
          else
          {
            ov.kind = ModuleBranchOverride::External;
          }
          mode.branch_overrides[site] = ov;
        }
        else
        {
          mode.force_fallback.insert(site);
        }
      }

      PPCMemoryImage mod_mem;
      for (size_t i = 0; i < rel.sections.size(); i++)
      {
        if (rel.sections[i].executable && !patched[i].empty())
          mod_mem.AddSection(static_cast<u32>(i) << 24, patched[i].data(), rel.sections[i].size);
      }

      std::set<u32> mod_block_set;
      for (const auto& [addr, ni] : m.blocks)
        mod_block_set.insert(addr);

      AOTCEmitter mod_emitter(mod_mem, {}, prefix);
      mod_emitter.SetModuleMode(&mode, std::move(mod_block_set));

      std::string filename = fmt::format("{}/{}_blocks_m{:03d}.c", output_dir, prefix, m.dense);
      std::ofstream file(filename);
      file << "#include \"aot_runtime.h\"\n";
      file << fmt::format("#include \"{}_forward_decls.h\"\n\n", prefix);
      file << "typedef void (*AOTBlockFunc)(AOTState*);\n\n";
      // Runtime section bases, written by the module tracker on (un)load.
      // Zero-initialized = module not resident.
      file << fmt::format("uint32_t {}_base[{}];\n\n", mp, rel.sections.size());

      // Forward declarations for this module's own blocks (musttail chaining).
      for (const auto& [addr, ni] : m.blocks)
      {
        file << fmt::format("__attribute__((noinline)) void {}_s{}_{:x}(AOTState* s);\n", mp,
                            addr >> 24, addr & 0x00FFFFFF);
      }
      file << "\n";

      for (const auto& [addr, ni] : m.blocks)
      {
        file << mod_emitter.TranslateBlock(addr, ni, false) << "\n";
        module_blocks_translated++;
        m.sections[addr >> 24].block_count++;
      }

      // Per-section flat dispatch tables, indexed by (offset >> 2).
      for (size_t sect = 0; sect < rel.sections.size(); sect++)
      {
        if (!rel.sections[sect].executable || rel.sections[sect].size == 0)
          continue;
        const u32 entries = (rel.sections[sect].size + 3) / 4;
        module_table_bytes += u64(entries) * sizeof(void*);
        file << fmt::format("const AOTBlockFunc {}_s{}_table[{}] = {{\n", mp, sect, entries);
        auto it = m.blocks.lower_bound(static_cast<u32>(sect) << 24);
        const u32 sect_end = (static_cast<u32>(sect) << 24) | 0x00FFFFFF;
        for (u32 e = 0; e < entries; e++)
        {
          const u32 addr = (static_cast<u32>(sect) << 24) | (e << 2);
          while (it != m.blocks.end() && it->first < addr && it->first <= sect_end)
            ++it;
          if (it != m.blocks.end() && it->first == addr)
            file << fmt::format("    {}_s{}_{:x},\n", mp, sect, addr & 0x00FFFFFF);
          else
            file << "    0,\n";
        }
        file << "};\n\n";
      }

      for (const auto& [name, count] : mod_emitter.GetUnhandledOpcodes())
        module_unhandled[name] += count;
    }

    fmt::println(std::cerr, "  Modules: {} translated ({} blocks, {:.1f} MB of tables)",
                 mods.size(), module_blocks_translated,
                 module_table_bytes / (1024.0 * 1024.0));
    for (const auto& [name, count] : module_unhandled)
      fmt::println(std::cerr, "    module fallback [{}]: {} sites", name, count);
  }

  // 5. Write dispatch table — flat direct-mapped array for O(1) lookup
  // (extent computed above, alongside the forward-decls header)
  {
    const u32 min_addr = table_base;

    fmt::println(std::cerr, "  Dispatch table: {:#010x}-{:#010x} ({} entries, {:.1f} MB)",
                 min_addr, table_max_addr, table_entries,
                 table_entries * sizeof(void*) / (1024.0 * 1024.0));

    std::string dispatch_file = fmt::format("{}/{}_dispatch.c", output_dir, prefix);
    std::ofstream file(dispatch_file);
    file << "#include \"aot_runtime.h\"\n";
    file << fmt::format("#include \"{}_forward_decls.h\"\n\n", prefix);

    file << "typedef void (*AOTBlockFunc)(AOTState*);\n\n";

    // Emit the flat lookup table. Non-static: block TUs probe it directly at
    // blr/bctr sites (declared in the forward-decls header; TABLE_BASE/SIZE
    // defines live there too). Read-only after link.
    file << fmt::format("AOTBlockFunc {}_fast_table[{}] = {{\n", prefix, table_entries);

    // Build a set for quick lookup — all translatable blocks get dispatch entries
    std::map<u32, std::string> addr_to_sym;
    for (const auto& b : cfg_blocks)
    {
      if (b.is_translatable)
        addr_to_sym[b.ppc_addr] = fmt::format("{}_block_{:08x}", prefix, b.ppc_addr);
    }

    // Emit table entries — NULL for gaps, function pointer for known blocks
    // Write in chunks to keep file manageable
    for (u32 i = 0; i < table_entries; i++)
    {
      u32 addr = min_addr + (i << 2);
      auto it = addr_to_sym.find(addr);
      if (it != addr_to_sym.end())
        file << fmt::format("    {},\n", it->second);
      else
        file << "    0,\n";
    }
    file << "};\n\n";

    // Single-block mode flag — defined in Dolphin's AotRuntime.cpp.
    // Declared extern here to avoid multiple-definition errors with multi-game builds.
    file << "extern int aot_single_block_mode;\n\n";

    // Emit the fast dispatch function — O(1) array lookup
    file << fmt::format("__attribute__((noinline)) void {}_dispatch(AOTState* s) {{\n", prefix);
    // Single-block mode only exists in harness builds (macOS build.sh passes
    // -DAOT_HARNESS=1); production/iOS skips the global load on every dispatch.
    file << "#if AOT_HARNESS\n";
    file << "    if (aot_single_block_mode) return;\n";
    file << "#endif\n";
    // Downcount check: pc is always set before musttail-ing into dispatch, so a
    // plain return resumes the Run loop. Without this, execution cycles whose only
    // backward jumps are indirect (blr/bctr) never return to the Run loop and
    // interrupts are never delivered.
    file << "    if (s->downcount <= 0) return;\n";
    file << "    uint32_t pc = s->pc;\n";
    file << fmt::format("    uint32_t idx = (pc - {}_TABLE_BASE) >> 2;\n", prefix);
    file << fmt::format("    if (idx < {}_TABLE_SIZE) {{\n", prefix);
    file << fmt::format("        AOTBlockFunc fn = {}_fast_table[idx];\n", prefix);
    file << "        if (fn) { [[clang::musttail]] return fn(s); }\n";
    file << "    }\n";
    if (mods.empty())
    {
      file << "    [[clang::musttail]] return aot_interpreter_single_step(s);\n";
    }
    else
    {
      // Module-aware fallback: consults the runtime tracker's active-module
      // ranges before degrading to the interpreter.
      file << "    extern void aot_module_dispatch(AOTState*);\n";
      file << "    [[clang::musttail]] return aot_module_dispatch(s);\n";
    }
    file << "}\n\n";

    // Emit a block lookup function for the diff harness — returns a single
    // block's function pointer without executing it or tail-calling.
    file << fmt::format("AOTBlockFunc {}_lookup_block(uint32_t pc) {{\n", prefix);
    file << fmt::format("    uint32_t idx = (pc - {}_TABLE_BASE) >> 2;\n", prefix);
    file << fmt::format("    if (idx < {}_TABLE_SIZE) return {}_fast_table[idx];\n", prefix,
                        prefix);
    file << "    return 0;\n";
    file << "}\n\n";

    // Module descriptors: one entry per compiled REL, registered alongside the
    // game so the runtime tracker can activate tables and write section bases
    // when the game loads/unloads modules.
    if (!mods.empty())
    {
      file << "typedef struct AotModuleSectionDesc {\n";
      file << "    uint32_t size;\n";
      file << "    uint32_t executable;\n";
      file << "    const AOTBlockFunc* table;  /* NULL for non-executable sections */\n";
      file << "    uint32_t* base_slot;        /* runtime section base, 0 = unloaded */\n";
      file << "} AotModuleSectionDesc;\n";
      file << "typedef struct AotModuleDesc {\n";
      file << "    uint32_t module_id;\n";
      file << "    uint32_t num_sections;\n";
      file << "    const AotModuleSectionDesc* sections;\n";
      file << "} AotModuleDesc;\n\n";

      for (const auto& m : mods)
      {
        const std::string mp = fmt::format("{}_m{:03d}", prefix, m.dense);
        file << fmt::format("extern uint32_t {}_base[];\n", mp);
        for (size_t sect = 0; sect < m.sections.size(); sect++)
        {
          if (m.sections[sect].executable && m.sections[sect].size > 0)
            file << fmt::format("extern const AOTBlockFunc {}_s{}_table[];\n", mp, sect);
        }
        file << fmt::format("static const AotModuleSectionDesc {}_sections[] = {{\n", mp);
        for (size_t sect = 0; sect < m.sections.size(); sect++)
        {
          const auto& s = m.sections[sect];
          const bool has_table = s.executable && s.size > 0;
          file << fmt::format("    {{ {:#x}u, {}, {}, &{}_base[{}] }},\n", s.size,
                              s.executable ? 1 : 0,
                              has_table ? fmt::format("{}_s{}_table", mp, sect) : "0", mp, sect);
        }
        file << "};\n";
      }
      file << fmt::format("\nstatic const AotModuleDesc {}_modules[] = {{\n", prefix);
      for (const auto& m : mods)
      {
        file << fmt::format("    {{ {}u, {}u, {}_m{:03d}_sections }},\n", m.module_id,
                            m.sections.size(), prefix, m.dense);
      }
      file << "};\n\n";
    }

    // Emit self-registration constructor — runs before main() to register
    // this game's dispatch/lookup with Dolphin's AotRegistry.
    file << "__attribute__((constructor))\n";
    file << fmt::format("static void aot_register_{}(void) {{\n", prefix);
    file << "    extern void aot_register_game(const char*,"
            " void (*)(AOTState*), AOTBlockFunc (*)(uint32_t));\n";
    file << fmt::format("    aot_register_game(\"{}\", {}_dispatch, {}_lookup_block);\n", prefix,
                        prefix, prefix);
    if (!mods.empty())
    {
      file << "    extern void aot_register_game_modules(const char*, const void*, uint32_t);\n";
      file << fmt::format("    aot_register_game_modules(\"{}\", {}_modules, {}u);\n", prefix,
                          prefix, mods.size());
    }
    file << "}\n";
  }

  // 6. Emit build script with LTO support
  {
    std::string build_script = fmt::format("{}/build.sh", output_dir);
    std::ofstream script(build_script);
    script << "#!/bin/bash\n";
    script << "set -e\n";
    script << "cd \"$(dirname \"$0\")\"\n";
    script << fmt::format("PREFIX=\"{}\"\n", prefix);
    // -fwrapv: emitted code relies on wrapping signed multiply (mulli/mullw, e.g.
    // LCG RNGs); -fno-strict-aliasing: insurance for generated pointer casts.
    // -DAOT_HARNESS=1: macOS libs keep the aot_single_block_mode test on block edges
    // so the AOT_COMPARE harness can stop chaining; iOS libs compile it out.
    script << "BLOCK_CFLAGS=\"-Os -flto=thin -arch arm64 -mcpu=apple-a14 -moutline"
              " -fwrapv -fno-strict-aliasing -DAOT_HARNESS=1\"\n";
    script << "DISPATCH_CFLAGS=\"-O2 -flto=thin -arch arm64 -mcpu=apple-a14"
              " -fwrapv -fno-strict-aliasing -DAOT_HARNESS=1\"\n";
    // Bounded parallelism: hundreds of block files with one clang each swamps
    // the machine. xargs -P instead of `wait -n` — macOS ships bash 3.2.
    script << "JOBS=\"${AOT_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}\"\n\n";
    script << "echo \"Compiling AOT blocks with LTO (${JOBS} jobs)...\"\n";
    script << "export BLOCK_CFLAGS\n";
    script << "printf '%s\\0' ${PREFIX}_blocks_*.c | \\\n";
    script << "    xargs -0 -n1 -P \"$JOBS\" sh -c"
              " 'exec clang -c $BLOCK_CFLAGS -I. \"$1\" -o \"${1%.c}.o\"' sh\n";
    script << "clang -c $DISPATCH_CFLAGS -I. \"${PREFIX}_dispatch.c\""
              " -o \"${PREFIX}_dispatch.o\"\n";
    script << "# Compile vertex loader AOT files if present\n";
    script << "if [[ -f \"${PREFIX}_vtx_loaders.c\" ]]; then\n";
    script << "    echo \"Compiling vertex loaders...\"\n";
    script << "    clang -c $DISPATCH_CFLAGS -I. \"${PREFIX}_vtx_loaders.c\""
              " -o \"${PREFIX}_vtx_loaders.o\"\n";
    script << "    clang -c $DISPATCH_CFLAGS -I. \"${PREFIX}_vtx_dispatch.c\""
              " -o \"${PREFIX}_vtx_dispatch.o\"\n";
    script << "fi\n\n";
    script << "echo \"Creating static library...\"\n";
    script << "ar rcs lib${PREFIX}_aot.a ${PREFIX}_*.o\n\n";
    script << "echo \"Done: lib${PREFIX}_aot.a\"\n";
    script << "echo \"Rebuild Dolphin with LTO (single game):\"\n";
    script << "echo \"  cmake .. -DENABLE_LTO=ON"
              " -DAOT_STATIC_LIB=$(pwd)/lib${PREFIX}_aot.a\"\n";
    script << "echo \"Or add to multi-game build (semicolon-separated):\"\n";
    script << "echo \"  cmake .. -DENABLE_LTO=ON"
              " -DAOT_STATIC_LIBS=\\\"/path/to/lib1.a;$(pwd)/lib${PREFIX}_aot.a\\\"\"\n";
    script.close();
    chmod(build_script.c_str(), 0755);
    fmt::println(std::cerr, "  Build script: {}", build_script);
  }

  // 7. Report results
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
