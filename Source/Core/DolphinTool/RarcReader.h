// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace DolphinTool
{
// Minimal reader for Nintendo RARC archives (magic "RARC"), enough to enumerate
// and slice out member files (e.g. the .rel modules inside Wind Waker's
// RELS.arc). Members may themselves be Yaz0-compressed; callers sniff that.

struct RarcMember
{
  std::string name;
  u32 data_offset;  // absolute offset into the archive buffer
  u32 size;
};

bool IsRarc(const u8* data, size_t size);

// Returns all file (non-directory) members. Empty on parse failure.
std::vector<RarcMember> RarcListFiles(const u8* data, size_t size);
}  // namespace DolphinTool
