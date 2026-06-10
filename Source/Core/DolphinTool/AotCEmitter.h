// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Gekko.h"
#include "DolphinTool/PPCMemoryImage.h"

namespace DolphinTool
{

// Module mode: translating a relocatable .rel module instead of the DOL.
// Block addresses are synthetic (section << 24) | offset coordinates; runtime
// addresses are computed from a per-module section base array written by the
// module tracker when the game loads the module.
struct ModuleBranchOverride
{
  enum Kind
  {
    Absolute,  // target is an absolute DOL address (relocation to module 0)
    Local,     // target is a synthetic (section, offset) in this same module
    External,  // target is another module — interpreter-fallback the instruction
  };
  Kind kind;
  u32 target;
};

struct ModuleImmReloc
{
  u8 type;                 // R_PPC_ADDR16_LO / _HI / _HA (own-module targets only;
                           // DOL targets are pre-patched into the instruction words)
  std::string target_expr;  // e.g. "(GZLE01_m042_base[4]+0x1234u)"
};

struct ModuleMode
{
  std::string fn_prefix;   // e.g. "GZLE01_m042"
  std::string base_array;  // e.g. "GZLE01_m042_base"
  const std::set<u32>* dol_blocks;  // translatable DOL blocks (absolute addresses)
  std::unordered_map<u32, ModuleImmReloc> imm_relocs;            // synth pc -> reloc
  std::unordered_map<u32, ModuleBranchOverride> branch_overrides;  // synth pc -> target
  // Sites that must single-step the relocated in-RAM instruction (cross-module
  // immediates and exotic relocation types — a handful per game).
  std::unordered_set<u32> force_fallback;
  // Per-section sizes, used to reject branch targets computed from raw
  // displacements in misdisassembled (dead-code) blocks.
  std::vector<u32> section_sizes;
};

// Translates PPC basic blocks into C source code for AOT compilation.
// Each PPC block becomes a C function operating on an AOTState struct
// that is layout-compatible with Dolphin's PowerPCState.
class AOTCEmitter
{
public:
  AOTCEmitter(const PPCMemoryImage& memory, std::set<u32> known_blocks, std::string prefix);

  // Switches the emitter to module mode (mode must outlive the emitter; pass
  // nullptr to return to DOL mode). In module mode m_known_blocks holds the
  // module's synthetic block addresses.
  void SetModuleMode(const ModuleMode* mode, std::set<u32> module_blocks);

  // Translate a single block. Returns the C function body as a string.
  // If from_trace is true, the block was observed during trace collection (hot).
  std::string TranslateBlock(u32 block_addr, u32 num_instructions, bool from_trace = true);

  // Get the set of unhandled opcodes encountered during translation.
  const std::map<std::string, u32>& GetUnhandledOpcodes() const { return m_unhandled_opcodes; }

private:
  // Emit a single PPC instruction as C code. Returns false if unhandled.
  bool EmitInstruction(std::string& out, UGeckoInstruction inst, u32 pc, bool is_block_end);

  // Integer arithmetic
  void EmitAddi(std::string& out, UGeckoInstruction inst);
  void EmitAddis(std::string& out, UGeckoInstruction inst);
  void EmitAddic(std::string& out, UGeckoInstruction inst, bool rc);
  void EmitMulli(std::string& out, UGeckoInstruction inst);
  void EmitSubfic(std::string& out, UGeckoInstruction inst);
  void EmitOri(std::string& out, UGeckoInstruction inst);
  void EmitOris(std::string& out, UGeckoInstruction inst);
  void EmitXori(std::string& out, UGeckoInstruction inst);
  void EmitXoris(std::string& out, UGeckoInstruction inst);
  void EmitAndi_rc(std::string& out, UGeckoInstruction inst);
  void EmitAndis_rc(std::string& out, UGeckoInstruction inst);

  // Integer register-register
  void EmitAddx(std::string& out, UGeckoInstruction inst);
  void EmitAddcx(std::string& out, UGeckoInstruction inst);
  void EmitAddex(std::string& out, UGeckoInstruction inst);
  void EmitAddmex(std::string& out, UGeckoInstruction inst);
  void EmitAddzex(std::string& out, UGeckoInstruction inst);
  void EmitSubfzex(std::string& out, UGeckoInstruction inst);
  void EmitSubfx(std::string& out, UGeckoInstruction inst);
  void EmitSubfcx(std::string& out, UGeckoInstruction inst);
  void EmitSubfex(std::string& out, UGeckoInstruction inst);
  void EmitNegx(std::string& out, UGeckoInstruction inst);
  void EmitMullwx(std::string& out, UGeckoInstruction inst);
  void EmitMulhwx(std::string& out, UGeckoInstruction inst);
  void EmitMulhwux(std::string& out, UGeckoInstruction inst);
  void EmitDivwx(std::string& out, UGeckoInstruction inst);
  void EmitDivwux(std::string& out, UGeckoInstruction inst);

  // Logical
  void EmitLogical(std::string& out, UGeckoInstruction inst, const char* op);
  void EmitAndcx(std::string& out, UGeckoInstruction inst);
  void EmitOrcx(std::string& out, UGeckoInstruction inst);
  void EmitNandx(std::string& out, UGeckoInstruction inst);
  void EmitNorx(std::string& out, UGeckoInstruction inst);
  void EmitEqvx(std::string& out, UGeckoInstruction inst);

  // Comparison
  void EmitCmpi(std::string& out, UGeckoInstruction inst);
  void EmitCmpli(std::string& out, UGeckoInstruction inst);
  void EmitCmp(std::string& out, UGeckoInstruction inst);
  void EmitCmpl(std::string& out, UGeckoInstruction inst);

  // Shift/rotate
  void EmitRlwinmx(std::string& out, UGeckoInstruction inst);
  void EmitRlwimix(std::string& out, UGeckoInstruction inst);
  void EmitRlwnmx(std::string& out, UGeckoInstruction inst);
  void EmitSlwx(std::string& out, UGeckoInstruction inst);
  void EmitSrwx(std::string& out, UGeckoInstruction inst);
  void EmitSrawx(std::string& out, UGeckoInstruction inst);
  void EmitSrawix(std::string& out, UGeckoInstruction inst);

  // Misc integer
  void EmitCntlzwx(std::string& out, UGeckoInstruction inst);
  void EmitExtsbx(std::string& out, UGeckoInstruction inst);
  void EmitExtshx(std::string& out, UGeckoInstruction inst);

  // Opcode table dispatch
  bool EmitTable31(std::string& out, UGeckoInstruction inst, u32 pc);
  bool EmitTable19(std::string& out, UGeckoInstruction inst, u32 pc);
  bool EmitTable59(std::string& out, UGeckoInstruction inst, u32 pc);
  bool EmitTable63(std::string& out, UGeckoInstruction inst, u32 pc);
  bool EmitTable4(std::string& out, UGeckoInstruction inst, u32 pc);

  // Load/store integer
  void EmitLoadInt(std::string& out, UGeckoInstruction inst, const char* helper, bool update,
                   bool indexed);
  void EmitStoreInt(std::string& out, UGeckoInstruction inst, const char* helper, bool update,
                    bool indexed);
  void EmitLmw(std::string& out, UGeckoInstruction inst);
  void EmitStmw(std::string& out, UGeckoInstruction inst);

  // Load/store FP
  void EmitLfs(std::string& out, UGeckoInstruction inst, bool update, bool indexed);
  void EmitLfd(std::string& out, UGeckoInstruction inst, bool update, bool indexed);
  void EmitStfs(std::string& out, UGeckoInstruction inst, bool update, bool indexed);
  void EmitStfd(std::string& out, UGeckoInstruction inst, bool update, bool indexed);

  // System/SPR
  void EmitMfspr(std::string& out, UGeckoInstruction inst);
  void EmitMtspr(std::string& out, UGeckoInstruction inst);
  void EmitMfcr(std::string& out, UGeckoInstruction inst);
  void EmitMtcrf(std::string& out, UGeckoInstruction inst);
  void EmitMcrxr(std::string& out, UGeckoInstruction inst);
  void EmitMfmsr(std::string& out, UGeckoInstruction inst);
  void EmitMtmsr(std::string& out, UGeckoInstruction inst, u32 pc);
  void EmitMfsr(std::string& out, UGeckoInstruction inst);
  void EmitMtsr(std::string& out, UGeckoInstruction inst);
  void EmitTwi(std::string& out, UGeckoInstruction inst, u32 pc);
  void EmitCrLogical(std::string& out, UGeckoInstruction inst, const char* op);
  void EmitMcrf(std::string& out, UGeckoInstruction inst);

  // Branches
  void EmitBx(std::string& out, UGeckoInstruction inst, u32 pc);
  void EmitBcx(std::string& out, UGeckoInstruction inst, u32 pc);
  void EmitBcctrx(std::string& out, UGeckoInstruction inst, u32 pc);
  void EmitBclrx(std::string& out, UGeckoInstruction inst, u32 pc);

  // Helpers emitted inline
  void EmitUpdateCR0(std::string& out, const char* result_expr);
  void EmitSetCarry(std::string& out, const char* expr);
  void EmitOECheck(std::string& out, const char* a, const char* b, const char* result);
  void EmitBranchTo(std::string& out, u32 target, u32 current_pc, bool target_is_dol = false);
  void EmitIndirectDispatch(std::string& out);

  // Module-mode formatting: PcStr/BlockFn/DispExpr produce byte-identical output
  // to the original literals in DOL mode.
  std::string PcStr(u32 pc) const;                    // "0x80003100u" or base-relative expr
  std::string BlockFn(u32 addr, bool is_dol) const;   // block function symbol for addr
  std::string Field16Expr() const;                    // patched 16-bit field of m_cur_imm
  std::string DispExpr(s32 offset) const;             // D-form displacement or LO reloc expr
  // Emits "pc = <site>; interpreter_single_step;" for instructions we cannot
  // translate in module mode (external-module relocs, relocs on unexpected
  // opcodes). RAM holds the relocated instruction, so this is always correct.
  void EmitModuleFallback(std::string& out, u32 pc);

  const PPCMemoryImage& m_memory;
  std::set<u32> m_known_blocks;
  std::string m_prefix;
  u32 m_block_cycle_count = 0;
  std::map<std::string, u32> m_unhandled_opcodes;

  const ModuleMode* m_module = nullptr;
  const ModuleImmReloc* m_cur_imm = nullptr;          // reloc on the current instruction
  const ModuleBranchOverride* m_cur_branch = nullptr;  // branch override on current inst
};

}  // namespace DolphinTool
