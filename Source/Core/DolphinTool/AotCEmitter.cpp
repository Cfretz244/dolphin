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

std::string AOTCEmitter::TranslateBlock(u32 block_addr, u32 num_instructions)
{
  std::string out;
  m_block_cycle_count = 0;
  out += fmt::format("void {}_block_{:08x}(AOTState* s) {{\n", m_prefix, block_addr);

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
  default:  return false;
  }
}

bool AOTCEmitter::EmitTable19(std::string& out, UGeckoInstruction inst, u32 pc)
{
  switch (I(inst.SUBOP10))
  {
  case 528: EmitBcctrx(out, inst, pc); return true;
  case 16:  EmitBclrx(out, inst, pc); return true;
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
  out += fmt::format("    s->downcount-={};\n", m_block_cycle_count);
  out += "    if(s->downcount<=0){";
  out += fmt::format(" s->pc={:#010x}u; return; }}\n", target);
  EmitBranchTo(out, target);
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
  out += fmt::format("    s->downcount-={}; if(s->downcount<=0){{ s->pc={:#010x}u; return; }}\n",
                     m_block_cycle_count, target);
  EmitBranchTo(out, target);

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
  out += fmt::format("    s->downcount-={}; s->pc=s->spr[9]&~3u;\n", m_block_cycle_count);
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
  out += fmt::format("    s->downcount-={}; s->pc=dest;\n", m_block_cycle_count);
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

void AOTCEmitter::EmitBranchTo(std::string& out, u32 target)
{
  if (m_known_blocks.contains(target))
    out += fmt::format("    {}_block_{:08x}(s); return;\n", m_prefix, target);
  else
    out += fmt::format("    s->pc={:#010x}u; {}_dispatch(s); return;\n", target, m_prefix);
}

void AOTCEmitter::EmitIndirectDispatch(std::string& out)
{
  out += fmt::format("    {}_dispatch(s); return;\n", m_prefix);
}

#undef I

}  // namespace DolphinTool
