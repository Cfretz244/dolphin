// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

class PPCMemoryImage;

namespace DolphinTool
{
// Decode original disc instructions, following static control flow from trace seeds.
std::map<u32, u32> DisassembleDolSeeds(const PPCMemoryImage& memory, const std::set<u32>& seeds);

int CfgCommand(const std::vector<std::string>& args);
}
