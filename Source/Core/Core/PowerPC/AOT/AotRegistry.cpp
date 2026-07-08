// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT/AotRegistry.h"

#include "Common/Logging/Log.h"

AotRegistry& AotRegistry::Instance()
{
  static AotRegistry instance;
  return instance;
}

void AotRegistry::Register(const std::string& game_id, AOTDispatchFunc dispatch,
                           AOTLookupFunc lookup)
{
  if (m_games.contains(game_id))
  {
    // This would only happen if two libraries provide the same game ID.
    // Last registration wins; this is a build misconfiguration.
    WARN_LOG_FMT(AOT, "AotRegistry: Duplicate registration for game {}, overwriting", game_id);
  }
  auto& entry = m_games[game_id];
  entry.game_id = game_id;
  entry.dispatch = dispatch;
  entry.lookup_block = lookup;
}

void AotRegistry::RegisterModules(const std::string& game_id, const AotModuleDesc* modules,
                                  uint32_t count)
{
  auto& entry = m_games[game_id];
  if (entry.game_id.empty())
    entry.game_id = game_id;
  entry.modules = modules;
  entry.module_count = count;
}

std::optional<AotGameEntry> AotRegistry::Find(const std::string& game_id) const
{
  auto it = m_games.find(game_id);
  if (it != m_games.end())
    return it->second;
  return std::nullopt;
}

std::vector<std::string> AotRegistry::GetRegisteredGameIDs() const
{
  std::vector<std::string> ids;
  ids.reserve(m_games.size());
  for (const auto& [id, entry] : m_games)
    ids.push_back(id);
  return ids;
}

extern "C" void aot_register_game(const char* game_id, AOTDispatchFunc dispatch,
                                  AOTLookupFunc lookup)
{
  AotRegistry::Instance().Register(game_id, dispatch, lookup);
}

extern "C" void aot_register_game_modules(const char* game_id, const void* modules,
                                          uint32_t count)
{
  AotRegistry::Instance().RegisterModules(game_id,
                                          static_cast<const AotModuleDesc*>(modules), count);
}
