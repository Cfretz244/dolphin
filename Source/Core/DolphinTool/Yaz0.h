// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <vector>

#include "Common/CommonTypes.h"

namespace DolphinTool
{
// Nintendo's Yaz0 RLE compression, used for .rel files and RARC members on
// GameCube discs (magic "Yaz0", big-endian decompressed size, 8 reserved bytes,
// then bit-packed literal/backref groups).

bool IsYaz0(const u8* data, size_t size);

// Returns the decompressed payload, or nullopt on malformed input.
std::optional<std::vector<u8>> Yaz0Decompress(const u8* data, size_t size);
}  // namespace DolphinTool
