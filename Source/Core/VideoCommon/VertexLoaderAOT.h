// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/VertexLoaderAotRegistry.h"
#include "VideoCommon/VertexLoaderBase.h"

// VertexLoaderBase implementation that delegates to a pre-compiled AOT vertex loader
// function. Used on iOS where JIT vertex loaders are unavailable.
class VertexLoaderAOT : public VertexLoaderBase
{
public:
  VertexLoaderAOT(const TVtxDesc& vtx_desc, const VAT& vtx_attr,
                  const VtxLoaderAotEntry& entry);

  int RunVertices(const u8* src, u8* dst, int count) override;

private:
  VtxLoaderAOTFunc m_func;
};
