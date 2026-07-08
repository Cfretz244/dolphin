// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

// aot_runtime.h is the single source of truth for the C ABI between the AOT
// runtime and generated game code: the translate step emits a verbatim copy of
// it into every aot-src tree, and dolphin-tool embeds its bytes at build time
// (StringifyHeader.cmake).
#include "Core/PowerPC/AOT/aot_runtime.h"

#include "Core/PowerPC/PowerPC.h"

// AOTState mirrors the layout of PowerPC::PowerPCState so generated C code can
// operate on the live CPU state directly. These casts are the only sanctioned
// way to cross between the two views; the layout contract is enforced by the
// static_asserts in AotRuntime.cpp.
inline PowerPC::PowerPCState& FromAot(AOTState* s)
{
  return *reinterpret_cast<PowerPC::PowerPCState*>(s);
}

inline const PowerPC::PowerPCState& FromAot(const AOTState* s)
{
  return *reinterpret_cast<const PowerPC::PowerPCState*>(s);
}

inline AOTState* ToAot(PowerPC::PowerPCState& ppc_state)
{
  return reinterpret_cast<AOTState*>(&ppc_state);
}
