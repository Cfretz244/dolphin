// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexLoaderBase.h"

#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <vector>

#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "Core/ConfigManager.h"
#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoaderAOT.h"
#include "VideoCommon/VertexLoaderAotRegistry.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexLoader_Color.h"
#include "VideoCommon/VertexLoader_Normal.h"
#include "VideoCommon/VertexLoader_Position.h"
#include "VideoCommon/VertexLoader_TextCoord.h"
#include "VideoCommon/VideoConfig.h"

#include <TargetConditionals.h>
#ifdef _M_X86_64
#include "VideoCommon/VertexLoaderX64.h"
#elif defined(_M_ARM_64) && !TARGET_OS_IOS
#include "VideoCommon/VertexLoaderARM64.h"
#endif

// a hacky implementation to compare two vertex loaders
class VertexLoaderTester : public VertexLoaderBase
{
public:
  VertexLoaderTester(std::unique_ptr<VertexLoaderBase> a_, std::unique_ptr<VertexLoaderBase> b_,
                     const TVtxDesc& vtx_desc, const VAT& vtx_attr)
      : VertexLoaderBase(vtx_desc, vtx_attr), a(std::move(a_)), b(std::move(b_))
  {
    ASSERT(a && b);
    if (a->m_vertex_size == b->m_vertex_size && a->m_native_components == b->m_native_components &&
        a->m_native_vtx_decl.stride == b->m_native_vtx_decl.stride)
    {
      // These are generated from the VAT and vertex desc, so they should match.
      // m_native_vtx_decl.stride isn't set yet, though.
      ASSERT(m_vertex_size == a->m_vertex_size && m_native_components == a->m_native_components);

      memcpy(&m_native_vtx_decl, &a->m_native_vtx_decl, sizeof(PortableVertexDeclaration));
    }
    else
    {
      PanicAlertFmt("Can't compare vertex loaders that expect different vertex formats!\n"
                    "a: m_vertex_size {}, m_native_components {:#010x}, stride {}\n"
                    "b: m_vertex_size {}, m_native_components {:#010x}, stride {}",
                    a->m_vertex_size, a->m_native_components, a->m_native_vtx_decl.stride,
                    b->m_vertex_size, b->m_native_components, b->m_native_vtx_decl.stride);
    }
  }
  int RunVertices(const u8* src, u8* dst, int count) override
  {
    buffer_a.resize(count * a->m_native_vtx_decl.stride + 4);
    buffer_b.resize(count * b->m_native_vtx_decl.stride + 4);

    const std::array<u32, 3> old_position_matrix_index_cache =
        VertexLoaderManager::position_matrix_index_cache;
    const std::array<std::array<float, 4>, 3> old_position_cache =
        VertexLoaderManager::position_cache;
    const std::array<float, 4> old_normal_cache = VertexLoaderManager::normal_cache;
    const std::array<float, 4> old_tangent_cache = VertexLoaderManager::tangent_cache;
    const std::array<float, 4> old_binormal_cache = VertexLoaderManager::binormal_cache;

    const int count_a = a->RunVertices(src, buffer_a.data(), count);

    const std::array<u32, 3> a_position_matrix_index_cache =
        VertexLoaderManager::position_matrix_index_cache;
    const std::array<std::array<float, 4>, 3> a_position_cache =
        VertexLoaderManager::position_cache;
    const std::array<float, 4> a_normal_cache = VertexLoaderManager::normal_cache;
    const std::array<float, 4> a_tangent_cache = VertexLoaderManager::tangent_cache;
    const std::array<float, 4> a_binormal_cache = VertexLoaderManager::binormal_cache;

    // Reset state before running b
    VertexLoaderManager::position_matrix_index_cache = old_position_matrix_index_cache;
    VertexLoaderManager::position_cache = old_position_cache;
    VertexLoaderManager::normal_cache = old_normal_cache;
    VertexLoaderManager::tangent_cache = old_tangent_cache;
    VertexLoaderManager::binormal_cache = old_binormal_cache;

    const int count_b = b->RunVertices(src, buffer_b.data(), count);

    const std::array<u32, 3> b_position_matrix_index_cache =
        VertexLoaderManager::position_matrix_index_cache;
    const std::array<std::array<float, 4>, 3> b_position_cache =
        VertexLoaderManager::position_cache;
    const std::array<float, 4> b_normal_cache = VertexLoaderManager::normal_cache;
    const std::array<float, 4> b_tangent_cache = VertexLoaderManager::tangent_cache;
    const std::array<float, 4> b_binormal_cache = VertexLoaderManager::binormal_cache;

    ASSERT_MSG(VIDEO, count_a == count_b,
               "The two vertex loaders have loaded a different amount of vertices (a: {}, b: {}).",
               count_a, count_b);

    ASSERT_MSG(VIDEO,
               memcmp(buffer_a.data(), buffer_b.data(),
                      std::min(count_a, count_b) * m_native_vtx_decl.stride) == 0,
               "The two vertex loaders have loaded different data.  Configuration:"
               "\nVertex desc:\n{}\n\nVertex attr:\n{}",
               m_VtxDesc, m_VtxAttr);

    ASSERT_MSG(VIDEO, a_position_matrix_index_cache == b_position_matrix_index_cache,
               "Expected matching position matrix caches after loading (a: {}; b: {})",
               fmt::join(a_position_matrix_index_cache, ", "),
               fmt::join(b_position_matrix_index_cache, ", "));

    // Some games (e.g. Donkey Kong Country Returns) have a few draws that contain NaN.
    // Since NaN != NaN, we need to compare the bits instead.
    const auto bit_equal = [](float val_a, float val_b) {
      return std::bit_cast<u32>(val_a) == std::bit_cast<u32>(val_b);
    };

    // The last element is allowed to be garbage for SIMD overwrites.
    // For XY, the last 2 are garbage.
    const bool positions_match = [&] {
      const size_t max_component = m_VtxAttr.g0.PosElements == CoordComponentCount::XYZ ? 3 : 2;
      for (size_t vertex = 0; vertex < 3; vertex++)
      {
        if (!std::equal(a_position_cache[vertex].begin(),
                        a_position_cache[vertex].begin() + max_component,
                        b_position_cache[vertex].begin(), bit_equal))
        {
          return false;
        }
      }
      return true;
    }();

    ASSERT_MSG(VIDEO, positions_match,
               "Expected matching position caches after loading (a: {} / {} / {}; b: {} / {} / {})",
               fmt::join(a_position_cache[0], ", "), fmt::join(a_position_cache[1], ", "),
               fmt::join(a_position_cache[2], ", "), fmt::join(b_position_cache[0], ", "),
               fmt::join(b_position_cache[1], ", "), fmt::join(b_position_cache[2], ", "));

    // The last element is allowed to be garbage for SIMD overwrites
    ASSERT_MSG(VIDEO,
               std::equal(a_normal_cache.begin(), a_normal_cache.begin() + 3,
                          b_normal_cache.begin(), b_normal_cache.begin() + 3, bit_equal),
               "Expected matching normal caches after loading (a: {}; b: {})",
               fmt::join(a_normal_cache, ", "), fmt::join(b_normal_cache, ", "));

    ASSERT_MSG(VIDEO,
               std::equal(a_tangent_cache.begin(), a_tangent_cache.begin() + 3,
                          b_tangent_cache.begin(), b_tangent_cache.begin() + 3, bit_equal),
               "Expected matching tangent caches after loading (a: {}; b: {})",
               fmt::join(a_tangent_cache, ", "), fmt::join(b_tangent_cache, ", "));

    ASSERT_MSG(VIDEO,
               std::equal(a_binormal_cache.begin(), a_binormal_cache.begin() + 3,
                          b_binormal_cache.begin(), b_binormal_cache.begin() + 3, bit_equal),
               "Expected matching binormal caches after loading (a: {}; b: {})",
               fmt::join(a_binormal_cache, ", "), fmt::join(b_binormal_cache, ", "));

    memcpy(dst, buffer_a.data(), count_a * m_native_vtx_decl.stride);
    m_numLoadedVertices += count;
    return count_a;
  }

private:
  std::unique_ptr<VertexLoaderBase> a;
  std::unique_ptr<VertexLoaderBase> b;

  std::vector<u8> buffer_a;
  std::vector<u8> buffer_b;
};

u32 VertexLoaderBase::GetVertexSize(const TVtxDesc& vtx_desc, const VAT& vtx_attr)
{
  u32 size = 0;

  // Each enabled TexMatIdx adds one byte, as does PosMatIdx
  size += std::popcount(vtx_desc.low.Hex & 0x1FF);

  const u32 pos_size = VertexLoader_Position::GetSize(vtx_desc.low.Position, vtx_attr.g0.PosFormat,
                                                      vtx_attr.g0.PosElements);
  size += pos_size;
  const u32 norm_size =
      VertexLoader_Normal::GetSize(vtx_desc.low.Normal, vtx_attr.g0.NormalFormat,
                                   vtx_attr.g0.NormalElements, vtx_attr.g0.NormalIndex3);
  size += norm_size;
  for (u32 i = 0; i < vtx_desc.low.Color.Size(); i++)
  {
    const u32 color_size =
        VertexLoader_Color::GetSize(vtx_desc.low.Color[i], vtx_attr.GetColorFormat(i));
    size += color_size;
  }
  for (u32 i = 0; i < vtx_desc.high.TexCoord.Size(); i++)
  {
    const u32 tc_size = VertexLoader_TextCoord::GetSize(
        vtx_desc.high.TexCoord[i], vtx_attr.GetTexFormat(i), vtx_attr.GetTexElements(i));
    size += tc_size;
  }

  return size;
}

u32 VertexLoaderBase::GetVertexComponents(const TVtxDesc& vtx_desc, const VAT& vtx_attr)
{
  u32 components = 0;
  if (vtx_desc.low.PosMatIdx)
    components |= VB_HAS_POSMTXIDX;
  for (u32 i = 0; i < vtx_desc.low.TexMatIdx.Size(); i++)
  {
    if (vtx_desc.low.TexMatIdx[i])
      components |= VB_HAS_TEXMTXIDX0 << i;
  }
  // Vertices always have positions; thus there is no VB_HAS_POS as it would always be set
  if (vtx_desc.low.Normal != VertexComponentFormat::NotPresent)
  {
    components |= VB_HAS_NORMAL;
    if (vtx_attr.g0.NormalElements == NormalComponentCount::NTB)
      components |= VB_HAS_TANGENT | VB_HAS_BINORMAL;
  }
  for (u32 i = 0; i < vtx_desc.low.Color.Size(); i++)
  {
    if (vtx_desc.low.Color[i] != VertexComponentFormat::NotPresent)
      components |= VB_HAS_COL0 << i;
  }
  for (u32 i = 0; i < vtx_desc.high.TexCoord.Size(); i++)
  {
    if (vtx_desc.high.TexCoord[i] != VertexComponentFormat::NotPresent)
      components |= VB_HAS_UV0 << i;
  }
  return components;
}

std::unique_ptr<VertexLoaderBase> VertexLoaderBase::CreateVertexLoader(const TVtxDesc& vtx_desc,
                                                                       const VAT& vtx_attr)
{
  const VertexLoaderType loader_type = g_ActiveConfig.vertex_loader_type;

  // Try AOT vertex loader first (critical for iOS, also works on desktop for validation)
  if (loader_type != VertexLoaderType::Software)
  {
    auto& registry = VertexLoaderAotRegistry::Instance();
    if (registry.HasEntriesForGame(SConfig::GetInstance().GetGameID()))
    {
      VertexLoaderAotRegistry::Key key = {vtx_desc.low.Hex, vtx_desc.high.Hex, vtx_attr.g0.Hex,
                                          vtx_attr.g1.Hex, vtx_attr.g2.Hex};
      if (auto* entry = registry.Find(SConfig::GetInstance().GetGameID(), key))
      {
        auto aot_loader = std::make_unique<VertexLoaderAOT>(vtx_desc, vtx_attr, *entry);
        if (loader_type != VertexLoaderType::Compare)
          return aot_loader;
        // In compare mode, fall through to create a native/software loader to compare against
      }
      else
      {
        // Diagnostic: this game HAS AOT loaders but this format wasn't in the traces —
        // it runs on the (much slower) fallback path. Loaders are cached per format, so
        // this fires once per missed format; a burst of these in a heavy area explains
        // stutter and means the traces should be extended through that content.
        fprintf(stderr,
                "VtxAOT: format miss for %s (%08x %08x %08x %08x %08x) — using fallback "
                "loader\n",
                SConfig::GetInstance().GetGameID().c_str(), key[0], key[1], key[2], key[3],
                key[4]);
      }
    }
  }

  if (loader_type == VertexLoaderType::Software)
  {
    return std::make_unique<VertexLoader>(vtx_desc, vtx_attr);
  }

  std::unique_ptr<VertexLoaderBase> native_loader = nullptr;

#if defined(_M_X86_64)
  native_loader = std::make_unique<VertexLoaderX64>(vtx_desc, vtx_attr);
#elif defined(_M_ARM_64) && !TARGET_OS_IOS
  native_loader = std::make_unique<VertexLoaderARM64>(vtx_desc, vtx_attr);
#endif

  // Use the software loader as a fallback
  // (not currently applicable, as both VertexLoaderX64 and VertexLoaderARM64
  // are always usable, but if a loader that only works on some CPUs is created
  // then this fallback would be used)
  if (!native_loader)
  {
    return std::make_unique<VertexLoader>(vtx_desc, vtx_attr);
  }

  if (loader_type == VertexLoaderType::Compare)
  {
    // If AOT loader is available, compare AOT against native JIT
    auto& registry = VertexLoaderAotRegistry::Instance();
    VertexLoaderAotRegistry::Key key = {vtx_desc.low.Hex, vtx_desc.high.Hex, vtx_attr.g0.Hex,
                                        vtx_attr.g1.Hex, vtx_attr.g2.Hex};
    auto* entry = registry.HasEntries() ?
        registry.Find(SConfig::GetInstance().GetGameID(), key) : nullptr;
    if (entry)
    {
      return std::make_unique<VertexLoaderTester>(
          std::make_unique<VertexLoaderAOT>(vtx_desc, vtx_attr, *entry),  // AOT
          std::move(native_loader),                                        // JIT reference
          vtx_desc, vtx_attr);
    }
    return std::make_unique<VertexLoaderTester>(
        std::make_unique<VertexLoader>(vtx_desc, vtx_attr),  // the software one
        std::move(native_loader),                            // the new one to compare
        vtx_desc, vtx_attr);
  }

  return native_loader;
}
