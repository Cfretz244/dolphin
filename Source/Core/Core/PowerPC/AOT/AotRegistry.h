// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct AOTState;

using AOTBlockFunc = void (*)(AOTState*);
using AOTDispatchFunc = void (*)(AOTState*);
using AOTLookupFunc = AOTBlockFunc (*)(uint32_t);

// Mirrors the AotModuleSectionDesc/AotModuleDesc structs emitted into each AOT
// library's dispatch.c — per-REL-module dispatch tables and runtime section
// base slots, activated by the module tracker when the game loads a module.
struct AotModuleSectionDesc
{
  uint32_t size;
  uint32_t executable;
  const AOTBlockFunc* table;  // nullptr for non-executable sections
  uint32_t* base_slot;        // runtime section base address, 0 = unloaded
};

struct AotModuleDesc
{
  uint32_t module_id;
  uint32_t num_sections;
  const AotModuleSectionDesc* sections;
};

struct AotGameEntry
{
  std::string game_id;
  AOTDispatchFunc dispatch;
  AOTLookupFunc lookup_block;
  const AotModuleDesc* modules = nullptr;
  uint32_t module_count = 0;
};

// Called by __attribute__((constructor)) in each AOT static library before main().
extern "C" void aot_register_game(const char* game_id, AOTDispatchFunc dispatch,
                                  AOTLookupFunc lookup);
extern "C" void aot_register_game_modules(const char* game_id, const void* modules,
                                          uint32_t count);

class AotRegistry
{
public:
  static AotRegistry& Instance();

  void Register(const std::string& game_id, AOTDispatchFunc dispatch, AOTLookupFunc lookup);
  void RegisterModules(const std::string& game_id, const AotModuleDesc* modules, uint32_t count);
  std::optional<AotGameEntry> Find(const std::string& game_id) const;
  std::vector<std::string> GetRegisteredGameIDs() const;

private:
  std::unordered_map<std::string, AotGameEntry> m_games;
};
