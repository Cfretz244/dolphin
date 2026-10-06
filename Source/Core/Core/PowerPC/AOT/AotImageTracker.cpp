// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT/AotImageTracker.h"

#include <algorithm>
#include <cstring>

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

void Init(const AotImageDesc* images, u32 count)
{
  s_images = images;
  s_image_count = count;
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
  INFO_LOG_FMT(AOT, "AotImageTracker: {} trusted images registered", count);
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
  if (!image)
    return 0;
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
