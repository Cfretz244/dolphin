// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/Yaz0.h"

#include <cstring>

#include "Common/Swap.h"

namespace DolphinTool
{
bool IsYaz0(const u8* data, size_t size)
{
  return size >= 16 && std::memcmp(data, "Yaz0", 4) == 0;
}

std::optional<std::vector<u8>> Yaz0Decompress(const u8* data, size_t size)
{
  if (!IsYaz0(data, size))
    return std::nullopt;

  u32 out_size;
  std::memcpy(&out_size, data + 4, 4);
  out_size = Common::swap32(out_size);

  std::vector<u8> out;
  out.reserve(out_size);

  size_t src = 16;
  u8 code = 0;
  int bits = 0;
  while (out.size() < out_size)
  {
    if (bits == 0)
    {
      if (src >= size)
        return std::nullopt;
      code = data[src++];
      bits = 8;
    }
    if (code & 0x80)
    {
      if (src >= size)
        return std::nullopt;
      out.push_back(data[src++]);
    }
    else
    {
      if (src + 2 > size)
        return std::nullopt;
      const u8 b1 = data[src];
      const u8 b2 = data[src + 1];
      src += 2;
      const u32 dist = (static_cast<u32>(b1 & 0x0F) << 8 | b2) + 1;
      u32 len = b1 >> 4;
      if (len == 0)
      {
        if (src >= size)
          return std::nullopt;
        len = data[src++] + 0x12;
      }
      else
      {
        len += 2;
      }
      if (dist > out.size())
        return std::nullopt;
      for (u32 i = 0; i < len; i++)
        out.push_back(out[out.size() - dist]);
    }
    code <<= 1;
    bits--;
  }
  return out;
}
}  // namespace DolphinTool
