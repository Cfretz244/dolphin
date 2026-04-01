// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"

// Maps DOL text sections into a virtual address space for PPC instruction reads.
class PPCMemoryImage
{
public:
  void AddSection(u32 virtual_addr, const u8* data, u32 size)
  {
    if (size == 0 || data == nullptr)
      return;
    m_sections.push_back({virtual_addr, size, data});
    std::sort(m_sections.begin(), m_sections.end(),
              [](const Section& a, const Section& b) { return a.base < b.base; });
  }

  std::optional<u32> ReadInstruction(u32 addr) const
  {
    for (const auto& sec : m_sections)
    {
      if (addr >= sec.base && addr < sec.base + sec.size)
      {
        u32 offset = addr - sec.base;
        if (offset + 4 > sec.size)
          return std::nullopt;
        u32 raw;
        std::memcpy(&raw, sec.data + offset, sizeof(u32));
        return Common::swap32(raw);
      }
    }
    return std::nullopt;
  }

  bool IsCodeAddress(u32 addr) const
  {
    for (const auto& sec : m_sections)
    {
      if (addr >= sec.base && addr < sec.base + sec.size)
        return true;
    }
    return false;
  }

private:
  struct Section
  {
    u32 base;
    u32 size;
    const u8* data;
  };
  std::vector<Section> m_sections;
};
