// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexLoaderAotRegistry.h"

#include "Common/Logging/Log.h"

VertexLoaderAotRegistry& VertexLoaderAotRegistry::Instance()
{
  static VertexLoaderAotRegistry instance;
  return instance;
}

void VertexLoaderAotRegistry::Register(const std::string& game_id, const Key& key,
                                       VtxLoaderAOTFunc func,
                                       const PortableVertexDeclaration& decl, u32 vertex_size,
                                       u32 native_components)
{
  VtxLoaderAotEntry entry;
  entry.func = func;
  entry.decl = decl;
  entry.vertex_size = vertex_size;
  entry.native_components = native_components;
  m_games[game_id][key] = entry;
}

const VtxLoaderAotEntry* VertexLoaderAotRegistry::Find(const std::string& game_id,
                                                        const Key& key) const
{
  auto game_it = m_games.find(game_id);
  if (game_it == m_games.end())
    return nullptr;
  auto loader_it = game_it->second.find(key);
  if (loader_it == game_it->second.end())
    return nullptr;
  return &loader_it->second;
}

// Convert the C-compatible VtxPortableDeclC to Dolphin's PortableVertexDeclaration
static PortableVertexDeclaration ConvertDecl(const VtxPortableDeclC* c)
{
  PortableVertexDeclaration d{};
  d.stride = c->stride;

  auto convert_attr = [](const VtxAttrFormatC& src) {
    AttributeFormat dst{};
    dst.type = static_cast<ComponentFormat>(src.type);
    dst.components = src.components;
    dst.offset = src.offset;
    dst.enable = src.enable != 0;
    dst.integer = src.integer != 0;
    return dst;
  };

  d.position = convert_attr(c->position);
  for (int i = 0; i < 3; i++)
    d.normals[i] = convert_attr(c->normals[i]);
  for (int i = 0; i < 2; i++)
    d.colors[i] = convert_attr(c->colors[i]);
  for (int i = 0; i < 8; i++)
    d.texcoords[i] = convert_attr(c->texcoords[i]);
  d.posmtx = convert_attr(c->posmtx);

  return d;
}

extern "C" void vtx_aot_register_loader(const char* game_id, uint32_t desc_low,
                                        uint32_t desc_high, uint32_t g0, uint32_t g1, uint32_t g2,
                                        VtxLoaderAOTFunc func, const VtxPortableDeclC* decl,
                                        uint32_t vertex_size, uint32_t native_components)
{
  VertexLoaderAotRegistry::Key key = {desc_low, desc_high, g0, g1, g2};
  PortableVertexDeclaration pvd = ConvertDecl(decl);
  VertexLoaderAotRegistry::Instance().Register(game_id, key, func, pvd, vertex_size,
                                               native_components);
  INFO_LOG_FMT(VIDEO,
               "VtxAOT: Registered loader for {} key=[{:08x},{:08x},{:08x},{:08x},{:08x}] "
               "vtx_size={} stride={}",
               game_id, desc_low, desc_high, g0, g1, g2, vertex_size, pvd.stride);
}
