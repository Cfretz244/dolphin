// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/CPUCoreBase.h"

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;
}

struct AOTState;
using AOTBlockFunc = void (*)(AOTState*);

#ifdef DOLPHIN_AOT_HARNESS
class AotHarness;
#endif

// AOT CPU core backend: executes pre-compiled C functions for translated PPC blocks.
// Falls back to the interpreter for untranslated addresses. The compare/diff
// instrumentation lives in AotHarness (DOLPHIN_AOT_HARNESS builds only).
class AOTCore : public CPUCoreBase
{
public:
  explicit AOTCore(Core::System& system);
  ~AOTCore() override;

  void Init() override;
  void Shutdown() override;
  void Run() override;
  void SingleStep() override;
  // Savestate loads clear caches and can wholesale replace the loaded-module
  // landscape — force a module queue rescan.
  void ClearCache() override;
  const char* GetName() const override { return "AOT"; }

private:
  // Compares the mounted disc's boot-DOL sha256 against the hash the selected
  // AOT library was generated from (aot_register_game_image). Runs lazily at
  // the first Run() slice — the disc is guaranteed mounted there, unlike at
  // Init(). Returns false on a PROVEN mismatch (hard stop); missing hash or
  // an unreadable disc only warns.
  bool VerifyImageIdentity();

  Core::System& m_system;
  PowerPC::PowerPCState& m_ppc_state;

  using DispatchFunc = void (*)(AOTState*);
  DispatchFunc m_dispatch = nullptr;
  DispatchFunc m_interp_dispatch = nullptr;

  std::string m_expected_dol_sha256;  // from the registry entry; empty = none
  bool m_image_checked = false;
  bool m_image_blocked = false;

#ifdef DOLPHIN_AOT_HARNESS
  // Non-null when an AOT_* debug switch or dolphin-tool diff engaged the
  // instrumented loop for this session.
  std::unique_ptr<AotHarness> m_harness;
#endif
};
