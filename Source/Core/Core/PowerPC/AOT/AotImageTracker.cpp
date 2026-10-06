// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT/AotImageTracker.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

extern "C" {
AotActiveImage aot_active_image = {0, 0, nullptr};
AotActiveImage aot_active_image_aux = {0, 0, nullptr};
AotActiveImage aot_active_image_real = {0, 0, nullptr};
static uint64_t s_idle_generation = 0;
const uint64_t* aot_images_generation = &s_idle_generation;
uint64_t aot_images_seen = 0;
}

namespace AotImageTracker
{
namespace
{
const AotImageDesc* s_images = nullptr;
u32 s_image_count = 0;
const AotImageDesc* s_active = nullptr;
const AotImageDesc* s_active_aux = nullptr;
const AotImageDesc* s_active_real = nullptr;
u32 s_active_mask = 0;
bool s_active_mask_valid = true;
u64 s_forced_generation = 0;  // added to the cache counter by MarkDirty()

// MEM1 host pointer for a 4-byte aligned effective address, or nullptr.
const u8* HostWord(const u8* ram, u32 ram_size, u32 addr)
{
  const u32 off = (addr & ~0x40000000u) - 0x80000000u;
  if ((addr & 3) || off >= ram_size || ram_size - off < 4)
    return nullptr;
  return ram + off;
}

u32 ReadWord(const u8* ram, u32 ram_size, u32 addr, bool* ok)
{
  const u8* p = HostWord(ram, ram_size, addr);
  if (!p)
  {
    *ok = false;
    return 0;
  }
  u32 v;
  std::memcpy(&v, p, 4);
  return Common::swap32(v);
}

bool ImageMatches(const AotImageDesc& image, const u8* ram, u32 ram_size)
{
  for (u32 i = 0; i < image.word_count; i++)
  {
    bool ok = true;
    if (ReadWord(ram, ram_size, image.words[i].addr, &ok) != image.words[i].word || !ok)
      return false;
  }
  return image.word_count > 0;
}

// Returns the patch mask read from RAM; *valid is cleared when a site holds
// neither its original nor its replacement word, or the mask is unsupported.
u32 ReadPatchMask(const AotImageDesc& image, const u8* ram, u32 ram_size, bool* valid)
{
  u32 mask = 0;
  *valid = true;
  for (u32 i = 0; i < image.patch_count; i++)
  {
    bool ok = true;
    const u32 word = ReadWord(ram, ram_size, image.patches[i].addr, &ok);
    if (!ok)
    {
      *valid = false;
      return 0;
    }
    if (word == image.patches[i].replacement)
      mask |= 1u << i;
    else if (word != image.patches[i].original)
      *valid = false;
  }
  if (mask != 0 && *valid)
  {
    bool supported = false;
    for (u32 k = 0; k < image.mask_count; k++)
      supported |= image.masks[k] == mask;
    if (!supported)
      *valid = false;
  }
  return mask;
}

void ApplyOverrides(const AotImageDesc& image, u32 mask, bool valid)
{
  s32 slot = -1;
  if (valid && mask != 0)
  {
    for (u32 k = 0; k < image.mask_count; k++)
      if (image.masks[k] == mask)
        slot = static_cast<s32>(k);
  }
  for (u32 i = 0; i < image.override_count; i++)
  {
    const AotImageOverride& ov = image.overrides[i];
    const u32 idx = (ov.pc - image.base) >> 2;
    if (idx >= image.size)
      continue;
    AOTBlockFunc fn = nullptr;
    if (!valid)
      fn = nullptr;  // conflicting/unknown patch words: interpret these pcs
    else if (mask == 0)
      fn = ov.base;
    else
      fn = ov.by_mask[slot];
    image.table[idx] = fn;
  }
}

// ---------------------------------------------------------------------------
// RSO modules (aot_images.h): miss-triggered identification.
//
// The games keep no usable module list (the SDK header's next/prev links are
// 0), so a module is found when dispatch misses one of its pcs: scan backwards
// from the pc for a located header (word[3] == base + 0x58, 10..40 sections),
// match the build-path basename its name pointer designates against the
// compiled descriptors, then verify every section size and the descriptor's
// identity words (unrelocated text words + base-independent external
// relocation sites) before writing the module's base slots and activating its
// table. Active modules are re-verified after every invalidation event (a
// module's heap buffer can be reused; the game icbi's new code).
// ---------------------------------------------------------------------------
struct ActiveRso
{
  u32 text_base;
  u32 text_size;
  u32 module_base;
  const AotRsoModuleDesc* desc;
};
const AotRsoModuleDesc* s_rso = nullptr;
u32 s_rso_count = 0;
u32 s_rso_max_span = 0;  // largest (header + section table + text) of any module
std::vector<ActiveRso> s_rso_active;
std::unordered_set<u32> s_rso_bad_pages;  // 4 KB pages that identified nothing
u64 s_rso_seen = 0;
u32 s_rso_scans = 0;  // scans since the last invalidation event (capped)
constexpr u32 kMaxRsoScansPerGeneration = 16;
// Lifetime counters (stats dumps).
u64 s_rso_stat_scans = 0, s_rso_stat_scan_words = 0, s_rso_stat_identified = 0,
    s_rso_stat_dropped = 0, s_rso_stat_revalidations = 0, s_rso_stat_capped = 0;

// Guest RAM as seen by the scans; fetched once per lookup slow path.
struct MemView
{
  const u8* ram = nullptr;
  const u8* exram = nullptr;
  u32 ram_size = 0;
  u32 exram_size = 0;

  MemView()
  {
    auto& memory = Core::System::GetInstance().GetMemory();
    ram = memory.GetRAM();
    exram = memory.GetEXRAM();
    ram_size = ram ? memory.GetRamSizeReal() : 0;
    exram_size = exram ? memory.GetExRamSizeReal() : 0;
  }

  // MEM1/MEM2 host pointer for [addr, addr + size), or nullptr.
  const u8* Ptr(u32 addr, u32 size) const
  {
    const u32 off1 = (addr & ~0x40000000u) - 0x80000000u;
    if (off1 < ram_size && ram_size - off1 >= size)
      return ram + off1;
    const u32 off2 = off1 - 0x10000000u;
    if (off2 < exram_size && exram_size - off2 >= size)
      return exram + off2;
    return nullptr;
  }

  bool Rd32(u32 addr, u32* out) const
  {
    const u8* p = Ptr(addr, 4);
    if (!p)
      return false;
    u32 v;
    std::memcpy(&v, p, 4);
    *out = Common::swap32(v);
    return true;
  }

  // Basename of the NUL-terminated build path at name_ptr (empty if invalid).
  std::string_view Basename(u32 name_ptr) const
  {
    const u8* p = Ptr(name_ptr, 1);
    if (!p)
      return {};
    // Within one memory region: Ptr(name_ptr, n) succeeding implies contiguity.
    u32 n = 0;
    while (n < 256 && Ptr(name_ptr, n + 1) && p[n] != 0)
      ++n;
    if (n == 0 || n == 256 || !Ptr(name_ptr, n + 1))
      return {};
    const std::string_view path(reinterpret_cast<const char*>(p), n);
    const size_t slash = path.find_last_of("\\/");
    return path.substr(slash == std::string_view::npos ? 0 : slash + 1);
  }
};

// Header signature of a located module at `base`.
bool LooksLikeHeader(const MemView& mem, u32 base, u32* nsec)
{
  u32 w3 = 0;
  return mem.Rd32(base + 12, &w3) && w3 == base + 0x58 && mem.Rd32(base + 8, nsec) &&
         *nsec >= 10 && *nsec <= 40;
}

// Full check of `desc` located at `base`; on success returns the text base.
bool VerifyRso(const MemView& mem, const AotRsoModuleDesc& desc, u32 base, u32* text_base)
{
  u32 nsec = 0, table = 0, name = 0;
  if (!mem.Rd32(base + 8, &nsec) || nsec != desc.num_sections || !mem.Rd32(base + 12, &table) ||
      table != base + 0x58 || !mem.Rd32(base + 0x10, &name) ||
      mem.Basename(name) != std::string_view(desc.name))
    return false;
  for (u32 i = 0; i < nsec; i++)
  {
    u32 size = 0;
    if (!mem.Rd32(table + i * 8 + 4, &size) || size != desc.section_sizes[i])
      return false;
  }
  u32 text = 0;
  if (!mem.Rd32(table + desc.text_section * 8, &text))
    return false;
  text &= ~1u;  // executable flag
  if ((text & 3) || !mem.Ptr(text, desc.text_size))
    return false;
  for (u32 i = 0; i < desc.word_count; i++)
  {
    u32 w = 0;
    if (!mem.Rd32(base + desc.words[i].offset, &w) || w != desc.words[i].word)
      return false;
  }
  *text_base = text;
  return true;
}

void ActivateRso(const MemView& mem, const AotRsoModuleDesc& desc, u32 base, u32 text_base)
{
  for (u32 i = 0; i < desc.num_sections; i++)
  {
    u32 v = 0;
    mem.Rd32(base + 0x58 + i * 8, &v);
    desc.base_slots[i] = v & ~1u;  // located address (bit 0: executable flag)
  }
  s_rso_active.push_back({text_base, desc.text_size, base, &desc});
  std::sort(s_rso_active.begin(), s_rso_active.end(),
            [](const ActiveRso& a, const ActiveRso& b) { return a.text_base < b.text_base; });
  ++s_rso_stat_identified;
  INFO_LOG_FMT(AOT, "AotRsoTracker: {} active at {:#010x} (text {:#010x}+{:#x})", desc.name, base,
               text_base, desc.text_size);
}

void DeactivateRso(const ActiveRso& r)
{
  for (u32 i = 0; i < r.desc->num_sections; i++)
    r.desc->base_slots[i] = 0;
  ++s_rso_stat_dropped;
  INFO_LOG_FMT(AOT, "AotRsoTracker: {} at {:#010x} gone", r.desc->name, r.module_base);
}

// Re-verify the active set after an invalidation event; forget bad pages.
void RevalidateRso()
{
  s_rso_seen = *aot_images_generation;
  s_rso_bad_pages.clear();
  s_rso_scans = 0;
  if (s_rso_active.empty())
    return;
  ++s_rso_stat_revalidations;
  const MemView mem;
  std::erase_if(s_rso_active, [&](const ActiveRso& r) {
    u32 text = 0;
    if (VerifyRso(mem, *r.desc, r.module_base, &text) && text == r.text_base)
      return false;
    DeactivateRso(r);
    return true;
  });
}

const ActiveRso* FindRso(u32 pc)
{
  auto it = std::upper_bound(s_rso_active.begin(), s_rso_active.end(), pc,
                             [](u32 v, const ActiveRso& r) { return v < r.text_base; });
  if (it == s_rso_active.begin())
    return nullptr;
  --it;
  return pc - it->text_base < it->text_size ? &*it : nullptr;
}

// Scan backwards from pc for a located RSO header whose module covers pc.
const ActiveRso* IdentifyRso(u32 pc)
{
  const u32 page = pc >> 12;
  // Not for pcs inside the active DOL/auxiliary image ranges (untranslated DOL
  // code is not a module); a bounded number of scans between invalidation events.
  if ((pc - aot_active_image.base) >> 2 < aot_active_image.size ||
      (pc - aot_active_image_aux.base) >> 2 < aot_active_image_aux.size ||
      s_rso_bad_pages.contains(page))
    return nullptr;
  if (s_rso_scans >= kMaxRsoScansPerGeneration)
  {
    ++s_rso_stat_capped;
    return nullptr;
  }
  const MemView mem;
  if (!mem.Ptr(pc, 4))
  {
    s_rso_bad_pages.insert(page);
    return nullptr;
  }
  ++s_rso_scans;
  ++s_rso_stat_scans;
  const u32 lo = pc > s_rso_max_span ? pc - s_rso_max_span : 0;
  for (u32 a = pc & ~3u; a >= lo && a >= 4; a -= 4)
  {
    ++s_rso_stat_scan_words;
    u32 nsec = 0;
    if (!LooksLikeHeader(mem, a, &nsec))
      continue;
    for (u32 m = 0; m < s_rso_count; m++)
    {
      u32 text = 0;
      if (s_rso[m].num_sections == nsec && VerifyRso(mem, s_rso[m], a, &text) &&
          pc - text < s_rso[m].text_size)
      {
        // A module can only be resident once; drop a stale entry for it.
        std::erase_if(s_rso_active, [&](const ActiveRso& r) {
          if (r.desc != &s_rso[m])
            return false;
          DeactivateRso(r);
          return true;
        });
        ActivateRso(mem, s_rso[m], a, text);
        return FindRso(pc);
      }
    }
    // A located, named module that is not ours (or failed verification) and
    // whose sections cover pc owns this pc: no point scanning further back.
    u32 name = 0;
    if (!mem.Rd32(a + 0x10, &name) || mem.Basename(name).empty())
      continue;
    bool covers = false;
    for (u32 i = 0; i < nsec && !covers; i++)
    {
      u32 off = 0, size = 0;
      if (mem.Rd32(a + 0x58 + i * 8, &off) && mem.Rd32(a + 0x58 + i * 8 + 4, &size))
        covers = off && pc - (off & ~1u) < size;
    }
    if (covers)
      break;
  }
  s_rso_bad_pages.insert(page);
  return nullptr;
}

const ActiveRso* LookupRso(u32 pc)
{
  if (!s_rso_count)
    return nullptr;
  if (*aot_images_generation != s_rso_seen)
    RevalidateRso();
  if (const ActiveRso* r = FindRso(pc))
    return r;
  return IdentifyRso(pc);
}

void Rescan()
{
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  const u8* ram = memory.GetRAM();
  const u32 ram_size = ram ? memory.GetRamSizeReal() : 0;

  const AotImageDesc* selected = nullptr;
  const AotImageDesc* aux = nullptr;
  const AotImageDesc* real = nullptr;
  for (u32 i = 0; i < s_image_count && ram; i++)
  {
    const AotImageDesc*& slot = (s_images[i].flags & AOT_IMAGE_REAL_MODE) ? real :
                                (s_images[i].flags & AOT_IMAGE_AUXILIARY) ? aux :
                                                                            selected;
    if (slot != nullptr)
      continue;
    if (ImageMatches(s_images[i], ram, ram_size))
      slot = &s_images[i];
  }
  if (real != s_active_real)
  {
    INFO_LOG_FMT(AOT, "AotImageTracker: real-mode image {}", real ? real->name : "none");
    s_active_real = real;
  }
  if (real)
    aot_active_image_real = {real->base, real->size, real->table};
  else
    aot_active_image_real = {0, 0, nullptr};
  if (aux != s_active_aux)
  {
    INFO_LOG_FMT(AOT, "AotImageTracker: auxiliary image {}", aux ? aux->name : "none");
    s_active_aux = aux;
  }
  if (aux)
    aot_active_image_aux = {aux->base, aux->size, aux->table};
  else
    aot_active_image_aux = {0, 0, nullptr};

  u32 mask = 0;
  bool valid = true;
  if (selected && selected->patch_count > 0)
    mask = ReadPatchMask(*selected, ram, ram_size, &valid);

  if (selected != s_active || mask != s_active_mask || valid != s_active_mask_valid)
  {
    if (selected)
    {
      ApplyOverrides(*selected, mask, valid);
      INFO_LOG_FMT(AOT, "AotImageTracker: active image '{}' ({:#010x}, {} entries), patch mask "
                        "{:#x}{}",
                   selected->name, selected->base, selected->size, mask,
                   valid ? "" : " (INVALID patch words -- patched pcs interpreted)");
    }
    else
    {
      INFO_LOG_FMT(AOT, "AotImageTracker: no image matches RAM -- interpreter until the next "
                        "invalidation event");
    }
    s_active = selected;
    s_active_mask = mask;
    s_active_mask_valid = valid;
  }

  if (selected)
    aot_active_image = {selected->base, selected->size, selected->table};
  else
    aot_active_image = {0, 0, nullptr};
}
}  // namespace

void Init(const AotImageDesc* images, u32 count, const AotRsoModuleDesc* rso_modules,
          u32 rso_count)
{
  s_images = images;
  s_image_count = count;
  s_rso = rso_modules;
  s_rso_count = rso_count;
  s_rso_max_span = 0;
  s_rso_active.clear();
  s_rso_bad_pages.clear();
  for (u32 m = 0; m < rso_count; m++)
  {
    // .text starts right after the header and section table (file offset
    // 0x100-0x130 in every Trilogy module; at most 0x58 + 40 * 8 = 0x198 plus
    // section alignment), so a pc is at most this far above its module base.
    s_rso_max_span = std::max(s_rso_max_span, rso_modules[m].text_size + 0x400u);
    for (u32 i = 0; i < rso_modules[m].num_sections; i++)
      rso_modules[m].base_slots[i] = 0;
  }
  s_active = nullptr;
  s_active_aux = nullptr;
  s_active_real = nullptr;
  s_active_mask = 0;
  s_active_mask_valid = true;
  aot_active_image = {0, 0, nullptr};
  aot_active_image_aux = {0, 0, nullptr};
  aot_active_image_real = {0, 0, nullptr};
  if (count == 0)
  {
    aot_images_generation = &s_idle_generation;
    aot_images_seen = s_idle_generation;
    return;
  }
  auto& ppc_state = Core::System::GetInstance().GetPPCState();
  aot_images_generation = &ppc_state.iCache.invalidation_generation;
  // Force the first dispatch to rescan.
  aot_images_seen = ppc_state.iCache.invalidation_generation - 1;
  INFO_LOG_FMT(AOT, "AotImageTracker: {} trusted images, {} RSO modules registered", count,
               rso_count);
  s_rso_seen = aot_images_seen;
  for (u32 i = 0; i < count; i++)
  {
    INFO_LOG_FMT(AOT, "  image {}: '{}' {:#010x}+{:#x} words, {} discriminators, {} patch sites, "
                      "{} overrides{}",
                 i, images[i].name, images[i].base, images[i].size * 4, images[i].word_count,
                 images[i].patch_count, images[i].override_count,
                 (images[i].flags & AOT_IMAGE_REAL_MODE) ? " (real mode)" :
                 (images[i].flags & AOT_IMAGE_AUXILIARY) ? " (auxiliary)" :
                                                           "");
  }
}

void Shutdown()
{
  for (const auto& r : s_rso_active)
    for (u32 i = 0; i < r.desc->num_sections; i++)
      r.desc->base_slots[i] = 0;
  s_rso_active.clear();
  s_rso = nullptr;
  s_rso_count = 0;
  s_images = nullptr;
  s_image_count = 0;
  s_active = nullptr;
  s_active_aux = nullptr;
  s_active_real = nullptr;
  aot_active_image = {0, 0, nullptr};
  aot_active_image_aux = {0, 0, nullptr};
  aot_active_image_real = {0, 0, nullptr};
  aot_images_generation = &s_idle_generation;
  aot_images_seen = s_idle_generation;
}

std::string RsoStatsLine()
{
  if (!s_rso_count)
    return {};
  std::string active;
  for (const auto& r : s_rso_active)
    active += fmt::format(" {}@{:#010x}", r.desc->name, r.text_base);
  return fmt::format("RSO modules: {} registered, identified {}, dropped {}, scans {} ({} words), "
                     "capped {}, revalidations {}; active:{}",
                     s_rso_count, s_rso_stat_identified, s_rso_stat_dropped, s_rso_stat_scans,
                     s_rso_stat_scan_words, s_rso_stat_capped, s_rso_stat_revalidations,
                     active.empty() ? " none" : active);
}

std::string DescribeRsoPc(u32 pc)
{
  const MemView mem;
  if (!mem.Ptr(pc, 4))
    return {};
  const u32 lo = pc > 0x80000u ? pc - 0x80000u : 0;
  for (u32 a = pc & ~3u; a >= lo && a >= 4; a -= 4)
  {
    u32 nsec = 0, name = 0;
    if (!LooksLikeHeader(mem, a, &nsec) || !mem.Rd32(a + 0x10, &name))
      continue;
    const std::string_view base = mem.Basename(name);
    if (base.empty())
      continue;
    for (u32 i = 0; i < nsec; i++)
    {
      u32 off = 0, size = 0;
      if (!mem.Rd32(a + 0x58 + i * 8, &off) || !mem.Rd32(a + 0x58 + i * 8 + 4, &size) || !off ||
          pc - (off & ~1u) >= size)
        continue;
      const bool translated = FindRso(pc) != nullptr;
      return fmt::format("{} s{}+{:#x}{}", base, i, pc - (off & ~1u), translated ? " [AOT]" : "");
    }
  }
  return {};
}

void MarkDirty()
{
  if (s_image_count == 0)
    return;
  // The counter is host-only and monotonic; bumping it is how every other
  // invalidation source signals us, so do the same rather than adding state.
  ++Core::System::GetInstance().GetPPCState().iCache.invalidation_generation;
}
}  // namespace AotImageTracker

extern "C" void aot_images_rescan(void)
{
  aot_images_seen = *aot_images_generation;
  AotImageTracker::Rescan();
}

// Real-mode images only serve instruction fetches with MSR.IR clear (their
// table is indexed by physical pc); the generated dispatch makes the same test.
static bool InstructionRelocationOff()
{
  return !Core::System::GetInstance().GetPPCState().msr.IR;
}

extern "C" AOTBlockFunc aot_images_lookup(uint32_t pc)
{
  if (*aot_images_generation != aot_images_seen)
    aot_images_rescan();
  if (aot_active_image_real.size && InstructionRelocationOff())
  {
    const u32 real_idx = (pc - aot_active_image_real.base) >> 2;
    return real_idx < aot_active_image_real.size ? aot_active_image_real.table[real_idx] :
                                                   nullptr;
  }
  const u32 idx = (pc - aot_active_image.base) >> 2;
  if (idx < aot_active_image.size)
    return aot_active_image.table[idx];
  const u32 aux_idx = (pc - aot_active_image_aux.base) >> 2;
  if (aux_idx < aot_active_image_aux.size)
    return aot_active_image_aux.table[aux_idx];
  if (!InstructionRelocationOff())
  {
    if (const auto* r = AotImageTracker::LookupRso(pc))
      return r->desc->table[(pc - r->text_base) >> 2];
  }
  return nullptr;
}

extern "C" uint32_t aot_images_block_size(uint32_t pc)
{
  using namespace AotImageTracker;
  if (*aot_images_generation != aot_images_seen)
    aot_images_rescan();
  const AotImageDesc* image = s_active;
  if (s_active_real && InstructionRelocationOff())
    image = s_active_real;
  else if (image && (pc - image->base) >> 2 >= image->size)
    image = s_active_aux;
  if (!image || ((pc - image->base) >> 2 >= image->size && !(image->flags & AOT_IMAGE_REAL_MODE)))
  {
    if (InstructionRelocationOff())
      return 0;
    const auto* r = AotImageTracker::LookupRso(pc);
    if (!r)
      return 0;
    const u32 off = pc - r->text_base;
    const AotImageBlockSize* begin = r->desc->block_sizes;
    const AotImageBlockSize* end = begin + r->desc->block_size_count;
    const auto it = std::lower_bound(
        begin, end, off, [](const AotImageBlockSize& b, u32 v) { return b.addr < v; });
    return it != end && it->addr == off ? it->num_instructions : 0;
  }
  // Overrides first: the active variant's boundary can differ from the base.
  for (u32 i = 0; i < image->override_count; i++)
  {
    const AotImageOverride& ov = image->overrides[i];
    if (ov.pc != pc)
      continue;
    if (!s_active_mask_valid)
      return 0;
    if (s_active_mask == 0)
      return ov.base ? ov.base_size : 0;
    for (u32 k = 0; k < image->mask_count; k++)
      if (image->masks[k] == s_active_mask)
        return ov.by_mask[k] ? ov.size_by_mask[k] : 0;
    return 0;
  }
  const AotImageBlockSize* begin = image->block_sizes;
  const AotImageBlockSize* end = begin + image->block_size_count;
  const auto it = std::lower_bound(begin, end, pc,
                                   [](const AotImageBlockSize& b, u32 v) { return b.addr < v; });
  if (it != end && it->addr == pc)
    return it->num_instructions;
  return 0;
}

extern "C" void aot_rso_dispatch(AOTState* s)
{
  // Same entry guard as <ID>_dispatch (per-site probes can reach here).
  if (s->downcount <= 0)
    return;
  if (s->msr & 0x20u)  // RSO code only runs with instruction relocation on
  {
    if (const auto* r = AotImageTracker::LookupRso(s->pc))
    {
      if (const AOTBlockFunc fn = r->desc->table[(s->pc - r->text_base) >> 2])
        [[clang::musttail]] return fn(s);
    }
  }
  [[clang::musttail]] return aot_interpreter_single_step(s);
}
