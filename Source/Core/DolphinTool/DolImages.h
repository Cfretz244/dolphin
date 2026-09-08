// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>
#include "DolphinTool/OverlayImages.h"

namespace DiscIO
{
class Volume;
}
namespace DolphinTool
{
// Opt-in snapshot-backed fixed-address images; separate from SDK REL modules.
// Every emitted block has an instruction-content guard, including direct edges.
bool WriteDolImageCFG(const DiscIO::Volume& volume,
                      const std::vector<TraceSnapshotBlock>& snapshots, const std::string& path);
bool IsDolImageCFG(const std::string& path);
bool TranslateDolImages(const DiscIO::Volume& volume, const std::string& cfg,
                        const std::string& output, const std::string& prefix,
                        const std::string& boot_hash);
}
