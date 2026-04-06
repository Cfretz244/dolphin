// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexLoaderAOT.h"

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoaderManager.h"

// Z-freeze state struct matching the generated C code's VtxLoaderState.
// Passed by pointer so generated code can update z-freeze caches.
struct VtxLoaderZFreezeState
{
  float (*position_cache)[4];
  u32* position_matrix_index_cache;
  float* normal_cache;
  float* tangent_cache;
  float* binormal_cache;
};

// Verify layout compatibility with the generated C struct
static_assert(sizeof(VtxLoaderZFreezeState) == 5 * sizeof(void*),
              "VtxLoaderZFreezeState must be 5 pointers");

VertexLoaderAOT::VertexLoaderAOT(const TVtxDesc& vtx_desc, const VAT& vtx_attr,
                                 const VtxLoaderAotEntry& entry)
    : VertexLoaderBase(vtx_desc, vtx_attr), m_func(entry.func)
{
  // Use the pre-computed values from the AOT registry entry
  m_native_vtx_decl = entry.decl;
  // m_vertex_size and m_native_components are set by VertexLoaderBase constructor
  // via GetVertexSize() and GetVertexComponents(), but verify they match
  ASSERT_MSG(VIDEO, m_vertex_size == entry.vertex_size,
             "AOT vertex size mismatch: computed={} baked={}", m_vertex_size, entry.vertex_size);
  ASSERT_MSG(VIDEO, m_native_components == entry.native_components,
             "AOT native components mismatch: computed={:#x} baked={:#x}", m_native_components,
             entry.native_components);
}

int VertexLoaderAOT::RunVertices(const u8* src, u8* dst, int count)
{
  m_numLoadedVertices += count;

  // Populate z-freeze state with pointers to VertexLoaderManager's caches
  VtxLoaderZFreezeState zf;
  zf.position_cache =
      reinterpret_cast<float(*)[4]>(VertexLoaderManager::position_cache.data());
  zf.position_matrix_index_cache = VertexLoaderManager::position_matrix_index_cache.data();
  zf.normal_cache = VertexLoaderManager::normal_cache.data();
  zf.tangent_cache = VertexLoaderManager::tangent_cache.data();
  zf.binormal_cache = VertexLoaderManager::binormal_cache.data();

  return m_func(src, dst, count,
                reinterpret_cast<const u8* const*>(VertexLoaderManager::cached_arraybases.data()),
                g_main_cp_state.array_strides.data(), &zf);
}
