// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "DolphinTool/RelFile.h"

namespace DiscIO
{
class Volume;
}

namespace DolphinTool
{
// Discovers every .rel module on a GameCube disc: plain *.rel files anywhere in
// the filesystem (Wind Waker streams ~235 from rels/) and *.rel members inside
// *.arc RARC archives (Wind Waker keeps ~180 resident ones in RELS.arc), with
// transparent Yaz0 decompression of either. Duplicate module ids are dropped
// with a warning (first occurrence wins).
std::vector<RelFile> DiscoverRelModules(const DiscIO::Volume& volume, bool verbose);
}  // namespace DolphinTool
