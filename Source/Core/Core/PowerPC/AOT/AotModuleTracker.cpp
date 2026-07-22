// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT/AotModuleTracker.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/AOT/AotRegistry.h"
#include "Core/PowerPC/AOT/AotState.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

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
  u32 module_id;
  u32 section;
};

const AotModuleDesc* s_modules = nullptr;
u32 s_module_count = 0;
std::unordered_map<u32, const AotModuleDesc*> s_by_id;
std::vector<ActiveRange> s_ranges;  // sorted by base
const ActiveRange* s_last_range = nullptr;
std::atomic<bool> s_dirty{false};
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
  // GetExRamSizeReal() reports the retail MEM2 size even on GC — only the
  // allocation is Wii-gated, so key off the pointer.
  const u8* exram = memory.GetEXRAM();
  const u32 exram_size = exram ? memory.GetExRamSizeReal() : 0;
  if (!ram)
    return;

  // Resolve an effective address to a host pointer with `size` readable
  // bytes: MEM1 (0x80000000/0xC0000000 mirrors) or Wii MEM2 (0x90000000/
  // 0xD0000000). Modules may be loaded into either region.
  const auto host = [&](u32 addr, u32 size) -> const u8* {
    const u32 off1 = (addr & ~0x40000000u) - 0x80000000u;
    if (off1 < ram_size && ram_size - off1 >= size)
      return ram + off1;
    const u32 off2 = off1 - 0x10000000u;
    if (off2 < exram_size && exram_size - off2 >= size)
      return exram + off2;
    return nullptr;
  };
  const auto rd32 = [&](u32 addr) -> u32 {
    const u8* p = host(addr, 4);
    if (!p)
      return 0;
    u32 v;
    std::memcpy(&v, p, 4);
    return Common::swap32(v);
  };
  const auto in_ram = [&](u32 addr) { return host(addr, 1) != nullptr; };

  u32 active = 0;
  u32 cur = rd32(s_queue_head_addr);
  for (u32 guard = 0; guard < 1024 && in_ram(cur); guard++)
  {
    const u32 id = rd32(cur + 0x00);
    const u32 next = rd32(cur + 0x04);
    const u32 num_sections = rd32(cur + 0x0C);
    const u32 section_info = rd32(cur + 0x10);

#ifdef DOLPHIN_AOT_HARNESS
    // Bisection aids: AOT_NO_MODULES=1 disables all table activation;
    // AOT_ONLY_MODULE=<id> activates a single module's tables.
    static const bool no_modules = std::getenv("AOT_NO_MODULES") != nullptr;
    static const char* only_env = std::getenv("AOT_ONLY_MODULE");
    static const u32 only_id = only_env ? static_cast<u32>(std::strtoul(only_env, nullptr, 0)) : 0;
    const auto it =
        (no_modules || (only_env && id != only_id)) ? s_by_id.end() : s_by_id.find(id);
#else
    const auto it = s_by_id.find(id);
#endif
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
#ifdef DOLPHIN_AOT_HARNESS
        static const bool debug_dump = std::getenv("AOT_MODULE_DEBUG") != nullptr;
#else
        constexpr bool debug_dump = false;
#endif
        for (u32 i = 0; i < num_sections; i++)
        {
          const u32 raw = rd32(section_info + i * 8);
          const u32 base = raw & ~3u;
          *desc.sections[i].base_slot = base;
          if (desc.sections[i].table && base != 0 && in_ram(base))
            s_ranges.push_back({base, desc.sections[i].size, desc.sections[i].table, id, i});
          if (debug_dump && desc.sections[i].size > 0)
          {
            WARN_LOG_FMT(AOT, "  module {} section {}: base={:#010x} size={:#x} exec={}", id,
                         i, base, desc.sections[i].size, desc.sections[i].executable);
          }
        }
        active++;
      }
      else if (s_warned_ids.insert(id).second)
      {
        WARN_LOG_FMT(AOT,
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
    INFO_LOG_FMT(AOT, "AotModuleTracker: {} modules active ({} table ranges)", active,
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
  s_dirty = true;

#ifdef DOLPHIN_AOT_HARNESS
  // Debug override for games whose OS module queue head deviates from the SDK
  // default; a game that needs this permanently should get a real config knob.
  if (const char* env = std::getenv("AOT_MODULE_QUEUE_ADDR"))
    s_queue_head_addr = static_cast<u32>(std::strtoul(env, nullptr, 0));
#endif

  if (count > 0)
  {
    INFO_LOG_FMT(AOT, "AotModuleTracker: {} compiled modules, queue head {:#010x}", count,
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
  s_dirty = true;
}

bool LookupBlock(u32 pc, AOTBlockFunc* fn, u32* module_id, u32* section, u32* offset)
{
  if (s_dirty.exchange(false))
    RescanModules();
  const ActiveRange* r = LookupRange(pc);
  if (!r)
    return false;
  *fn = r->table[(pc - r->base) >> 2];
  *module_id = r->module_id;
  *section = r->section;
  *offset = pc - r->base;
  return true;
}
}  // namespace AotModuleTracker

extern "C" void aot_module_dispatch(AOTState* s)
{
  using namespace AotModuleTracker;
  // Downcount guard mirroring <ID>_dispatch's entry check. Today every caller
  // sits behind that check already, but per-site indirect probes make this
  // entry point reachable on its own — it must be self-sufficient as the bound
  // on pure-indirect cycles. pc is always set before dispatch, so a plain
  // return resumes the Run loop.
  if (s->downcount <= 0)
    return;
  if (s_dirty.exchange(false))
    RescanModules();
  const u32 pc = s->pc;
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
