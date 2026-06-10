// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/RarcReader.h"

#include <cstring>

#include "Common/Swap.h"

namespace DolphinTool
{
namespace
{
u32 ReadU32(const u8* data, size_t offset)
{
  u32 v;
  std::memcpy(&v, data + offset, 4);
  return Common::swap32(v);
}

u16 ReadU16(const u8* data, size_t offset)
{
  u16 v;
  std::memcpy(&v, data + offset, 2);
  return Common::swap16(v);
}
}  // namespace

bool IsRarc(const u8* data, size_t size)
{
  return size >= 0x40 && std::memcmp(data, "RARC", 4) == 0;
}

std::vector<RarcMember> RarcListFiles(const u8* data, size_t size)
{
  std::vector<RarcMember> out;
  if (!IsRarc(data, size))
    return out;

  // Header (0x20 bytes), then the info block all of whose offsets are relative
  // to the info block start at 0x20.
  const u32 data_start = ReadU32(data, 0x0C) + 0x20;
  const u32 num_entries = ReadU32(data, 0x28);
  const u32 entries_off = ReadU32(data, 0x2C) + 0x20;
  const u32 strtab_size = ReadU32(data, 0x30);
  const u32 strtab_off = ReadU32(data, 0x34) + 0x20;

  if (entries_off + static_cast<u64>(num_entries) * 0x14 > size || strtab_off > size)
    return out;

  for (u32 i = 0; i < num_entries; i++)
  {
    const size_t e = entries_off + i * 0x14;
    const u8 flags = data[e + 4];
    const bool is_dir = (flags & 0x02) != 0;
    if (is_dir)
      continue;

    const u16 name_off = ReadU16(data, e + 6);
    const u32 member_off = ReadU32(data, e + 8);
    const u32 member_size = ReadU32(data, e + 12);

    if (name_off >= strtab_size)
      continue;
    const char* name_start = reinterpret_cast<const char*>(data + strtab_off + name_off);
    const size_t max_len = size - (strtab_off + name_off);
    const size_t name_len = strnlen(name_start, max_len);

    const u64 abs_off = static_cast<u64>(data_start) + member_off;
    if (abs_off + member_size > size)
      continue;

    out.push_back({std::string(name_start, name_len), static_cast<u32>(abs_off), member_size});
  }
  return out;
}
}  // namespace DolphinTool
