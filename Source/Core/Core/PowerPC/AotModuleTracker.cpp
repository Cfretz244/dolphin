// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AotModuleTracker.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/AotRegistry.h"
#include "Core/System.h"

extern "C" void aot_interpreter_single_step(AOTState* s);

namespace AotModuleTracker
{
namespace
{
// OSModuleQueue head in low memory (standard SDK location). Overridable for
// games that deviate: AOT_MODULE_QUEUE_ADDR=0x........
constexpr u32 DEFAULT_QUEUE_HEAD_ADDR = 0x800030C8;

// OSModuleInfo layout (matches the on-disc REL header, link fields live):
//   +0x00 u32 id, +0x04 u32 next, +0x08 u32 prev,
//   +0x0C u32 numSections, +0x10 u32 sectionInfoOffset
// Post-OSLink the section table holds absolute addresses (flags in bit 0).

struct ActiveRange
{
  u32 base;
  u32 size;
  const AOTBlockFunc* table;
};

const AotModuleDesc* s_modules = nullptr;
u32 s_module_count = 0;
std::unordered_map<u32, const AotModuleDesc*> s_by_id;
std::vector<ActiveRange> s_ranges;  // sorted by base
const ActiveRange* s_last_range = nullptr;
volatile int s_dirty = 0;
u32 s_queue_head_addr = DEFAULT_QUEUE_HEAD_ADDR;
std::unordered_set<u32> s_warned_ids;
u32 s_last_active_count = 0;

void ZeroAllSlots()
{
  for (u32 m = 0; m < s_module_count; m++)
  {
    const AotModuleDesc& desc = s_modules[m];
    for (u32 i = 0; i < desc.num_sections; i++)
      *desc.sections[i].base_slot = 0;
  }
}

void RescanModules()
{
  s_ranges.clear();
  s_last_range = nullptr;
  if (!s_modules)
    return;
  ZeroAllSlots();

  auto& memory = Core::System::GetInstance().GetMemory();
  const u8* ram = memory.GetRAM();
  const u32 ram_size = memory.GetRamSizeReal();
  if (!ram)
    return;

  const auto rd32 = [&](u32 addr) -> u32 {
    const u32 offset = addr & 0x3FFFFFFFu;
    if (offset + 4 > ram_size)
      return 0;
    u32 v;
    std::memcpy(&v, ram + offset, 4);
    return Common::swap32(v);
  };
  const auto in_ram = [&](u32 addr) {
    return ((addr & ~0x40000000u) - 0x80000000u) < ram_size;
  };

  u32 active = 0;
  u32 cur = rd32(s_queue_head_addr);
  for (u32 guard = 0; guard < 1024 && in_ram(cur); guard++)
  {
    const u32 id = rd32(cur + 0x00);
    const u32 next = rd32(cur + 0x04);
    const u32 num_sections = rd32(cur + 0x0C);
    const u32 section_info = rd32(cur + 0x10);

    // Bisection aids: AOT_NO_MODULES=1 disables all table activation;
    // AOT_ONLY_MODULE=<id> activates a single module's tables.
    static const bool no_modules = std::getenv("AOT_NO_MODULES") != nullptr;
    static const char* only_env = std::getenv("AOT_ONLY_MODULE");
    static const u32 only_id = only_env ? static_cast<u32>(std::strtoul(only_env, nullptr, 0)) : 0;
    const auto it =
        (no_modules || (only_env && id != only_id)) ? s_by_id.end() : s_by_id.find(id);
    if (it != s_by_id.end())
    {
      const AotModuleDesc& desc = *it->second;
      bool ok = desc.num_sections == num_sections && in_ram(section_info);
      if (ok)
      {
        // Validate section sizes against the compiled descriptor before
        // activating — a mismatch means a different module build than the
        // pinned ISO, and interpreter fallback is the safe answer.
        for (u32 i = 0; i < num_sections && ok; i++)
        {
          const u32 size = rd32(section_info + i * 8 + 4);
          if (size != desc.sections[i].size)
            ok = false;
        }
      }
      if (ok)
      {
        static const bool debug_dump = std::getenv("AOT_MODULE_DEBUG") != nullptr;
        for (u32 i = 0; i < num_sections; i++)
        {
          const u32 raw = rd32(section_info + i * 8);
          const u32 base = raw & ~3u;
          *desc.sections[i].base_slot = base;
          if (desc.sections[i].table && base != 0 && in_ram(base))
            s_ranges.push_back({base, desc.sections[i].size, desc.sections[i].table});
          if (debug_dump && desc.sections[i].size > 0)
          {
            WARN_LOG_FMT(POWERPC, "  module {} section {}: base={:#010x} size={:#x} exec={}", id,
                         i, base, desc.sections[i].size, desc.sections[i].executable);
          }
        }
        active++;
      }
      else if (s_warned_ids.insert(id).second)
      {
        WARN_LOG_FMT(POWERPC,
                     "AotModuleTracker: module {} layout mismatch (sections {} vs {}), "
                     "leaving on interpreter",
                     id, num_sections, desc.num_sections);
      }
    }

    if (next == cur)
      break;
    cur = next;
  }

  std::sort(s_ranges.begin(), s_ranges.end(),
            [](const ActiveRange& a, const ActiveRange& b) { return a.base < b.base; });

  if (active != s_last_active_count)
  {
    INFO_LOG_FMT(POWERPC, "AotModuleTracker: {} modules active ({} table ranges)", active,
                 s_ranges.size());
    s_last_active_count = active;
  }
}

const ActiveRange* LookupRange(u32 pc)
{
  // Largest base <= pc, then range check.
  auto it = std::upper_bound(s_ranges.begin(), s_ranges.end(), pc,
                             [](u32 v, const ActiveRange& r) { return v < r.base; });
  if (it == s_ranges.begin())
    return nullptr;
  --it;
  return (pc - it->base < it->size) ? &*it : nullptr;
}
}  // namespace

void Init(const AotModuleDesc* modules, u32 count)
{
  s_modules = modules;
  s_module_count = count;
  s_by_id.clear();
  for (u32 m = 0; m < count; m++)
    s_by_id[modules[m].module_id] = &modules[m];
  s_ranges.clear();
  s_last_range = nullptr;
  s_warned_ids.clear();
  s_last_active_count = 0;
  s_dirty = 1;

  if (const char* env = std::getenv("AOT_MODULE_QUEUE_ADDR"))
    s_queue_head_addr = static_cast<u32>(std::strtoul(env, nullptr, 0));

  if (count > 0)
  {
    INFO_LOG_FMT(POWERPC, "AotModuleTracker: {} compiled modules, queue head {:#010x}", count,
                 s_queue_head_addr);
  }
}

void Shutdown()
{
  if (s_modules)
    ZeroAllSlots();
  s_modules = nullptr;
  s_module_count = 0;
  s_by_id.clear();
  s_ranges.clear();
  s_last_range = nullptr;
}

void MarkDirty()
{
  s_dirty = 1;
}
}  // namespace AotModuleTracker

extern "C" void aot_module_dispatch(AOTState* s)
{
  using namespace AotModuleTracker;
  if (s_dirty)
  {
    s_dirty = 0;
    RescanModules();
  }
  // pc is the first AOTState field; read it without needing the full layout.
  const u32 pc = *reinterpret_cast<const u32*>(s);
  const ActiveRange* r =
      (s_last_range && pc - s_last_range->base < s_last_range->size) ? s_last_range :
                                                                       LookupRange(pc);
  if (r)
  {
    const AOTBlockFunc fn = r->table[(pc - r->base) >> 2];
    if (fn)
    {
      s_last_range = r;
      [[clang::musttail]] return fn(s);
    }
  }
  [[clang::musttail]] return aot_interpreter_single_step(s);
}
