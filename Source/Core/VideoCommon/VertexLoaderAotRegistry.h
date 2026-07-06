// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "VideoCommon/NativeVertexFormat.h"

// Function pointer type matching generated vertex loader C functions
typedef int (*VtxLoaderAOTFunc)(const u8* src, u8* dst, int count,
                                const u8* const* arraybases, const u32* strides,
                                void* zfreeze_state);

struct VtxLoaderAotEntry
{
  VtxLoaderAOTFunc func;
  PortableVertexDeclaration decl;
  u32 vertex_size;
  u32 native_components;
};

class VertexLoaderAotRegistry
{
public:
  using Key = std::array<u32, 5>;

  static VertexLoaderAotRegistry& Instance();

  void Register(const std::string& game_id, const Key& key, VtxLoaderAOTFunc func,
                const PortableVertexDeclaration& decl, u32 vertex_size, u32 native_components);

  const VtxLoaderAotEntry* Find(const std::string& game_id, const Key& key) const;

  bool HasEntries() const { return !m_games.empty(); }

  // Whether THIS game registered any loaders. A multi-game binary links vtx AOT
  // for some games and not others (e.g. Melee's is backed out), so per-game
  // checks must use this, not HasEntries().
  bool HasEntriesForGame(const std::string& game_id) const
  {
    return m_games.find(game_id) != m_games.end();
  }

private:
  struct KeyHash
  {
    size_t operator()(const Key& k) const
    {
      // Same polynomial hash as VertexLoaderUID
      size_t h = 0;
      for (u32 v : k)
        h = h * 137 + v;
      return h;
    }
  };

  std::unordered_map<std::string, std::unordered_map<Key, VtxLoaderAotEntry, KeyHash>> m_games;
};

// C structs matching the generated code's VtxPortableDecl / VtxAttrFormat
struct VtxAttrFormatC
{
  int type;
  int components;
  int offset;
  int enable;
  int integer;
};

struct VtxPortableDeclC
{
  int stride;
  VtxAttrFormatC position;
  VtxAttrFormatC normals[3];
  VtxAttrFormatC colors[2];
  VtxAttrFormatC texcoords[8];
  VtxAttrFormatC posmtx;
};

// Called by __attribute__((constructor)) in generated AOT vertex loader libraries.
extern "C" void vtx_aot_register_loader(const char* game_id, uint32_t desc_low, uint32_t desc_high,
                                        uint32_t g0, uint32_t g1, uint32_t g2,
                                        VtxLoaderAOTFunc func, const VtxPortableDeclC* decl,
                                        uint32_t vertex_size, uint32_t native_components);
