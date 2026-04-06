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
extern uint32_t aot_read_u16_se(AOTState* s, uint32_t addr);  // sign-extended half
extern uint32_t aot_read_u32(AOTState* s, uint32_t addr);
extern uint64_t aot_read_u64(AOTState* s, uint32_t addr);
extern void aot_write_u8(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u16(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u16_br(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u32(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u64(AOTState* s, uint64_t val, uint32_t addr);
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

// ============================================================================
// FP fast-path inlines: common-case arithmetic without interpreter dispatch.
// Falls back to runtime helpers for NaN/Inf edge cases.
// The ps[] fields store raw uint64_t bit patterns of doubles.
// Single-precision ops round to float then "fill" both PS0 and PS1.
// ============================================================================

// Helper: reinterpret uint64_t <-> double without aliasing issues
static inline double aot_u64_to_f64(uint64_t u) { double d; __builtin_memcpy(&d, &u, 8); return d; }
static inline uint64_t aot_f64_to_u64(double d) { uint64_t u; __builtin_memcpy(&u, &d, 8); return u; }

// Update FPSCR FPRF field from a single-precision result.
// FPRF (bits 12-16 PPC = mask 0x0001F000): C, FL, FG, FE, FU
// Fast path only handles non-NaN (NaN goes to slow path), so no FU case.
// FPSCR bits: FX(0) XX(6) FI(14) FR(13) are NOT updated by the fast path —
// only FPRF is set here, matching what matters for control flow (mcrfs, mffs).
static inline void aot_update_fprf_single(AOTState* s, float result) {
    uint32_t fprf;
    if (result == 0.0f)
        fprf = 0x00002000;  // FE: zero (ignores sign of zero — close enough)
    else if (result > 0.0f)
        fprf = 0x00004000;  // FG: positive
    else
        fprf = 0x00008000;  // FL: negative
    s->fpscr = (s->fpscr & ~0x0001F000u) | fprf;
}

// Single-precision fast paths (fadds, fsubs, fmuls)
// Fast path: result is not NaN -> store rounded float as double into both PS slots.
// Slow path: NaN/Inf -> full interpreter path for exact FPSCR handling.

static inline void aot_fast_faddsx(AOTState* s, int fd, int fa, int fb) {
    double a = aot_u64_to_f64(s->ps[fa].ps0);
    double b = aot_u64_to_f64(s->ps[fb].ps0);
    double sum = a + b;
    if (__builtin_expect(sum == sum, 1)) {
        float f = (float)sum;
        uint64_t r = aot_f64_to_u64((double)f);
        s->ps[fd].ps0 = r; s->ps[fd].ps1 = r;
        aot_update_fprf_single(s, f);
    } else { aot_faddsx(s, fd, fa, fb); }
}

static inline void aot_fast_fsubsx(AOTState* s, int fd, int fa, int fb) {
    double a = aot_u64_to_f64(s->ps[fa].ps0);
    double b = aot_u64_to_f64(s->ps[fb].ps0);
    double diff = a - b;
    if (__builtin_expect(diff == diff, 1)) {
        float f = (float)diff;
        uint64_t r = aot_f64_to_u64((double)f);
        s->ps[fd].ps0 = r; s->ps[fd].ps1 = r;
        aot_update_fprf_single(s, f);
    } else { aot_fsubsx(s, fd, fa, fb); }
}

static inline void aot_fast_fmulsx(AOTState* s, int fd, int fa, int fc) {
    double a = aot_u64_to_f64(s->ps[fa].ps0);
    double c = aot_u64_to_f64(s->ps[fc].ps0);
    double prod = a * c;
    if (__builtin_expect(prod == prod, 1)) {
        float f = (float)prod;
        uint64_t r = aot_f64_to_u64((double)f);
        s->ps[fd].ps0 = r; s->ps[fd].ps1 = r;
        aot_update_fprf_single(s, f);
    } else { aot_fmulsx(s, fd, fa, fc); }
}

// Paired-singles fast paths (ps_add, ps_sub, ps_mul, ps_madd, ps_msub)
// Operate on both PS0 and PS1 independently. FPRF reflects PS1 result per PPC spec.

static inline void aot_fast_ps_add(AOTState* s, int fd, int fa, int fb) {
    double a0 = aot_u64_to_f64(s->ps[fa].ps0), a1 = aot_u64_to_f64(s->ps[fa].ps1);
    double b0 = aot_u64_to_f64(s->ps[fb].ps0), b1 = aot_u64_to_f64(s->ps[fb].ps1);
    double r0 = a0 + b0, r1 = a1 + b1;
    if (__builtin_expect(r0 == r0 && r1 == r1, 1)) {
        float f1 = (float)r1;
        s->ps[fd].ps0 = aot_f64_to_u64((double)(float)r0);
        s->ps[fd].ps1 = aot_f64_to_u64((double)f1);
        aot_update_fprf_single(s, f1);
    } else { aot_ps_add(s, fd, fa, fb); }
}

static inline void aot_fast_ps_sub(AOTState* s, int fd, int fa, int fb) {
    double a0 = aot_u64_to_f64(s->ps[fa].ps0), a1 = aot_u64_to_f64(s->ps[fa].ps1);
    double b0 = aot_u64_to_f64(s->ps[fb].ps0), b1 = aot_u64_to_f64(s->ps[fb].ps1);
    double r0 = a0 - b0, r1 = a1 - b1;
    if (__builtin_expect(r0 == r0 && r1 == r1, 1)) {
        float f1 = (float)r1;
        s->ps[fd].ps0 = aot_f64_to_u64((double)(float)r0);
        s->ps[fd].ps1 = aot_f64_to_u64((double)f1);
        aot_update_fprf_single(s, f1);
    } else { aot_ps_sub(s, fd, fa, fb); }
}

static inline void aot_fast_ps_mul(AOTState* s, int fd, int fa, int fc) {
    double a0 = aot_u64_to_f64(s->ps[fa].ps0), a1 = aot_u64_to_f64(s->ps[fa].ps1);
    double c0 = aot_u64_to_f64(s->ps[fc].ps0), c1 = aot_u64_to_f64(s->ps[fc].ps1);
    double r0 = a0 * c0, r1 = a1 * c1;
    if (__builtin_expect(r0 == r0 && r1 == r1, 1)) {
        float f1 = (float)r1;
        s->ps[fd].ps0 = aot_f64_to_u64((double)(float)r0);
        s->ps[fd].ps1 = aot_f64_to_u64((double)f1);
        aot_update_fprf_single(s, f1);
    } else { aot_ps_mul(s, fd, fa, fc); }
}

// ps_madd: fd = fa * fc + fb (both slots, single-precision result)
static inline void aot_fast_ps_madd(AOTState* s, int fd, int fa, int fc, int fb) {
    double a0 = aot_u64_to_f64(s->ps[fa].ps0), a1 = aot_u64_to_f64(s->ps[fa].ps1);
    double c0 = aot_u64_to_f64(s->ps[fc].ps0), c1 = aot_u64_to_f64(s->ps[fc].ps1);
    double b0 = aot_u64_to_f64(s->ps[fb].ps0), b1 = aot_u64_to_f64(s->ps[fb].ps1);
    double r0 = a0 * c0 + b0, r1 = a1 * c1 + b1;
    if (__builtin_expect(r0 == r0 && r1 == r1, 1)) {
        float f1 = (float)r1;
        s->ps[fd].ps0 = aot_f64_to_u64((double)(float)r0);
        s->ps[fd].ps1 = aot_f64_to_u64((double)f1);
        aot_update_fprf_single(s, f1);
    } else { aot_ps_madd(s, fd, fa, fc, fb); }
}

// ps_msub: fd = fa * fc - fb (both slots, single-precision result)
static inline void aot_fast_ps_msub(AOTState* s, int fd, int fa, int fc, int fb) {
    double a0 = aot_u64_to_f64(s->ps[fa].ps0), a1 = aot_u64_to_f64(s->ps[fa].ps1);
    double c0 = aot_u64_to_f64(s->ps[fc].ps0), c1 = aot_u64_to_f64(s->ps[fc].ps1);
    double b0 = aot_u64_to_f64(s->ps[fb].ps0), b1 = aot_u64_to_f64(s->ps[fb].ps1);
    double r0 = a0 * c0 - b0, r1 = a1 * c1 - b1;
    if (__builtin_expect(r0 == r0 && r1 == r1, 1)) {
        float f1 = (float)r1;
        s->ps[fd].ps0 = aot_f64_to_u64((double)(float)r0);
        s->ps[fd].ps1 = aot_f64_to_u64((double)f1);
        aot_update_fprf_single(s, f1);
    } else { aot_ps_msub(s, fd, fa, fc, fb); }
}

// Single-block mode flag (set by compare/diff harness to stop block chaining)
extern int aot_single_block_mode;

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
    // Build edge map: block_addr -> set of target addresses
    std::unordered_map<u32, std::vector<u32>> block_edges;
    for (const auto& e : cfg_edges)
      block_edges[e.from_addr].push_back(e.to_addr);

    u32 smc_skipped = 0;
    for (auto& b : cfg_blocks)
    {
      if (!b.is_translatable || b.num_instructions == 0)
        continue;

      u32 fall_through_addr = b.ppc_addr + b.num_instructions * 4;
      auto edge_it = block_edges.find(b.ppc_addr);

      // Check if any edge targets a non-sequential address (i.e., a branch)
      bool has_non_sequential_edge = false;
      if (edge_it != block_edges.end())
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

  // Write forward declarations header
  {
    std::ofstream fwd(output_dir + "/" + prefix + "_forward_decls.h");
    fwd << "#ifndef " << prefix << "_FORWARD_DECLS_H\n";
    fwd << "#define " << prefix << "_FORWARD_DECLS_H\n";
    fwd << "#include \"aot_runtime.h\"\n\n";
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

  // 5. Write dispatch table — flat direct-mapped array for O(1) lookup
  {
    // Find the address range of all translatable blocks
    u32 min_addr = UINT32_MAX, max_addr = 0;
    for (const auto& b : cfg_blocks)
    {
      if (b.is_translatable)
      {
        min_addr = std::min(min_addr, b.ppc_addr);
        max_addr = std::max(max_addr, b.ppc_addr);
      }
    }
    // Align base to 4-byte boundary
    min_addr &= ~3u;
    u32 table_entries = ((max_addr - min_addr) >> 2) + 1;

    fmt::println(std::cerr, "  Dispatch table: {:#010x}-{:#010x} ({} entries, {:.1f} MB)",
                 min_addr, max_addr, table_entries,
                 table_entries * sizeof(void*) / (1024.0 * 1024.0));

    std::string dispatch_file = fmt::format("{}/{}_dispatch.c", output_dir, prefix);
    std::ofstream file(dispatch_file);
    file << "#include \"aot_runtime.h\"\n";
    file << fmt::format("#include \"{}_forward_decls.h\"\n\n", prefix);

    file << "typedef void (*AOTBlockFunc)(AOTState*);\n\n";

    // Emit the flat lookup table
    file << fmt::format("#define {}_TABLE_BASE {:#010x}u\n", prefix, min_addr);
    file << fmt::format("#define {}_TABLE_SIZE {}u\n\n", prefix, table_entries);

    file << fmt::format("static AOTBlockFunc {}_fast_table[{}] = {{\n", prefix, table_entries);

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
    file << "    if (aot_single_block_mode) return;\n";
    file << "    uint32_t pc = s->pc;\n";
    file << fmt::format("    uint32_t idx = (pc - {}_TABLE_BASE) >> 2;\n", prefix);
    file << fmt::format("    if (idx < {}_TABLE_SIZE) {{\n", prefix);
    file << fmt::format("        AOTBlockFunc fn = {}_fast_table[idx];\n", prefix);
    file << "        if (fn) { [[clang::musttail]] return fn(s); }\n";
    file << "    }\n";
    file << "    [[clang::musttail]] return aot_interpreter_single_step(s);\n";
    file << "}\n\n";

    // Emit a block lookup function for the diff harness — returns a single
    // block's function pointer without executing it or tail-calling.
    file << fmt::format("AOTBlockFunc {}_lookup_block(uint32_t pc) {{\n", prefix);
    file << fmt::format("    uint32_t idx = (pc - {}_TABLE_BASE) >> 2;\n", prefix);
    file << fmt::format("    if (idx < {}_TABLE_SIZE) return {}_fast_table[idx];\n", prefix,
                        prefix);
    file << "    return 0;\n";
    file << "}\n\n";

    // Emit self-registration constructor — runs before main() to register
    // this game's dispatch/lookup with Dolphin's AotRegistry.
    file << "__attribute__((constructor))\n";
    file << fmt::format("static void aot_register_{}(void) {{\n", prefix);
    file << "    extern void aot_register_game(const char*,"
            " void (*)(AOTState*), AOTBlockFunc (*)(uint32_t));\n";
    file << fmt::format("    aot_register_game(\"{}\", {}_dispatch, {}_lookup_block);\n", prefix,
                        prefix, prefix);
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
    script << "BLOCK_CFLAGS=\"-Os -flto=thin -arch arm64 -mcpu=apple-a14 -moutline\"\n";
    script << "DISPATCH_CFLAGS=\"-O2 -flto=thin -arch arm64 -mcpu=apple-a14\"\n\n";
    script << "echo \"Compiling AOT blocks with LTO...\"\n";
    script << "for f in ${PREFIX}_blocks_*.c; do\n";
    script << "    clang -c $BLOCK_CFLAGS -I. \"$f\" -o \"${f%.c}.o\" &\n";
    script << "done\n";
    script << "clang -c $DISPATCH_CFLAGS -I. \"${PREFIX}_dispatch.c\""
              " -o \"${PREFIX}_dispatch.o\" &\n";
    script << "wait\n\n";
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
