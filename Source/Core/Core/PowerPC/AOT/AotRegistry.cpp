// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT/AotRegistry.h"

#include <algorithm>

#include "Common/Logging/Log.h"

AotRegistry& AotRegistry::Instance()
{
  static AotRegistry instance;
  return instance;
}

bool AotRegistry::IsRejected(const std::string& game_id) const
{
  return std::find(m_rejected.begin(), m_rejected.end(), game_id) != m_rejected.end();
}

void AotRegistry::Register(const std::string& game_id, AOTDispatchFunc dispatch,
                           AOTLookupFunc lookup, uint32_t abi_version)
{
  if (abi_version != AOT_ABI_VERSION)
  {
    // A library generated against a different aot_runtime.h. Running it would
    // silently corrupt state; the game runs on the interpreter instead.
    ERROR_LOG_FMT(AOT,
                  "AotRegistry: {} was built against AOT ABI v{}, runtime is v{} — rejecting "
                  "library. Re-run `stack.sh translate {}` and rebuild.",
                  game_id, abi_version, AOT_ABI_VERSION, game_id);
    m_rejected.push_back(game_id);
    return;
  }
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
  if (IsRejected(game_id))
    return;
  auto& entry = m_games[game_id];
  if (entry.game_id.empty())
    entry.game_id = game_id;
  entry.modules = modules;
  entry.module_count = count;
}

void AotRegistry::RegisterBlockSizes(const std::string& game_id, const AotBlockSize* blocks,
                                     uint32_t count, const AotModuleBlockSize* module_blocks,
                                     uint32_t module_count)
{
  if (IsRejected(game_id))
    return;
  auto& entry = m_games[game_id];
  if (entry.game_id.empty())
    entry.game_id = game_id;
  entry.block_sizes = blocks;
  entry.block_size_count = count;
  entry.module_block_sizes = module_blocks;
  entry.module_block_size_count = module_count;
}

void AotRegistry::RegisterImageHash(const std::string& game_id, const char* dol_sha256_hex)
{
  if (IsRejected(game_id))
    return;
  auto& entry = m_games[game_id];
  if (entry.game_id.empty())
    entry.game_id = game_id;
  entry.dol_sha256 = dol_sha256_hex != nullptr ? dol_sha256_hex : "";
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
                                  AOTLookupFunc lookup, uint32_t abi_version)
{
  AotRegistry::Instance().Register(game_id, dispatch, lookup, abi_version);
}

extern "C" void aot_register_game_modules(const char* game_id, const AotModuleDesc* modules,
                                          uint32_t count)
{
  AotRegistry::Instance().RegisterModules(game_id, modules, count);
}

extern "C" void aot_register_block_sizes(const char* game_id, const AotBlockSize* blocks,
                                         uint32_t count, const AotModuleBlockSize* module_blocks,
                                         uint32_t module_count)
{
  AotRegistry::Instance().RegisterBlockSizes(game_id, blocks, count, module_blocks, module_count);
}

extern "C" void aot_register_game_image(const char* game_id, const char* dol_sha256_hex)
{
  AotRegistry::Instance().RegisterImageHash(game_id, dol_sha256_hex);
}
