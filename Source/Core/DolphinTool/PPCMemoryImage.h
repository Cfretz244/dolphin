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
    const Section* sec = FindSection(addr);
    if (!sec)
      return std::nullopt;
    u32 offset = addr - sec->base;
    if (offset + 4 > sec->size)
      return std::nullopt;
    u32 raw;
    std::memcpy(&raw, sec->data + offset, sizeof(u32));
    return Common::swap32(raw);
  }

  bool IsCodeAddress(u32 addr) const { return FindSection(addr) != nullptr; }

private:
  struct Section
  {
    u32 base;
    u32 size;
    const u8* data;
  };

  // Sections are sorted by base and non-overlapping (overlay images can have hundreds
  // of ranges, so lookups binary-search instead of scanning).
  const Section* FindSection(u32 addr) const
  {
    auto it = std::upper_bound(m_sections.begin(), m_sections.end(), addr,
                               [](u32 a, const Section& s) { return a < s.base; });
    if (it == m_sections.begin())
      return nullptr;
    --it;
    return (addr >= it->base && addr < it->base + it->size) ? &*it : nullptr;
  }

  std::vector<Section> m_sections;
};
