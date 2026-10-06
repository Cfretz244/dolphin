// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "DolphinTool/OverlayImages.h"
#include <set>
#include <utility>
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
// trace_ranges: address ranges whose code exists nowhere on disc (e.g. a
// launcher's relocated DOL-loader stub); their bytes come from image_snapshots
// (which must be self-consistent) and are stored in the CFG database.
bool WriteDolImageCFG(const DiscIO::Volume& volume,
                      const std::vector<TraceSnapshotBlock>& snapshots, const std::string& path,
                      bool primehack = false,
                      const std::vector<std::pair<u32, u32>>& trace_ranges = {},
                      const std::vector<TraceSnapshotBlock>& image_snapshots = {});
bool IsDolImageCFG(const std::string& path);
bool TranslateDolImages(const DiscIO::Volume& volume, const std::string& cfg,
                        const std::string& output, const std::string& prefix,
                        const std::string& boot_hash);
// Trusted multi-image translation of the same CFG: per-image flat tables with
// no per-block guards, selected at runtime by AotImageTracker (aot_images.h).
// `selected` restricts emission to those base image ids (empty = all bases).
// `real_mode_dumps`: raw dumps of physical MEM1 [0, N<=0x3000) holding the OS
// exception vectors; each distinct one becomes an AOT_IMAGE_REAL_MODE image.
bool TranslateTrustedImages(const DiscIO::Volume& volume, const std::string& cfg,
                            const std::string& output, const std::string& prefix,
                            const std::string& boot_hash, const std::set<size_t>& selected,
                            const std::vector<std::string>& real_mode_dumps = {});
} // namespace DolphinTool
