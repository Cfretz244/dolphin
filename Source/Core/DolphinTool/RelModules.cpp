// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinTool/RelModules.h"

#include <cctype>
#include <cstring>
#include <iostream>
#include <set>
#include <string>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

#include "DolphinTool/RarcReader.h"
#include "DolphinTool/Yaz0.h"

namespace DolphinTool
{
namespace
{
bool HasExtension(const std::string& name, const char* ext)
{
  const size_t ext_len = std::strlen(ext);
  if (name.size() < ext_len)
    return false;
  for (size_t i = 0; i < ext_len; i++)
  {
    if (std::tolower(name[name.size() - ext_len + i]) != ext[i])
      return false;
  }
  return true;
}

// Decompresses if Yaz0, otherwise returns the input unchanged.
std::vector<u8> MaybeYaz0(std::vector<u8> data, const std::string& name)
{
  if (!IsYaz0(data.data(), data.size()))
    return data;
  auto decompressed = Yaz0Decompress(data.data(), data.size());
  if (!decompressed)
  {
    fmt::println(std::cerr, "Warning: {} has Yaz0 magic but failed to decompress", name);
    return {};
  }
  return std::move(*decompressed);
}

struct DiscoveryState
{
  const DiscIO::Volume* volume;
  std::vector<RelFile> modules;
  std::set<u32> seen_ids;
  u32 parse_failures = 0;
  bool verbose;
};

void AddModule(DiscoveryState& state, const u8* data, size_t size, const std::string& name)
{
  auto rel = RelFile::Parse(data, size, name);
  if (!rel)
  {
    state.parse_failures++;
    return;
  }
  if (!state.seen_ids.insert(rel->module_id).second)
  {
    fmt::println(std::cerr, "Warning: duplicate module id {} ({}), keeping first occurrence",
                 rel->module_id, name);
    return;
  }
  if (state.verbose)
  {
    fmt::println(std::cerr, "  module {:3d}: {} ({} sections, {} relocs)", rel->module_id, name,
                 rel->sections.size(), rel->relocs.size());
  }
  state.modules.push_back(std::move(*rel));
}

void ScanArchive(DiscoveryState& state, const std::vector<u8>& arc, const std::string& arc_name)
{
  if (!IsRarc(arc.data(), arc.size()))
    return;
  for (const auto& member : RarcListFiles(arc.data(), arc.size()))
  {
    if (!HasExtension(member.name, ".rel"))
      continue;
    std::vector<u8> data(arc.begin() + member.data_offset,
                         arc.begin() + member.data_offset + member.size);
    data = MaybeYaz0(std::move(data), arc_name + "/" + member.name);
    if (!data.empty())
      AddModule(state, data.data(), data.size(), member.name);
  }
}

void ScanDirectory(DiscoveryState& state, const DiscIO::FileInfo& directory)
{
  for (const DiscIO::FileInfo& entry : directory)
  {
    if (entry.IsDirectory())
    {
      ScanDirectory(state, entry);
      continue;
    }
    const std::string name = entry.GetName();
    const bool is_rel = HasExtension(name, ".rel");
    const bool is_arc = HasExtension(name, ".arc") || HasExtension(name, ".szs");
    if (!is_rel && !is_arc)
      continue;

    std::vector<u8> data(entry.GetSize());
    const u64 read = DiscIO::ReadFile(*state.volume, DiscIO::PARTITION_NONE, &entry, data.data(),
                                      data.size());
    if (read != data.size())
    {
      fmt::println(std::cerr, "Warning: short read of {} ({}/{} bytes)", name, read, data.size());
      continue;
    }
    data = MaybeYaz0(std::move(data), name);
    if (data.empty())
      continue;

    if (is_rel)
      AddModule(state, data.data(), data.size(), name);
    else
      ScanArchive(state, data, name);
  }
}
}  // namespace

std::vector<RelFile> DiscoverRelModules(const DiscIO::Volume& volume, bool verbose)
{
  DiscoveryState state;
  state.volume = &volume;
  state.verbose = verbose;

  const DiscIO::FileSystem* fs = volume.GetFileSystem(DiscIO::PARTITION_NONE);
  if (!fs || !fs->IsValid())
  {
    fmt::println(std::cerr, "Warning: disc has no readable filesystem; no REL modules scanned");
    return {};
  }

  ScanDirectory(state, fs->GetRoot());

  if (state.parse_failures > 0)
    fmt::println(std::cerr, "Warning: {} REL files failed to parse", state.parse_failures);
  return std::move(state.modules);
}
}  // namespace DolphinTool
