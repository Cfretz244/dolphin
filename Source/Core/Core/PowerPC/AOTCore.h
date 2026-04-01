// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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

// AOT CPU core backend: executes pre-compiled C functions for translated PPC blocks.
// Falls back to the interpreter for untranslated addresses.
class AOTCore : public CPUCoreBase
{
public:
  explicit AOTCore(Core::System& system);
  ~AOTCore() override;

  void Init() override;
  void Shutdown() override;
  void Run() override;
  void SingleStep() override;
  void ClearCache() override {}
  const char* GetName() const override { return "AOT"; }

private:
  Core::System& m_system;
  PowerPC::PowerPCState& m_ppc_state;

  using DispatchFunc = void (*)(AOTState*);
  DispatchFunc m_dispatch = nullptr;
};
