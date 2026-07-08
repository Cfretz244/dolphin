// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/PowerPC.h"

// Paired-single operation cores, shared between the interpreter's ps_*
// handlers (Interpreter_Paired.cpp) and the AOT runtime's aot_ps_* helpers
// (AotRuntime.cpp). Bodies are verbatim from the interpreter; callers handle
// instruction-field extraction and the Rc=1 (UpdateCR1) branch themselves.
// ps_div/ps_res/ps_rsqrte are not here — their exception logic stays in the
// interpreter and the AOT runtime delegates to it.

inline void PS_Sel(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  ppc_state.ps[fd].SetBoth(a.PS0AsDouble() >= -0.0 ? c.PS0AsDouble() : b.PS0AsDouble(),
                           a.PS1AsDouble() >= -0.0 ? c.PS1AsDouble() : b.PS1AsDouble());
}

inline void PS_Neg(PowerPC::PowerPCState& ppc_state, int fd, int fb)
{
  const auto& b = ppc_state.ps[fb];

  ppc_state.ps[fd].SetBoth(b.PS0AsU64() ^ (UINT64_C(1) << 63), b.PS1AsU64() ^ (UINT64_C(1) << 63));
}

inline void PS_Mr(PowerPC::PowerPCState& ppc_state, int fd, int fb)
{
  ppc_state.ps[fd] = ppc_state.ps[fb];
}

inline void PS_Nabs(PowerPC::PowerPCState& ppc_state, int fd, int fb)
{
  const auto& b = ppc_state.ps[fb];

  ppc_state.ps[fd].SetBoth(b.PS0AsU64() | (UINT64_C(1) << 63), b.PS1AsU64() | (UINT64_C(1) << 63));
}

inline void PS_Abs(PowerPC::PowerPCState& ppc_state, int fd, int fb)
{
  const auto& b = ppc_state.ps[fb];

  ppc_state.ps[fd].SetBoth(b.PS0AsU64() & ~(UINT64_C(1) << 63),
                           b.PS1AsU64() & ~(UINT64_C(1) << 63));
}

inline void PS_Merge00(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];

  ppc_state.ps[fd].SetBoth(a.PS0AsDouble(), b.PS0AsDouble());
}

inline void PS_Merge01(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];

  ppc_state.ps[fd].SetBoth(a.PS0AsDouble(), b.PS1AsDouble());
}

inline void PS_Merge10(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];

  ppc_state.ps[fd].SetBoth(a.PS1AsDouble(), b.PS0AsDouble());
}

inline void PS_Merge11(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];

  ppc_state.ps[fd].SetBoth(a.PS1AsDouble(), b.PS1AsDouble());
}

inline void PS_Sub(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];

  const float ps0 =
      ForceSingle(ppc_state.fpscr, NI_sub(ppc_state, a.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 =
      ForceSingle(ppc_state.fpscr, NI_sub(ppc_state, a.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Add(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];

  const float ps0 =
      ForceSingle(ppc_state.fpscr, NI_add(ppc_state, a.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 =
      ForceSingle(ppc_state.fpscr, NI_add(ppc_state, a.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Mul(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc)
{
  const auto& a = ppc_state.ps[fa];
  const auto& c = ppc_state.ps[fc];

  const double c0 = Force25Bit(c.PS0AsDouble());
  const double c1 = Force25Bit(c.PS1AsDouble());

  const float ps0 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS0AsDouble(), c0).value);
  const float ps1 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS1AsDouble(), c1).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Msub(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(
      ppc_state.fpscr,
      NI_msub<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 = ForceSingle(
      ppc_state.fpscr,
      NI_msub<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Madd(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Nmsub(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float tmp0 = ForceSingle(
      ppc_state.fpscr,
      NI_msub<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float tmp1 = ForceSingle(
      ppc_state.fpscr,
      NI_msub<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  const float ps0 = std::isnan(tmp0) ? tmp0 : -tmp0;
  const float ps1 = std::isnan(tmp1) ? tmp1 : -tmp1;

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Nmadd(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float tmp0 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float tmp1 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  const float ps0 = std::isnan(tmp0) ? tmp0 : -tmp0;
  const float ps1 = std::isnan(tmp1) ? tmp1 : -tmp1;

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Sum0(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 =
      ForceSingle(ppc_state.fpscr, NI_add(ppc_state, a.PS0AsDouble(), b.PS1AsDouble()).value);
  const float ps1 = ForceSingle(ppc_state.fpscr, c.PS1AsDouble());

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Sum1(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(ppc_state.fpscr, c.PS0AsDouble());
  const float ps1 =
      ForceSingle(ppc_state.fpscr, NI_add(ppc_state, a.PS0AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps1);
}

inline void PS_Muls0(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc)
{
  const auto& a = ppc_state.ps[fa];
  const auto& c = ppc_state.ps[fc];

  const double c0 = Force25Bit(c.PS0AsDouble());
  const float ps0 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS0AsDouble(), c0).value);
  const float ps1 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS1AsDouble(), c0).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Muls1(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc)
{
  const auto& a = ppc_state.ps[fa];
  const auto& c = ppc_state.ps[fc];

  const double c1 = Force25Bit(c.PS1AsDouble());
  const float ps0 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS0AsDouble(), c1).value);
  const float ps1 = ForceSingle(ppc_state.fpscr, NI_mul(ppc_state, a.PS1AsDouble(), c1).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Madds0(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS1AsDouble(), c.PS0AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}

inline void PS_Madds1(PowerPC::PowerPCState& ppc_state, int fd, int fa, int fc, int fb)
{
  const auto& a = ppc_state.ps[fa];
  const auto& b = ppc_state.ps[fb];
  const auto& c = ppc_state.ps[fc];

  const float ps0 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS1AsDouble(), b.PS0AsDouble()).value);
  const float ps1 = ForceSingle(
      ppc_state.fpscr,
      NI_madd<true>(ppc_state, a.PS1AsDouble(), c.PS1AsDouble(), b.PS1AsDouble()).value);

  ppc_state.ps[fd].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(ps0);
}
