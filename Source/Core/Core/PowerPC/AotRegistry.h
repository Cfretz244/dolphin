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
using AOTInvalidateFunc = void (*)(uint32_t);

struct AotGameEntry
{
  std::string game_id;
  AOTDispatchFunc dispatch;
  AOTLookupFunc lookup_block;
  AOTInvalidateFunc invalidate_block;
};

// Called by __attribute__((constructor)) in each AOT static library before main().
// Legacy v1: no invalidation support (sets invalidate_block = nullptr)
extern "C" void aot_register_game(const char* game_id, AOTDispatchFunc dispatch,
                                  AOTLookupFunc lookup);
// v2: with invalidation callback
extern "C" void aot_register_game_v2(const char* game_id, AOTDispatchFunc dispatch,
                                     AOTLookupFunc lookup, AOTInvalidateFunc invalidate);

class AotRegistry
{
public:
  static AotRegistry& Instance();

  void Register(const std::string& game_id, AOTDispatchFunc dispatch, AOTLookupFunc lookup,
                AOTInvalidateFunc invalidate = nullptr);
  std::optional<AotGameEntry> Find(const std::string& game_id) const;
  std::vector<std::string> GetRegisteredGameIDs() const;

private:
  std::unordered_map<std::string, AotGameEntry> m_games;
};
