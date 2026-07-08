// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Shared C ABI types (AOTState, AOTBlockFunc, AotModuleDesc, AotBlockSize,
// registration entry points) — the same header the generated code compiles
// against.
#include "Core/PowerPC/AOT/aot_runtime.h"

using AOTDispatchFunc = void (*)(AOTState*);
using AOTLookupFunc = AOTBlockFunc (*)(uint32_t);

struct AotGameEntry
{
  std::string game_id;
  AOTDispatchFunc dispatch;
  AOTLookupFunc lookup_block;
  const AotModuleDesc* modules = nullptr;
  uint32_t module_count = 0;
  // Block boundary metadata for the compare/diff harness; only present in
  // AOT_HARNESS builds of the game library (empty otherwise).
  const AotBlockSize* block_sizes = nullptr;
  uint32_t block_size_count = 0;
  const AotModuleBlockSize* module_block_sizes = nullptr;
  uint32_t module_block_size_count = 0;
};

// Multi-game registry, populated before main() by each linked AOT library's
// __attribute__((constructor)) via the aot_register_* entry points declared
// in aot_runtime.h. AOTCore::Init selects the entry matching the loaded game.
class AotRegistry
{
public:
  static AotRegistry& Instance();

  void Register(const std::string& game_id, AOTDispatchFunc dispatch, AOTLookupFunc lookup,
                uint32_t abi_version);
  void RegisterModules(const std::string& game_id, const AotModuleDesc* modules, uint32_t count);
  void RegisterBlockSizes(const std::string& game_id, const AotBlockSize* blocks, uint32_t count,
                          const AotModuleBlockSize* module_blocks, uint32_t module_count);
  std::optional<AotGameEntry> Find(const std::string& game_id) const;
  std::vector<std::string> GetRegisteredGameIDs() const;

private:
  // Games whose library was built against a different AOT_ABI_VERSION are
  // rejected at registration (the game falls back to the interpreter) — a
  // stale .a must never run against a runtime with a different ABI.
  bool IsRejected(const std::string& game_id) const;

  std::unordered_map<std::string, AotGameEntry> m_games;
  std::vector<std::string> m_rejected;
};
