// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/PowerPC/AOT/aot_images.h"

// Runtime selector for trusted multi-image AOT libraries (Trilogy: launcher
// and per-game DOLs sharing one address range, plus PrimeHack patch variants).
//
// The active image is re-evaluated lazily: the generated dispatch compares the
// emulated instruction cache's invalidation counter against the value seen at
// the last rescan and calls aot_images_rescan() on a change. A rescan compares
// every image's discriminator words against RAM, publishes the first full
// match in aot_active_image, and rewrites that image's override table entries
// for the current patch mask. Native code of an image only ever runs while it
// is active (dispatch and every indirect probe funnel through the table), so
// no per-block content checks are needed — the same trust model as the JIT
// and as every single-image game in the stack.
namespace AotImageTracker
{
void Init(const AotImageDesc* images, u32 count);
void Shutdown();
// Force a rescan at the next dispatch (savestate load, core reset).
void MarkDirty();
}  // namespace AotImageTracker
