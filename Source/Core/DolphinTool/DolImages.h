// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "DolphinTool/OverlayImages.h"
#include <set>
#include <string>
#include <vector>

namespace DiscIO
{
class Volume;
}
namespace DolphinTool
{
// Opt-in snapshot-backed fixed-address images; separate from SDK REL modules.
// Every emitted block has an instruction-content guard, including direct edges.
bool WriteDolImageCFG(const DiscIO::Volume& volume,
                      const std::vector<TraceSnapshotBlock>& snapshots, const std::string& path,
                      bool primehack = false);
bool IsDolImageCFG(const std::string& path);
bool TranslateDolImages(const DiscIO::Volume& volume, const std::string& cfg,
                        const std::string& output, const std::string& prefix,
                        const std::string& boot_hash);
// Trusted multi-image translation of the same CFG: per-image flat tables with
// no per-block guards, selected at runtime by AotImageTracker (aot_images.h).
// `selected` restricts emission to those base image ids (empty = all bases).
bool TranslateTrustedImages(const DiscIO::Volume& volume, const std::string& cfg,
                            const std::string& output, const std::string& prefix,
                            const std::string& boot_hash, const std::set<size_t>& selected);
} // namespace DolphinTool
