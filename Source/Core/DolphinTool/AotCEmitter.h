// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Gekko.h"
#include "DolphinTool/PPCMemoryImage.h"

namespace DolphinTool
{

// Translates PPC basic blocks into C source code for AOT compilation.
// Each PPC block becomes a C function operating on an AOTState struct
// that is layout-compatible with Dolphin's PowerPCState.
class AOTCEmitter
{
public:
  AOTCEmitter(const PPCMemoryImage& memory, std::set<u32> known_blocks, std::string prefix);

  // Translate a single block. Returns the C function body as a string.
  std::string TranslateBlock(u32 block_addr, u32 num_instructions);

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

  // Branches
  void EmitBx(std::string& out, UGeckoInstruction inst, u32 pc);
  void EmitBcx(std::string& out, UGeckoInstruction inst, u32 pc);
  void EmitBcctrx(std::string& out, UGeckoInstruction inst, u32 pc);
  void EmitBclrx(std::string& out, UGeckoInstruction inst, u32 pc);

  // Helpers emitted inline
  void EmitUpdateCR0(std::string& out, const char* result_expr);
  void EmitSetCarry(std::string& out, const char* expr);
  void EmitOECheck(std::string& out, const char* a, const char* b, const char* result);
  void EmitBranchTo(std::string& out, u32 target);
  void EmitIndirectDispatch(std::string& out);

  const PPCMemoryImage& m_memory;
  std::set<u32> m_known_blocks;
  std::string m_prefix;
  u32 m_block_cycle_count = 0;
  std::map<std::string, u32> m_unhandled_opcodes;
};

}  // namespace DolphinTool
