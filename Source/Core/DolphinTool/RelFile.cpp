// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/RelFile.h"

#include <cstring>
#include <iostream>

#include <fmt/format.h>
#include <fmt/ostream.h>

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

std::optional<RelFile> RelFile::Parse(const u8* data, size_t size, const std::string& name)
{
  // v1 header is 0x40 bytes; v2 adds align/bssAlign (0x48); v3 adds fixSize (0x4C).
  if (size < 0x40)
  {
    fmt::println(std::cerr, "REL {}: too small ({} bytes)", name, size);
    return std::nullopt;
  }

  RelFile rel;
  rel.name = name;
  rel.module_id = ReadU32(data, 0x00);
  const u32 num_sections = ReadU32(data, 0x0C);
  const u32 section_info_offset = ReadU32(data, 0x10);
  rel.version = ReadU32(data, 0x1C);
  rel.bss_size = ReadU32(data, 0x20);
  const u32 rel_offset = ReadU32(data, 0x24);
  const u32 imp_offset = ReadU32(data, 0x28);
  const u32 imp_size = ReadU32(data, 0x2C);
  rel.prolog_section = data[0x30];
  rel.epilog_section = data[0x31];
  rel.unresolved_section = data[0x32];
  rel.prolog_offset = ReadU32(data, 0x34);
  rel.epilog_offset = ReadU32(data, 0x38);
  rel.unresolved_offset = ReadU32(data, 0x3C);

  if (rel.version < 1 || rel.version > 3)
  {
    fmt::println(std::cerr, "REL {}: unsupported version {}", name, rel.version);
    return std::nullopt;
  }
  if (num_sections == 0 || num_sections > 64)
  {
    fmt::println(std::cerr, "REL {}: implausible section count {}", name, num_sections);
    return std::nullopt;
  }
  if (section_info_offset + static_cast<u64>(num_sections) * 8 > size)
  {
    fmt::println(std::cerr, "REL {}: section table out of bounds", name);
    return std::nullopt;
  }

  // Section table: u32 offset (bit 0 = executable flag), u32 size.
  // offset 0 with nonzero size denotes .bss.
  rel.sections.resize(num_sections);
  for (u32 i = 0; i < num_sections; i++)
  {
    const size_t e = section_info_offset + i * 8;
    const u32 raw_offset = ReadU32(data, e);
    const u32 sec_size = ReadU32(data, e + 4);
    auto& sec = rel.sections[i];
    sec.file_offset = raw_offset & ~3u;
    sec.executable = (raw_offset & 1) != 0;
    sec.size = sec_size;
    if (sec.file_offset != 0 && sec_size != 0)
    {
      if (static_cast<u64>(sec.file_offset) + sec_size > size)
      {
        fmt::println(std::cerr, "REL {}: section {} out of bounds ({:#x}+{:#x} > {:#x})", name, i,
                     sec.file_offset, sec_size, size);
        return std::nullopt;
      }
      sec.data.assign(data + sec.file_offset, data + sec.file_offset + sec_size);
    }
  }

  // Imp table: pairs of (target module id, offset of that module's relocation
  // stream within the file). Each stream patches THIS module against symbols in
  // the target module.
  if (imp_offset + static_cast<u64>(imp_size) > size || imp_size % 8 != 0)
  {
    fmt::println(std::cerr, "REL {}: imp table out of bounds", name);
    return std::nullopt;
  }

  for (u32 imp = 0; imp < imp_size; imp += 8)
  {
    const u32 target_module = ReadU32(data, imp_offset + imp);
    const u32 stream_offset = ReadU32(data, imp_offset + imp + 4);
    if (stream_offset < rel_offset || stream_offset >= size)
    {
      fmt::println(std::cerr, "REL {}: relocation stream for module {} out of bounds", name,
                   target_module);
      return std::nullopt;
    }

    // Stream entries: u16 position delta, u8 type, u8 target section, u32 addend.
    // The patch-site section is selected by R_DOLPHIN_SECTION markers; the
    // position accumulates deltas within it.
    u8 site_section = 0;
    u32 position = 0;
    bool have_section = false;
    for (size_t p = stream_offset; p + 8 <= size;)
    {
      const u16 delta = ReadU16(data, p);
      const u8 type = data[p + 2];
      const u8 section = data[p + 3];
      const u32 addend = ReadU32(data, p + 4);
      p += 8;

      if (type == R_DOLPHIN_END)
        break;
      if (type == R_DOLPHIN_SECTION)
      {
        site_section = section;
        position = 0;
        have_section = true;
        continue;
      }
      position += delta;
      if (type == R_DOLPHIN_NOP || type == R_DOLPHIN_MRKREF || type == R_PPC_NONE)
        continue;

      if (!have_section || site_section >= num_sections)
      {
        fmt::println(std::cerr, "REL {}: relocation before section marker", name);
        return std::nullopt;
      }
      const auto& site_sec = rel.sections[site_section];
      if (position + 4 > site_sec.size || site_sec.IsBss())
      {
        fmt::println(std::cerr,
                     "REL {}: relocation site out of bounds (section {} offset {:#x})", name,
                     site_section, position);
        return std::nullopt;
      }

      rel.relocs.push_back({site_section, position, type, target_module, section, addend});
    }
  }

  return rel;
}
}  // namespace DolphinTool
