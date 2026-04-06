// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/AotCEmitter.h"

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCTables.h"
#include "DolphinTool/PPCMemoryImage.h"

// Unary + promotes bitfields to int, allowing fmt::format to bind them.
#define I(x) (+(x))

namespace DolphinTool
{

AOTCEmitter::AOTCEmitter(const PPCMemoryImage& memory, std::set<u32> known_blocks,
                         std::string prefix)
    : m_memory(memory), m_known_blocks(std::move(known_blocks)), m_prefix(std::move(prefix))
{
}

std::string AOTCEmitter::TranslateBlock(u32 block_addr, u32 num_instructions, bool from_trace)
{
  std::string out;
  m_block_cycle_count = 0;
  const char* section = from_trace ? "__TEXT,__aot_hot" : "__TEXT,__aot_cold";
  out += fmt::format("__attribute__((noinline, section(\"{}\")))"
                     " void {}_block_{:08x}(AOTState* s) {{\n",
                     section, m_prefix, block_addr);

  for (u32 i = 0; i < num_instructions; i++)
  {
    u32 pc = block_addr + i * 4;
    auto inst_word = m_memory.ReadInstruction(pc);
    if (!inst_word)
      break;

    UGeckoInstruction inst(*inst_word);
    const GekkoOPInfo* info = PPCTables::GetOpInfo(inst, pc);
    if (info)
      m_block_cycle_count += info->num_cycles;

    bool is_last = (i == num_instructions - 1);
    if (!EmitInstruction(out, inst, pc, is_last))
    {
      std::string opname = info ? info->opname : "unknown";
      m_unhandled_opcodes[opname]++;
      out += fmt::format("    s->pc = {:#010x}u; aot_interpreter_single_step(s);\n", pc);
    }

  }

  // Always emit a fallthrough exit at the end of every block.
  // Branch instructions that are taken will `return` before reaching this.
  // This handles: blocks without branches, conditional branches not taken,
  // and any other case where execution falls through.
  {
    u32 next_pc = block_addr + num_instructions * 4;
    out += fmt::format("    s->downcount -= {};\n", m_block_cycle_count);
    out += fmt::format("    s->pc = {:#010x}u;\n", next_pc);
  }

  out += "}\n";
  return out;
}

bool AOTCEmitter::EmitInstruction(std::string& out, UGeckoInstruction inst, u32 pc, bool is_last)
{
  switch (I(inst.OPCD))
  {
  case 14: EmitAddi(out, inst); return true;
  case 15: EmitAddis(out, inst); return true;
  case 12: EmitAddic(out, inst, false); return true;
  case 13: EmitAddic(out, inst, true); return true;
  case 7:  EmitMulli(out, inst); return true;
  case 8:  EmitSubfic(out, inst); return true;
  case 24: EmitOri(out, inst); return true;
  case 25: EmitOris(out, inst); return true;
  case 26: EmitXori(out, inst); return true;
  case 27: EmitXoris(out, inst); return true;
  case 28: EmitAndi_rc(out, inst); return true;
  case 29: EmitAndis_rc(out, inst); return true;
  case 11: EmitCmpi(out, inst); return true;
  case 10: EmitCmpli(out, inst); return true;
  case 20: EmitRlwimix(out, inst); return true;
  case 21: EmitRlwinmx(out, inst); return true;
  case 23: EmitRlwnmx(out, inst); return true;
  case 18: EmitBx(out, inst, pc); return true;
  case 16: EmitBcx(out, inst, pc); return true;
  case 3:  EmitTwi(out, inst, pc); return true;  // twi

  // Load integer
  case 32: EmitLoadInt(out, inst, "aot_read_u32", false, false); return true; // lwz
  case 33: EmitLoadInt(out, inst, "aot_read_u32", true, false); return true;  // lwzu
  case 40: EmitLoadInt(out, inst, "aot_read_u16", false, false); return true; // lhz
  case 41: EmitLoadInt(out, inst, "aot_read_u16", true, false); return true;  // lhzu
  case 42: EmitLoadInt(out, inst, "aot_read_u16_se", false, false); return true; // lha
  case 43: EmitLoadInt(out, inst, "aot_read_u16_se", true, false); return true;  // lhau
  case 34: EmitLoadInt(out, inst, "aot_read_u8", false, false); return true;  // lbz
  case 35: EmitLoadInt(out, inst, "aot_read_u8", true, false); return true;   // lbzu
  // Store integer
  case 36: EmitStoreInt(out, inst, "aot_write_u32", false, false); return true; // stw
  case 37: EmitStoreInt(out, inst, "aot_write_u32", true, false); return true;  // stwu
  case 44: EmitStoreInt(out, inst, "aot_write_u16", false, false); return true; // sth
  case 45: EmitStoreInt(out, inst, "aot_write_u16", true, false); return true;  // sthu
  case 38: EmitStoreInt(out, inst, "aot_write_u8", false, false); return true;  // stb
  case 39: EmitStoreInt(out, inst, "aot_write_u8", true, false); return true;   // stbu
  // Load/store multiple
  case 46: EmitLmw(out, inst); return true;
  case 47: EmitStmw(out, inst); return true;
  // FP/PS instructions — check MSR.FP first (FPU unavailable exception if FP=0)
  case 48: case 49: case 50: case 51:  // lfs, lfsu, lfd, lfdu
  case 52: case 53: case 54: case 55:  // stfs, stfsu, stfd, stfdu
  case 56: case 57: case 60: case 61:  // psq_l, psq_lu, psq_st, psq_stu
  case 59: case 63: case 4:            // FP arith, PS arith
  {
    out += fmt::format("    if(!aot_check_fpu(s,{:#010x}u)) {{ s->downcount-={}; return; }}\n",
                       pc, m_block_cycle_count);
    switch (I(inst.OPCD))
    {
    case 48: EmitLfs(out, inst, false, false); return true;
    case 49: EmitLfs(out, inst, true, false); return true;
    case 50: EmitLfd(out, inst, false, false); return true;
    case 51: EmitLfd(out, inst, true, false); return true;
    case 52: EmitStfs(out, inst, false, false); return true;
    case 53: EmitStfs(out, inst, true, false); return true;
    case 54: EmitStfd(out, inst, false, false); return true;
    case 55: EmitStfd(out, inst, true, false); return true;
    case 56: out += fmt::format("    aot_psq_l(s,{},{},{});\n", I(inst.RD), I(inst.RA), inst.hex); return true;
    case 57: out += fmt::format("    aot_psq_lu(s,{},{},{});\n", I(inst.RD), I(inst.RA), inst.hex); return true;
    case 60: out += fmt::format("    aot_psq_st(s,{},{},{});\n", I(inst.RS), I(inst.RA), inst.hex); return true;
    case 61: out += fmt::format("    aot_psq_stu(s,{},{},{});\n", I(inst.RS), I(inst.RA), inst.hex); return true;
    case 59: return EmitTable59(out, inst, pc);
    case 63: return EmitTable63(out, inst, pc);
    case 4:  return EmitTable4(out, inst, pc);
    default: return false;
    }
  }
  // Sync/isync (no-ops)
  case 17:
    out += fmt::format("    s->downcount-={}; s->pc={:#010x}u; s->npc=s->pc; aot_sc(s); return;\n",
                       m_block_cycle_count, pc + 4);
    return true; // sc
  case 46 + 128: return true; // placeholder

  case 31: return EmitTable31(out, inst, pc);
  case 19: return EmitTable19(out, inst, pc);
  default: return false;
  }
}

// ============================================================================
// Immediate integer
// ============================================================================

void AOTCEmitter::EmitAddi(std::string& out, UGeckoInstruction inst)
{
  u32 rd = I(inst.RD), ra = I(inst.RA);
  u32 imm = u32(s32(s16(inst.SIMM_16)));
  if (ra)
    out += fmt::format("    s->gpr[{}] = s->gpr[{}] + {:#x}u;\n", rd, ra, imm);
  else
    out += fmt::format("    s->gpr[{}] = {:#x}u;\n", rd, imm);
}

void AOTCEmitter::EmitAddis(std::string& out, UGeckoInstruction inst)
{
  u32 rd = I(inst.RD), ra = I(inst.RA);
  u32 imm = u32(s32(s16(inst.SIMM_16)) << 16);
  if (ra)
    out += fmt::format("    s->gpr[{}] = s->gpr[{}] + {:#x}u;\n", rd, ra, imm);
  else
    out += fmt::format("    s->gpr[{}] = {:#x}u;\n", rd, imm);
}

void AOTCEmitter::EmitAddic(std::string& out, UGeckoInstruction inst, bool rc)
{
  u32 rd = I(inst.RD), ra = I(inst.RA);
  u32 imm = u32(s32(s16(inst.SIMM_16)));
  out += fmt::format("    {{ uint32_t a=s->gpr[{}], imm={:#x}u, r=a+imm; s->gpr[{}]=r; "
                     "s->xer_ca=(imm>(~a));", ra, imm, rd);
  if (rc) out += " { int64_t se=(int64_t)(int32_t)r; uint64_t cr=(uint64_t)se; if(r==0) cr|=1ULL<<63; cr=(cr&~(1ULL<<59))|((uint64_t)(s->xer_so_ov>>1)<<59); s->cr_fields[0]=cr; }";
  out += " }\n";
}

void AOTCEmitter::EmitMulli(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->gpr[{}]=(uint32_t)((int32_t)s->gpr[{}]*(int32_t){});\n",
                     I(inst.RD), I(inst.RA), s32(s16(inst.SIMM_16)));
}

void AOTCEmitter::EmitSubfic(std::string& out, UGeckoInstruction inst)
{
  u32 rd = I(inst.RD), ra = I(inst.RA);
  s32 imm = s32(s16(inst.SIMM_16));
  out += fmt::format("    {{ uint32_t a=s->gpr[{}]; s->gpr[{}]=(uint32_t)({}-"
                     "(int32_t)a); s->xer_ca=(a==0)||({:#x}u>(~(0u-a))); }}\n",
                     ra, rd, imm, u32(imm));
}

void AOTCEmitter::EmitOri(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]|{:#x}u;\n", I(inst.RA), I(inst.RS), u32(inst.UIMM));
}
void AOTCEmitter::EmitOris(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]|{:#x}u;\n", I(inst.RA), I(inst.RS), u32(inst.UIMM)<<16);
}
void AOTCEmitter::EmitXori(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]^{:#x}u;\n", I(inst.RA), I(inst.RS), u32(inst.UIMM));
}
void AOTCEmitter::EmitXoris(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]^{:#x}u;\n", I(inst.RA), I(inst.RS), u32(inst.UIMM)<<16);
}
void AOTCEmitter::EmitAndi_rc(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS);
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]&{:#x}u;\n", ra, rs, u32(inst.UIMM));
  EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}
void AOTCEmitter::EmitAndis_rc(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS);
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]&{:#x}u;\n", ra, rs, u32(inst.UIMM)<<16);
  EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

// ============================================================================
// Comparison
// ============================================================================

void AOTCEmitter::EmitCmpi(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    aot_cmp_signed(s,{},(int32_t)s->gpr[{}],(int32_t){});\n",
                     I(inst.CRFD), I(inst.RA), s32(s16(inst.SIMM_16)));
}
void AOTCEmitter::EmitCmpli(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    aot_cmp_unsigned(s,{},s->gpr[{}],{:#x}u);\n",
                     I(inst.CRFD), I(inst.RA), u32(inst.UIMM));
}
void AOTCEmitter::EmitCmp(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    aot_cmp_signed(s,{},(int32_t)s->gpr[{}],(int32_t)s->gpr[{}]);\n",
                     I(inst.CRFD), I(inst.RA), I(inst.RB));
}
void AOTCEmitter::EmitCmpl(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    aot_cmp_unsigned(s,{},s->gpr[{}],s->gpr[{}]);\n",
                     I(inst.CRFD), I(inst.RA), I(inst.RB));
}

// ============================================================================
// Rotate/shift
// ============================================================================

void AOTCEmitter::EmitRlwinmx(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS);
  out += fmt::format("    s->gpr[{}]=aot_rotl(s->gpr[{}],{})&aot_rotation_mask({},{});\n",
                     ra, rs, I(inst.SH), I(inst.MB), I(inst.ME));
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

void AOTCEmitter::EmitRlwimix(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS);
  out += fmt::format("    {{ uint32_t m=aot_rotation_mask({},{}); s->gpr[{}]="
                     "(s->gpr[{}]&~m)|(aot_rotl(s->gpr[{}],{})&m); }}\n",
                     I(inst.MB), I(inst.ME), ra, ra, rs, I(inst.SH));
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

void AOTCEmitter::EmitRlwnmx(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS), rb = I(inst.RB);
  out += fmt::format("    s->gpr[{}]=aot_rotl(s->gpr[{}],s->gpr[{}]&0x1f)&"
                     "aot_rotation_mask({},{});\n", ra, rs, rb, I(inst.MB), I(inst.ME));
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

void AOTCEmitter::EmitSlwx(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS), rb = I(inst.RB);
  out += fmt::format("    {{ uint32_t n=s->gpr[{}]; s->gpr[{}]=(n&0x20)?0:"
                     "(s->gpr[{}]<<(n&0x1f)); }}\n", rb, ra, rs);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

void AOTCEmitter::EmitSrwx(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS), rb = I(inst.RB);
  out += fmt::format("    {{ uint32_t n=s->gpr[{}]; s->gpr[{}]=(n&0x20)?0:"
                     "(s->gpr[{}]>>(n&0x1f)); }}\n", rb, ra, rs);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

void AOTCEmitter::EmitSrawx(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS), rb = I(inst.RB);
  out += fmt::format("    {{ uint32_t rb=s->gpr[{}]; int32_t rs=(int32_t)s->gpr[{}]; "
    "if(rb&0x20){{ s->gpr[{}]=(rs<0)?0xFFFFFFFFu:0; s->xer_ca=(rs<0); }}"
    "else{{ uint32_t n=rb&0x1f; s->gpr[{}]=(uint32_t)(rs>>n); "
    "s->xer_ca=(rs<0)&&(n>0)&&(((uint32_t)rs<<(32-n))!=0); }} }}\n",
    rb, rs, ra, ra);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

void AOTCEmitter::EmitSrawix(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS), sh = I(inst.SH);
  out += fmt::format("    {{ int32_t rs=(int32_t)s->gpr[{}]; s->gpr[{}]=(uint32_t)(rs>>{}); ",
                     rs, ra, sh);
  if (sh > 0)
    out += fmt::format("s->xer_ca=(rs<0)&&(((uint32_t)rs<<{})!=0); ", 32 - sh);
  else
    out += "s->xer_ca=0; ";
  out += "}\n";
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

// ============================================================================
// Misc integer
// ============================================================================

void AOTCEmitter::EmitCntlzwx(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS);
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]?__builtin_clz(s->gpr[{}]):32;\n", ra, rs, rs);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

void AOTCEmitter::EmitExtsbx(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS);
  out += fmt::format("    s->gpr[{}]=(uint32_t)(int32_t)(int8_t)s->gpr[{}];\n", ra, rs);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

void AOTCEmitter::EmitExtshx(std::string& out, UGeckoInstruction inst)
{
  u32 ra = I(inst.RA), rs = I(inst.RS);
  out += fmt::format("    s->gpr[{}]=(uint32_t)(int32_t)(int16_t)s->gpr[{}];\n", ra, rs);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

// ============================================================================
// Table 31 — register-register
// ============================================================================

bool AOTCEmitter::EmitTable31(std::string& out, UGeckoInstruction inst, u32 pc)
{
  switch (I(inst.SUBOP10))
  {
  case 266: EmitAddx(out, inst); return true;
  case 10:  EmitAddcx(out, inst); return true;
  case 138: EmitAddex(out, inst); return true;
  case 234: EmitAddmex(out, inst); return true;
  case 202: EmitAddzex(out, inst); return true;
  case 40:  EmitSubfx(out, inst); return true;
  case 8:   EmitSubfcx(out, inst); return true;
  case 136: EmitSubfex(out, inst); return true;
  case 104: EmitNegx(out, inst); return true;
  case 235: EmitMullwx(out, inst); return true;
  case 75:  EmitMulhwx(out, inst); return true;
  case 11:  EmitMulhwux(out, inst); return true;
  case 491: EmitDivwx(out, inst); return true;
  case 459: EmitDivwux(out, inst); return true;
  case 28:  EmitLogical(out, inst, "&"); return true;
  case 60:  EmitAndcx(out, inst); return true;
  case 444: EmitLogical(out, inst, "|"); return true;
  case 412: EmitOrcx(out, inst); return true;
  case 316: EmitLogical(out, inst, "^"); return true;
  case 476: EmitNandx(out, inst); return true;
  case 124: EmitNorx(out, inst); return true;
  case 284: EmitEqvx(out, inst); return true;
  case 24:  EmitSlwx(out, inst); return true;
  case 536: EmitSrwx(out, inst); return true;
  case 792: EmitSrawx(out, inst); return true;
  case 824: EmitSrawix(out, inst); return true;
  case 26:  EmitCntlzwx(out, inst); return true;
  case 954: EmitExtsbx(out, inst); return true;
  case 922: EmitExtshx(out, inst); return true;
  case 0:   EmitCmp(out, inst); return true;
  case 32:  EmitCmpl(out, inst); return true;
  // Indexed load/store integer
  case 23:  EmitLoadInt(out, inst, "aot_read_u32", false, true); return true;  // lwzx
  case 55:  EmitLoadInt(out, inst, "aot_read_u32", true, true); return true;   // lwzux
  case 279: EmitLoadInt(out, inst, "aot_read_u16", false, true); return true;  // lhzx
  case 311: EmitLoadInt(out, inst, "aot_read_u16", true, true); return true;   // lhzux
  case 343: EmitLoadInt(out, inst, "aot_read_u16_se", false, true); return true; // lhax
  case 375: EmitLoadInt(out, inst, "aot_read_u16_se", true, true); return true;  // lhaux
  case 87:  EmitLoadInt(out, inst, "aot_read_u8", false, true); return true;   // lbzx
  case 119: EmitLoadInt(out, inst, "aot_read_u8", true, true); return true;    // lbzux
  case 151: EmitStoreInt(out, inst, "aot_write_u32", false, true); return true; // stwx
  case 183: EmitStoreInt(out, inst, "aot_write_u32", true, true); return true;  // stwux
  case 407: EmitStoreInt(out, inst, "aot_write_u16", false, true); return true; // sthx
  case 439: EmitStoreInt(out, inst, "aot_write_u16", true, true); return true;  // sthux
  case 215: EmitStoreInt(out, inst, "aot_write_u8", false, true); return true;  // stbx
  case 247: EmitStoreInt(out, inst, "aot_write_u8", true, true); return true;   // stbux
  case 918: EmitStoreInt(out, inst, "aot_write_u16_br", false, true); return true; // sthbrx
  // Indexed load/store FP
  case 535: EmitLfs(out, inst, false, true); return true;   // lfsx
  case 567: EmitLfs(out, inst, true, true); return true;    // lfsux
  case 599: EmitLfd(out, inst, false, true); return true;   // lfdx
  case 631: EmitLfd(out, inst, true, true); return true;    // lfdux
  case 663: EmitStfs(out, inst, false, true); return true;  // stfsx
  case 695: EmitStfs(out, inst, true, true); return true;   // stfsux
  case 727: EmitStfd(out, inst, false, true); return true;  // stfdx
  case 759: EmitStfd(out, inst, true, true); return true;   // stfdux
  // SPR
  case 339: EmitMfspr(out, inst); return true;
  case 467: EmitMtspr(out, inst); return true;
  case 595: EmitMfsr(out, inst); return true;   // mfsr
  case 210: EmitMtsr(out, inst); return true;   // mtsr
  case 19:  EmitMfcr(out, inst); return true;
  case 144: EmitMtcrf(out, inst); return true;
  case 512: EmitMcrxr(out, inst); return true;  // mcrxr
  // Sync
  case 598: return true; // sync (no-op)
  case 854: return true; // eieio (no-op)
  // Cache
  case 278: out += fmt::format("    aot_dcbt(s,0);\n"); return true; // dcbt (no-op)
  case 246: out += fmt::format("    aot_dcbt(s,0);\n"); return true; // dcbtst (no-op)
  case 1014:
    if (I(inst.RA))
      out += fmt::format("    aot_dcbz(s,s->gpr[{}]+s->gpr[{}]);\n", I(inst.RA), I(inst.RB));
    else
      out += fmt::format("    aot_dcbz(s,s->gpr[{}]);\n", I(inst.RB));
    return true;
  case 86:  return true; // dcbf (no-op for AOT)
  case 54:  return true; // dcbst (no-op for AOT)
  case 470: return true; // dcbi (no-op for AOT)
  case 982: out += fmt::format("    aot_icbi(s,s->gpr[{}]+s->gpr[{}]);\n", I(inst.RA), I(inst.RB)); return true;
  // Missing arithmetic
  case 200: EmitSubfzex(out, inst); return true;  // subfzex
  // MSR
  case 83:  EmitMfmsr(out, inst); return true;
  case 146: EmitMtmsr(out, inst, pc); return true;
  // Timebase
  case 371: out += fmt::format("    s->gpr[{}]=aot_mftb(s,{});\n", I(inst.RD), I(inst.SPR)); return true;
  // stfiwx
  case 983: out += fmt::format("    aot_write_u32(s,(uint32_t)s->ps[{}].ps0,s->gpr[{}]+s->gpr[{}]);\n",
                               I(inst.RS), I(inst.RA), I(inst.RB)); return true;
  default:  return false;
  }
}

bool AOTCEmitter::EmitTable19(std::string& out, UGeckoInstruction inst, u32 pc)
{
  switch (I(inst.SUBOP10))
  {
  case 528: EmitBcctrx(out, inst, pc); return true;
  case 16:  EmitBclrx(out, inst, pc); return true;
  case 0:   EmitMcrf(out, inst); return true;
  case 33:  EmitCrLogical(out, inst, "~|"); return true; // crnor
  case 129: EmitCrLogical(out, inst, "&~"); return true; // crandc
  case 225: EmitCrLogical(out, inst, "~&"); return true; // crnand
  case 193: EmitCrLogical(out, inst, "^"); return true;  // crxor
  case 257: EmitCrLogical(out, inst, "&"); return true;  // crand
  case 289: EmitCrLogical(out, inst, "~^"); return true; // creqv (XNOR)
  case 417: EmitCrLogical(out, inst, "|~"); return true; // crorc
  case 449: EmitCrLogical(out, inst, "|"); return true;  // cror
  case 150: return true; // isync (no-op)
  case 50:  out += "    aot_rfi(s); return;\n"; return true; // rfi
  default:  return false;
  }
}

// ============================================================================
// Register-register arithmetic
// ============================================================================

#define ARITH_PROLOGUE(inst) \
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB); \
  out += fmt::format("    {{ uint32_t a=s->gpr[{}],b=s->gpr[{}],r;\n", ra, rb)

#define ARITH_STORE(inst) \
  out += fmt::format("    r={}; s->gpr[{}]=r;\n", "a+b", rd)

void AOTCEmitter::EmitAddx(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    {{ uint32_t a=s->gpr[{}],b=s->gpr[{}],r=a+b; s->gpr[{}]=r;", ra, rb, rd);
  if (inst.OE) out += " { uint32_t ov=(((a^r)&(b^r))>>31)!=0; s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, "r"); }
  out += " }\n";
}

void AOTCEmitter::EmitAddcx(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    {{ uint32_t a=s->gpr[{}],b=s->gpr[{}],r=a+b; s->gpr[{}]=r; s->xer_ca=(b>(~a));", ra, rb, rd);
  if (inst.OE) out += " { uint32_t ov=(((a^r)&(b^r))>>31)!=0; s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, "r"); }
  out += " }\n";
}

void AOTCEmitter::EmitAddex(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    {{ uint32_t c=s->xer_ca,a=s->gpr[{}],b=s->gpr[{}],r=a+b+c; s->gpr[{}]=r; s->xer_ca=(b>(~a))||(c&&(a+b==0xFFFFFFFFu));", ra, rb, rd);
  if (inst.OE) out += " { uint32_t ov=(((a^r)&(b^r))>>31)!=0; s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, "r"); }
  out += " }\n";
}

void AOTCEmitter::EmitAddmex(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA);
  out += fmt::format("    {{ uint32_t c=s->xer_ca,a=s->gpr[{}],r=a+0xFFFFFFFFu+c; s->gpr[{}]=r; s->xer_ca=((c-1)>(~a));", ra, rd);
  if (inst.OE) out += " { uint32_t b=0xFFFFFFFFu,ov=(((a^r)&(b^r))>>31)!=0; s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, "r"); }
  out += " }\n";
}

void AOTCEmitter::EmitAddzex(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA);
  out += fmt::format("    {{ uint32_t c=s->xer_ca,a=s->gpr[{}],r=a+c; s->gpr[{}]=r; s->xer_ca=(c>(~a));", ra, rd);
  if (inst.OE) out += " { uint32_t ov=(((a^r)&(0^r))>>31)!=0; s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, "r"); }
  out += " }\n";
}

void AOTCEmitter::EmitSubfzex(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA);
  // subfze: rD = ~rA + carry (subtract from zero extended)
  out += fmt::format("    {{ uint32_t c=s->xer_ca,a=~s->gpr[{}],r=a+c; s->gpr[{}]=r; s->xer_ca=(c>(~a));", ra, rd);
  if (inst.OE) out += " { uint32_t ov=(((a^r)&(0^r))>>31)!=0; s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, "r"); }
  out += " }\n";
}

void AOTCEmitter::EmitSubfx(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    {{ uint32_t a=s->gpr[{}],b=s->gpr[{}],r=b-a; s->gpr[{}]=r;", ra, rb, rd);
  if (inst.OE) out += " { uint32_t na=~a,ov=(((na^r)&(b^r))>>31)!=0; s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, "r"); }
  out += " }\n";
}

void AOTCEmitter::EmitSubfcx(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    {{ uint32_t a=s->gpr[{}],b=s->gpr[{}],r=b-a; s->gpr[{}]=r; s->xer_ca=(a==0)||(b>=a);", ra, rb, rd);
  if (inst.OE) out += " { uint32_t na=~a,ov=(((na^r)&(b^r))>>31)!=0; s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, "r"); }
  out += " }\n";
}

void AOTCEmitter::EmitSubfex(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    {{ uint32_t c=s->xer_ca,a=s->gpr[{}],b=s->gpr[{}],na=~a,r=na+b+c; s->gpr[{}]=r; s->xer_ca=(b>(~na))||(c&&(na+b==0xFFFFFFFFu));", ra, rb, rd);
  if (inst.OE) out += " { uint32_t ov=(((na^r)&(b^r))>>31)!=0; s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, "r"); }
  out += " }\n";
}

void AOTCEmitter::EmitNegx(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA);
  out += fmt::format("    s->gpr[{}]=(uint32_t)(-(int32_t)s->gpr[{}]);\n", rd, ra);
  if (inst.OE)
    out += fmt::format("    {{ uint32_t ov=(s->gpr[{}]==0x80000000u); s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }}\n", rd);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", rd).c_str());
}

void AOTCEmitter::EmitMullwx(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    s->gpr[{}]=(uint32_t)((int32_t)s->gpr[{}]*(int32_t)s->gpr[{}]);\n", rd, ra, rb);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", rd).c_str());
}

void AOTCEmitter::EmitMulhwx(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    s->gpr[{}]=(uint32_t)(((int64_t)(int32_t)s->gpr[{}]*(int64_t)(int32_t)s->gpr[{}])>>32);\n", rd, ra, rb);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", rd).c_str());
}

void AOTCEmitter::EmitMulhwux(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    s->gpr[{}]=(uint32_t)(((uint64_t)s->gpr[{}]*(uint64_t)s->gpr[{}])>>32);\n", rd, ra, rb);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", rd).c_str());
}

void AOTCEmitter::EmitDivwx(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    {{ int32_t a=(int32_t)s->gpr[{}],b=(int32_t)s->gpr[{}]; "
    "int ov=(b==0)||((uint32_t)a==0x80000000u&&b==-1); "
    "s->gpr[{}]=ov?((a<0)?0xFFFFFFFFu:0):(uint32_t)(a/b);", ra, rb, rd);
  if (inst.OE) out += " s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2;";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, fmt::format("s->gpr[{}]", rd).c_str()); }
  out += " }\n";
}

void AOTCEmitter::EmitDivwux(std::string& out, UGeckoInstruction inst)
{
  u32 rd=I(inst.RD), ra=I(inst.RA), rb=I(inst.RB);
  out += fmt::format("    {{ uint32_t a=s->gpr[{}],b=s->gpr[{}]; s->gpr[{}]=b?a/b:0;", ra, rb, rd);
  if (inst.OE) out += " { uint32_t ov=(b==0); s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }";
  if (inst.Rc) { out += "\n"; EmitUpdateCR0(out, fmt::format("s->gpr[{}]", rd).c_str()); }
  out += " }\n";
}

// ============================================================================
// Logical
// ============================================================================

void AOTCEmitter::EmitLogical(std::string& out, UGeckoInstruction inst, const char* op)
{
  u32 ra=I(inst.RA), rs=I(inst.RS), rb=I(inst.RB);
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]{}s->gpr[{}];\n", ra, rs, op, rb);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

void AOTCEmitter::EmitAndcx(std::string& out, UGeckoInstruction inst)
{
  u32 ra=I(inst.RA), rs=I(inst.RS), rb=I(inst.RB);
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]&~s->gpr[{}];\n", ra, rs, rb);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}
void AOTCEmitter::EmitOrcx(std::string& out, UGeckoInstruction inst)
{
  u32 ra=I(inst.RA), rs=I(inst.RS), rb=I(inst.RB);
  out += fmt::format("    s->gpr[{}]=s->gpr[{}]|~s->gpr[{}];\n", ra, rs, rb);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}
void AOTCEmitter::EmitNandx(std::string& out, UGeckoInstruction inst)
{
  u32 ra=I(inst.RA), rs=I(inst.RS), rb=I(inst.RB);
  out += fmt::format("    s->gpr[{}]=~(s->gpr[{}]&s->gpr[{}]);\n", ra, rs, rb);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}
void AOTCEmitter::EmitNorx(std::string& out, UGeckoInstruction inst)
{
  u32 ra=I(inst.RA), rs=I(inst.RS), rb=I(inst.RB);
  out += fmt::format("    s->gpr[{}]=~(s->gpr[{}]|s->gpr[{}]);\n", ra, rs, rb);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}
void AOTCEmitter::EmitEqvx(std::string& out, UGeckoInstruction inst)
{
  u32 ra=I(inst.RA), rs=I(inst.RS), rb=I(inst.RB);
  out += fmt::format("    s->gpr[{}]=~(s->gpr[{}]^s->gpr[{}]);\n", ra, rs, rb);
  if (inst.Rc) EmitUpdateCR0(out, fmt::format("s->gpr[{}]", ra).c_str());
}

// ============================================================================
// Branch instructions
// ============================================================================

void AOTCEmitter::EmitBx(std::string& out, UGeckoInstruction inst, u32 pc)
{
  u32 target = u32(SignExt26(inst.LI << 2));
  if (!inst.AA) target += pc;
  if (inst.LK) out += fmt::format("    s->spr[8]={:#010x}u;\n", pc + 4);
  EmitBranchTo(out, target, pc);
}

void AOTCEmitter::EmitBcx(std::string& out, UGeckoInstruction inst, u32 pc)
{
  u32 target = u32(SignExt16(s16(inst.BD << 2)));
  if (!inst.AA) target += pc;
  u32 bo = I(inst.BO);

  out += "    {\n";
  if (!(bo & BO_DONT_DECREMENT_FLAG)) out += "    s->spr[9]--;\n";

  bool always = (bo & BO_DONT_DECREMENT_FLAG) && (bo & BO_DONT_CHECK_CONDITION);
  if (!always)
  {
    if (bo & BO_DONT_DECREMENT_FLAG)
      out += "    int ctr_ok=1;\n";
    else
      out += fmt::format("    int ctr_ok=((s->spr[9]!=0)^({}));\n", (bo>>1)&1);

    if (bo & BO_DONT_CHECK_CONDITION)
      out += "    int cond_ok=1;\n";
    else
      out += fmt::format("    int cond_ok=(aot_cr_get_bit(s,{})=={});\n", I(inst.BI), (bo>>3)&1);

    out += "    if(ctr_ok&&cond_ok) {\n";
  }

  if (inst.LK) out += fmt::format("    s->spr[8]={:#010x}u;\n", pc + 4);
  EmitBranchTo(out, target, pc);

  if (!always) out += "    }\n";
  out += "    }\n";
}

void AOTCEmitter::EmitBcctrx(std::string& out, UGeckoInstruction inst, u32 pc)
{
  u32 bo2 = I(inst.BO_2);
  bool always = (bo2 & BO_DONT_CHECK_CONDITION) != 0;
  out += "    {\n";
  if (!always)
  {
    out += fmt::format("    if(aot_cr_get_bit(s,{})=={}) {{\n", I(inst.BI_2), (bo2>>3)&1);
  }
  if (inst.LK_3) out += fmt::format("    s->spr[8]={:#010x}u;\n", pc + 4);
  {
    u32 cycles = m_block_cycle_count;
    out += fmt::format("    s->downcount-={}; s->pc=s->spr[9]&~3u;\n", cycles);
  }
  EmitIndirectDispatch(out);
  if (!always) out += "    }\n";
  out += "    }\n";
}

void AOTCEmitter::EmitBclrx(std::string& out, UGeckoInstruction inst, u32 pc)
{
  u32 bo2 = I(inst.BO_2);
  bool always = (bo2 & BO_DONT_DECREMENT_FLAG) && (bo2 & BO_DONT_CHECK_CONDITION);

  out += "    {\n";
  if (!(bo2 & BO_DONT_DECREMENT_FLAG)) out += "    s->spr[9]--;\n";

  if (!always)
  {
    if (bo2 & BO_DONT_DECREMENT_FLAG) out += "    int ctr_ok=1;\n";
    else out += fmt::format("    int ctr_ok=((s->spr[9]!=0)^({}));\n", (bo2>>1)&1);
    if (bo2 & BO_DONT_CHECK_CONDITION) out += "    int cond_ok=1;\n";
    else out += fmt::format("    int cond_ok=(aot_cr_get_bit(s,{})=={});\n", I(inst.BI_2), (bo2>>3)&1);
    out += "    if(ctr_ok&&cond_ok) {\n";
  }

  out += "    uint32_t dest=s->spr[8]&~3u;\n";
  if (inst.LK_3) out += fmt::format("    s->spr[8]={:#010x}u;\n", pc + 4);
  {
    u32 cycles = m_block_cycle_count;
    out += fmt::format("    s->downcount-={}; s->pc=dest;\n", cycles);
  }
  EmitIndirectDispatch(out);

  if (!always) out += "    }\n";
  out += "    }\n";
}

// ============================================================================
// Helpers
// ============================================================================

void AOTCEmitter::EmitUpdateCR0(std::string& out, const char* expr)
{
  out += fmt::format("    {{ int64_t se=(int64_t)(int32_t)({}); uint64_t cr=(uint64_t)se; "
    "if(({})==0) cr|=1ULL<<63; cr=(cr&~(1ULL<<59))|((uint64_t)(s->xer_so_ov>>1)<<59); "
    "s->cr_fields[0]=cr; }}\n", expr, expr);
}

void AOTCEmitter::EmitSetCarry(std::string& out, const char* expr)
{
  out += fmt::format("    s->xer_ca=({});\n", expr);
}

void AOTCEmitter::EmitOECheck(std::string& out, const char* a, const char* b, const char* result)
{
  out += fmt::format("    {{ uint32_t ov=((({0}^{2})&({1}^{2}))>>31)!=0; "
    "s->xer_so_ov=(s->xer_so_ov&0xfe)|ov; if(ov) s->xer_so_ov|=2; }}\n", a, b, result);
}

void AOTCEmitter::EmitBranchTo(std::string& out, u32 target, u32 current_pc)
{
  if (m_known_blocks.contains(target))
  {
    if (target <= current_pc)
    {
      // Backward edge — must check downcount to prevent infinite loops
      out += fmt::format("    s->downcount-={};\n", m_block_cycle_count);
      out += fmt::format("    if(s->downcount<=0||aot_single_block_mode){{ s->pc={:#010x}u; return; }}\n", target);
    }
    else
    {
      // Forward edge — just decrement (but respect single-block mode for compare harness)
      out += fmt::format("    s->downcount-={};\n", m_block_cycle_count);
      out += fmt::format("    if(aot_single_block_mode){{ s->pc={:#010x}u; return; }}\n", target);
    }
    out += fmt::format("    [[clang::musttail]] return {}_block_{:08x}(s);\n", m_prefix, target);
  }
  else
  {
    // Unknown target — always check
    out += fmt::format("    s->downcount-={};\n", m_block_cycle_count);
    out += fmt::format("    s->pc={:#010x}u; [[clang::musttail]] return {}_dispatch(s);\n", target,
                       m_prefix);
  }
}

void AOTCEmitter::EmitIndirectDispatch(std::string& out)
{
  out += fmt::format("    [[clang::musttail]] return {}_dispatch(s);\n", m_prefix);
}

// ============================================================================
// Load/store integer
// ============================================================================

void AOTCEmitter::EmitLoadInt(std::string& out, UGeckoInstruction inst, const char* helper,
                              bool update, bool indexed)
{
  u32 rd = I(inst.RD), ra = I(inst.RA);
  out += "    {\n";
  if (indexed)
  {
    u32 rb = I(inst.RB);
    if (ra)
      out += fmt::format("        uint32_t ea=s->gpr[{}]+s->gpr[{}];\n", ra, rb);
    else
      out += fmt::format("        uint32_t ea=s->gpr[{}];\n", rb);
  }
  else
  {
    s32 offset = s32(s16(inst.SIMM_16));
    if (ra)
      out += fmt::format("        uint32_t ea=s->gpr[{}]+{};\n", ra, offset);
    else
      out += fmt::format("        uint32_t ea=(uint32_t){};\n", offset);
  }
  out += fmt::format("        uint32_t val={}(s,ea);\n", helper);
  out += fmt::format("        s->gpr[{}]=val;\n", rd);
  if (update && ra)
    out += fmt::format("        s->gpr[{}]=ea;\n", ra);
  out += "    }\n";
}

void AOTCEmitter::EmitStoreInt(std::string& out, UGeckoInstruction inst, const char* helper,
                               bool update, bool indexed)
{
  u32 rs = I(inst.RS), ra = I(inst.RA);
  out += "    {\n";
  if (indexed)
  {
    u32 rb = I(inst.RB);
    if (ra)
      out += fmt::format("        uint32_t ea=s->gpr[{}]+s->gpr[{}];\n", ra, rb);
    else
      out += fmt::format("        uint32_t ea=s->gpr[{}];\n", rb);
  }
  else
  {
    s32 offset = s32(s16(inst.SIMM_16));
    if (ra)
      out += fmt::format("        uint32_t ea=s->gpr[{}]+{};\n", ra, offset);
    else
      out += fmt::format("        uint32_t ea=(uint32_t){};\n", offset);
  }
  out += fmt::format("        {}(s,s->gpr[{}],ea);\n", helper, rs);
  if (update && ra)
    out += fmt::format("        s->gpr[{}]=ea;\n", ra);
  out += "    }\n";
}

void AOTCEmitter::EmitLmw(std::string& out, UGeckoInstruction inst)
{
  u32 rd = I(inst.RD), ra = I(inst.RA);
  s32 offset = s32(s16(inst.SIMM_16));
  out += "    {\n";
  if (ra)
    out += fmt::format("        uint32_t ea=s->gpr[{}]+{};\n", ra, offset);
  else
    out += fmt::format("        uint32_t ea=(uint32_t){};\n", offset);
  out += fmt::format("        for(int r={};r<32;r++,ea+=4) s->gpr[r]=aot_read_u32(s,ea);\n", rd);
  out += "    }\n";
}

void AOTCEmitter::EmitStmw(std::string& out, UGeckoInstruction inst)
{
  u32 rs = I(inst.RS), ra = I(inst.RA);
  s32 offset = s32(s16(inst.SIMM_16));
  out += "    {\n";
  if (ra)
    out += fmt::format("        uint32_t ea=s->gpr[{}]+{};\n", ra, offset);
  else
    out += fmt::format("        uint32_t ea=(uint32_t){};\n", offset);
  out += fmt::format("        for(int r={};r<32;r++,ea+=4) aot_write_u32(s,s->gpr[r],ea);\n", rs);
  out += "    }\n";
}

// ============================================================================
// Load/store FP
// ============================================================================

void AOTCEmitter::EmitLfs(std::string& out, UGeckoInstruction inst, bool update, bool indexed)
{
  u32 fd = I(inst.RD), ra = I(inst.RA);
  out += "    {\n";
  if (indexed)
  {
    u32 rb = I(inst.RB);
    out += fmt::format("        uint32_t ea={}+s->gpr[{}];\n",
                       ra ? fmt::format("s->gpr[{}]", ra) : "0", rb);
  }
  else
  {
    s32 offset = s32(s16(inst.SIMM_16));
    out += fmt::format("        uint32_t ea={}+{};\n",
                       ra ? fmt::format("s->gpr[{}]", ra) : "0", offset);
  }
  out += "        uint32_t raw=aot_read_u32(s,ea);\n";
  out += fmt::format("        uint64_t dv=aot_convert_to_double(raw); "
                     "s->ps[{}].ps0=dv; s->ps[{}].ps1=dv;\n", fd, fd);
  if (update && ra)
    out += fmt::format("        s->gpr[{}]=ea;\n", ra);
  out += "    }\n";
}

void AOTCEmitter::EmitLfd(std::string& out, UGeckoInstruction inst, bool update, bool indexed)
{
  u32 fd = I(inst.RD), ra = I(inst.RA);
  out += "    {\n";
  if (indexed)
  {
    u32 rb = I(inst.RB);
    out += fmt::format("        uint32_t ea={}+s->gpr[{}];\n",
                       ra ? fmt::format("s->gpr[{}]", ra) : "0", rb);
  }
  else
  {
    s32 offset = s32(s16(inst.SIMM_16));
    out += fmt::format("        uint32_t ea={}+{};\n",
                       ra ? fmt::format("s->gpr[{}]", ra) : "0", offset);
  }
  out += fmt::format("        s->ps[{}].ps0=aot_read_u64(s,ea);\n", fd);
  if (update && ra)
    out += fmt::format("        s->gpr[{}]=ea;\n", ra);
  out += "    }\n";
}

void AOTCEmitter::EmitStfs(std::string& out, UGeckoInstruction inst, bool update, bool indexed)
{
  u32 fs = I(inst.RS), ra = I(inst.RA);
  out += "    {\n";
  if (indexed)
  {
    u32 rb = I(inst.RB);
    out += fmt::format("        uint32_t ea={}+s->gpr[{}];\n",
                       ra ? fmt::format("s->gpr[{}]", ra) : "0", rb);
  }
  else
  {
    s32 offset = s32(s16(inst.SIMM_16));
    out += fmt::format("        uint32_t ea={}+{};\n",
                       ra ? fmt::format("s->gpr[{}]", ra) : "0", offset);
  }
  out += fmt::format("        aot_write_u32(s,aot_convert_to_single(s->ps[{}].ps0),ea);\n", fs);
  if (update && ra)
    out += fmt::format("        s->gpr[{}]=ea;\n", ra);
  out += "    }\n";
}

void AOTCEmitter::EmitStfd(std::string& out, UGeckoInstruction inst, bool update, bool indexed)
{
  u32 fs = I(inst.RS), ra = I(inst.RA);
  out += "    {\n";
  if (indexed)
  {
    u32 rb = I(inst.RB);
    out += fmt::format("        uint32_t ea={}+s->gpr[{}];\n",
                       ra ? fmt::format("s->gpr[{}]", ra) : "0", rb);
  }
  else
  {
    s32 offset = s32(s16(inst.SIMM_16));
    out += fmt::format("        uint32_t ea={}+{};\n",
                       ra ? fmt::format("s->gpr[{}]", ra) : "0", offset);
  }
  out += fmt::format("        aot_write_u64(s,s->ps[{}].ps0,ea);\n", fs);
  if (update && ra)
    out += fmt::format("        s->gpr[{}]=ea;\n", ra);
  out += "    }\n";
}

// ============================================================================
// SPR/system
// ============================================================================

void AOTCEmitter::EmitMfspr(std::string& out, UGeckoInstruction inst)
{
  u32 rd = I(inst.RD);
  u32 spr = (I(inst.SPR) & 0x1F) << 5 | (I(inst.SPR) >> 5);
  // Common SPRs inline, special ones via runtime
  switch (spr)
  {
  case 8:   // LR
  case 9:   // CTR
    out += fmt::format("    s->gpr[{}]=s->spr[{}];\n", rd, spr);
    break;
  case 1:   // XER
    out += fmt::format("    s->gpr[{}]=((uint32_t)(s->xer_so_ov>>1)<<31)|"
                       "((uint32_t)(s->xer_so_ov&1)<<30)|((uint32_t)s->xer_ca<<29)|"
                       "((uint32_t)s->xer_stringctrl);\n", rd);
    break;
  default:
    out += fmt::format("    s->gpr[{}]=aot_mfspr_special(s,{});\n", rd, spr);
    break;
  }
}

void AOTCEmitter::EmitMtspr(std::string& out, UGeckoInstruction inst)
{
  u32 rs = I(inst.RS);
  u32 spr = (I(inst.SPR) & 0x1F) << 5 | (I(inst.SPR) >> 5);
  switch (spr)
  {
  case 8:   // LR
  case 9:   // CTR
    out += fmt::format("    s->spr[{}]=s->gpr[{}];\n", spr, rs);
    break;
  case 1:   // XER
    out += fmt::format("    {{ uint32_t v=s->gpr[{}]; s->xer_so_ov=((v>>31)<<1)|((v>>30)&1); "
                       "s->xer_ca=(v>>29)&1; s->xer_stringctrl=v&0xFFFF; }}\n", rs);
    break;
  default:
    out += fmt::format("    aot_mtspr_special(s,{},s->gpr[{}]);\n", spr, rs);
    break;
  }
}

void AOTCEmitter::EmitMfcr(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->gpr[{}]=aot_mfcr(s);\n", I(inst.RD));
}

void AOTCEmitter::EmitMtcrf(std::string& out, UGeckoInstruction inst)
{
  u32 rs = I(inst.RS);
  u32 crm = I(inst.CRM);
  out += fmt::format("    aot_mtcrf(s,{:#x},{});\n", crm, rs);
}

void AOTCEmitter::EmitMcrxr(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    {{ uint32_t xer_top=((s->xer_so_ov>>1)<<3)|((s->xer_so_ov&1)<<2)"
                     "|((uint32_t)s->xer_ca<<1); aot_cr_set_field(s,{},xer_top); "
                     "s->xer_ca=0; s->xer_so_ov=0; }}\n", I(inst.CRFD));
}

void AOTCEmitter::EmitMfmsr(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->gpr[{}]=s->msr;\n", I(inst.RD));
}

void AOTCEmitter::EmitMtmsr(std::string& out, UGeckoInstruction inst, u32 pc)
{
  // mtmsr ends the block (may enable exceptions). Set PC to fallthrough
  // before calling aot_mtmsr which may change PC via CheckExceptions.
  out += fmt::format("    s->downcount-={}; s->pc={:#010x}u; s->npc=s->pc; "
                     "aot_mtmsr(s,s->gpr[{}]); return;\n",
                     m_block_cycle_count, pc + 4, I(inst.RS));
}

void AOTCEmitter::EmitMfsr(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->gpr[{}]=s->sr[{}];\n", I(inst.RD), I(inst.SR));
}

void AOTCEmitter::EmitMtsr(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->sr[{}]=s->gpr[{}]; aot_sr_updated(s);\n", I(inst.SR), I(inst.RS));
}

void AOTCEmitter::EmitTwi(std::string& out, UGeckoInstruction inst, u32 pc)
{
  out += fmt::format("    s->pc={:#010x}u; if(aot_twi(s,{},(int32_t)s->gpr[{}],{})) "
                     "{{ s->downcount-={}; return; }}\n",
                     pc + 4, I(inst.TO), I(inst.RA), I(inst.SIMM_16), m_block_cycle_count);
}

void AOTCEmitter::EmitCrLogical(std::string& out, UGeckoInstruction inst, const char* op)
{
  // CRM field encoding differs for CR logical ops
  u32 crbD = I(inst.RD);  // actually crbD
  u32 crbA = I(inst.RA);  // actually crbA
  u32 crbB = I(inst.RB);  // actually crbB

  // Use runtime helper for simplicity
  out += fmt::format("    aot_cr_logical(s,{},{},{},\"{}\");\n", crbD, crbA, crbB, op);
}

void AOTCEmitter::EmitMcrf(std::string& out, UGeckoInstruction inst)
{
  out += fmt::format("    s->cr_fields[{}]=s->cr_fields[{}];\n", I(inst.CRFD), I(inst.CRFS));
}

// ============================================================================
// FP instructions — all go through runtime helpers for correctness
// ============================================================================

bool AOTCEmitter::EmitTable59(std::string& out, UGeckoInstruction inst, u32 pc)
{
  // Table 59: single-precision FP (SUBOP5)
  u32 fd=I(inst.FD), fa=I(inst.FA), fb=I(inst.FB), fc=I(inst.FC);
  switch (I(inst.SUBOP5))
  {
  case 18: out += fmt::format("    aot_fdivsx(s,{},{},{});\n", fd, fa, fb); return true;
  case 20: out += fmt::format("    aot_fast_fsubsx(s,{},{},{});\n", fd, fa, fb); return true;
  case 21: out += fmt::format("    aot_fast_faddsx(s,{},{},{});\n", fd, fa, fb); return true;
  case 24: out += fmt::format("    aot_fresx(s,{},{});\n", fd, fb); return true;
  case 25: out += fmt::format("    aot_fast_fmulsx(s,{},{},{});\n", fd, fa, fc); return true;
  case 28: out += fmt::format("    aot_fmsubsx(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 29: out += fmt::format("    aot_fmaddsx(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 30: out += fmt::format("    aot_fnmsubsx(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 31: out += fmt::format("    aot_fnmaddsx(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  default: return false;
  }
}

bool AOTCEmitter::EmitTable63(std::string& out, UGeckoInstruction inst, u32 pc)
{
  u32 fd=I(inst.FD), fa=I(inst.FA), fb=I(inst.FB), fc=I(inst.FC);

  // Table 63 uses both SUBOP10 and SUBOP5
  switch (I(inst.SUBOP10))
  {
  case 0:   out += fmt::format("    aot_fcmpu(s,{},{},{});\n", I(inst.CRFD), fa, fb); return true;
  case 32:  out += fmt::format("    aot_fcmpo(s,{},{},{});\n", I(inst.CRFD), fa, fb); return true;
  case 12:  out += fmt::format("    aot_frspx(s,{},{});\n", fd, fb); return true;
  case 14:  out += fmt::format("    aot_fctiwx(s,{},{});\n", fd, fb); return true;
  case 15:  out += fmt::format("    aot_fctiwzx(s,{},{});\n", fd, fb); return true;
  case 40:  // fnegx
    out += fmt::format("    s->ps[{}].ps0=s->ps[{}].ps0^(1ULL<<63);\n", fd, fb);
    if (inst.Rc) out += "    s->cr_fields[1]=s->cr_fields[1]; /* CR1 not impl */\n";
    return true;
  case 72:  // fmrx
    out += fmt::format("    s->ps[{}].ps0=s->ps[{}].ps0;\n", fd, fb);
    if (inst.Rc) out += "    s->cr_fields[1]=s->cr_fields[1];\n";
    return true;
  case 264: // fabsx
    out += fmt::format("    s->ps[{}].ps0=s->ps[{}].ps0&~(1ULL<<63);\n", fd, fb);
    if (inst.Rc) out += "    s->cr_fields[1]=s->cr_fields[1];\n";
    return true;
  case 136: // fnabsx
    out += fmt::format("    s->ps[{}].ps0=s->ps[{}].ps0|(1ULL<<63);\n", fd, fb);
    if (inst.Rc) out += "    s->cr_fields[1]=s->cr_fields[1];\n";
    return true;
  case 583: // mffsx
    out += fmt::format("    s->ps[{}].ps0=(uint64_t)s->fpscr;\n", fd);
    return true;
  case 711: // mtfsfx
    out += fmt::format("    aot_mtfsf(s,{},{});\n", I(inst.FM), fb);
    return true;
  case 134: // mtfsfix
    out += fmt::format("    aot_mtfsfi(s,{},{});\n", I(inst.CRFD), I(inst.RB));
    return true;
  case 70:  // mtfsb0x
    out += fmt::format("    s->fpscr&=~(1u<<(31-{}));\n", I(inst.RD));
    return true;
  case 38:  // mtfsb1x
    out += fmt::format("    s->fpscr|=(1u<<(31-{}));\n", I(inst.RD));
    return true;
  case 64:  // mcrfs
    out += fmt::format("    aot_mcrfs(s,{},{});\n", I(inst.CRFD), I(inst.CRFS));
    return true;
  case 26:  // frsqrtex (also SUBOP5=26, caught here first)
    out += fmt::format("    aot_frsqrtex(s,{},{});\n", fd, fb);
    return true;
  default:
    break;
  }

  // SUBOP5 (for double-precision arithmetic)
  switch (I(inst.SUBOP5))
  {
  case 18: out += fmt::format("    aot_fdivx(s,{},{},{});\n", fd, fa, fb); return true;
  case 23: out += fmt::format("    aot_fselx(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 20: out += fmt::format("    aot_fsubx(s,{},{},{});\n", fd, fa, fb); return true;
  case 21: out += fmt::format("    aot_faddx(s,{},{},{});\n", fd, fa, fb); return true;
  case 25: out += fmt::format("    aot_fmulx(s,{},{},{});\n", fd, fa, fc); return true;
  case 28: out += fmt::format("    aot_fmsubx(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 29: out += fmt::format("    aot_fmaddx(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 30: out += fmt::format("    aot_fnmsubx(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 31: out += fmt::format("    aot_fnmaddx(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  default: return false;
  }
}

// ============================================================================
// Paired singles (table 4) — all via runtime helpers
// ============================================================================

bool AOTCEmitter::EmitTable4(std::string& out, UGeckoInstruction inst, u32 pc)
{
  u32 fd=I(inst.FD), fa=I(inst.FA), fb=I(inst.FB), fc=I(inst.FC);

  switch (I(inst.SUBOP10))
  {
  case 6:   out += fmt::format("    aot_psq_lx(s,{});\n", inst.hex); return true;
  case 7:   out += fmt::format("    aot_psq_stx(s,{});\n", inst.hex); return true;
  case 38:  out += fmt::format("    aot_psq_lux(s,{});\n", inst.hex); return true;
  case 39:  out += fmt::format("    aot_psq_stux(s,{});\n", inst.hex); return true;
  case 1014:
    if (I(inst.RA))
      out += fmt::format("    aot_dcbz_l(s,s->gpr[{}]+s->gpr[{}]);\n", I(inst.RA), I(inst.RB));
    else
      out += fmt::format("    aot_dcbz_l(s,s->gpr[{}]);\n", I(inst.RB));
    return true;
  default: break;
  }

  // PS arithmetic uses SUBOP5
  switch (I(inst.SUBOP5))
  {
  case 10: out += fmt::format("    aot_ps_sum0(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 11: out += fmt::format("    aot_ps_sum1(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 12: out += fmt::format("    aot_ps_muls0(s,{},{},{});\n", fd, fa, fc); return true;
  case 13: out += fmt::format("    aot_ps_muls1(s,{},{},{});\n", fd, fa, fc); return true;
  case 14: out += fmt::format("    aot_ps_madds0(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 15: out += fmt::format("    aot_ps_madds1(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 18: out += fmt::format("    aot_ps_div(s,{},{},{});\n", fd, fa, fb); return true;
  case 20: out += fmt::format("    aot_fast_ps_sub(s,{},{},{});\n", fd, fa, fb); return true;
  case 21: out += fmt::format("    aot_fast_ps_add(s,{},{},{});\n", fd, fa, fb); return true;
  case 23: out += fmt::format("    aot_ps_sel(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 24: out += fmt::format("    aot_ps_res(s,{},{});\n", fd, fb); return true;
  case 25: out += fmt::format("    aot_fast_ps_mul(s,{},{},{});\n", fd, fa, fc); return true;
  case 26: out += fmt::format("    aot_ps_rsqrte(s,{},{});\n", fd, fb); return true;
  case 28: out += fmt::format("    aot_fast_ps_msub(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 29: out += fmt::format("    aot_fast_ps_madd(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 30: out += fmt::format("    aot_ps_nmsub(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  case 31: out += fmt::format("    aot_ps_nmadd(s,{},{},{},{});\n", fd, fa, fc, fb); return true;
  default: break;
  }

  // PS misc (SUBOP10 again for moves/compares/merges)
  switch (I(inst.SUBOP10))
  {
  case 40:  out += fmt::format("    aot_ps_neg(s,{},{});\n", fd, fb); return true;
  case 72:  out += fmt::format("    aot_ps_mr(s,{},{});\n", fd, fb); return true;
  case 136: out += fmt::format("    aot_ps_nabs(s,{},{});\n", fd, fb); return true;
  case 264: out += fmt::format("    aot_ps_abs(s,{},{});\n", fd, fb); return true;
  case 528: out += fmt::format("    aot_ps_merge00(s,{},{},{});\n", fd, fa, fb); return true;
  case 560: out += fmt::format("    aot_ps_merge01(s,{},{},{});\n", fd, fa, fb); return true;
  case 592: out += fmt::format("    aot_ps_merge10(s,{},{},{});\n", fd, fa, fb); return true;
  case 624: out += fmt::format("    aot_ps_merge11(s,{},{},{});\n", fd, fa, fb); return true;
  case 0:   out += fmt::format("    aot_ps_cmpu0(s,{},{},{});\n", I(inst.CRFD), fa, fb); return true;
  case 32:  out += fmt::format("    aot_ps_cmpo0(s,{},{},{});\n", I(inst.CRFD), fa, fb); return true;
  case 64:  out += fmt::format("    aot_ps_cmpu1(s,{},{},{});\n", I(inst.CRFD), fa, fb); return true;
  case 96:  out += fmt::format("    aot_ps_cmpo1(s,{},{},{});\n", I(inst.CRFD), fa, fb); return true;
  default: return false;
  }
}

#undef I

}  // namespace DolphinTool
