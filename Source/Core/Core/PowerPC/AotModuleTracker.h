// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

struct AOTState;
struct AotModuleDesc;

// Tracks which REL modules the game currently has loaded and at what
// addresses, by walking the OS module queue in emulated low memory whenever
// the icbi-driven dirty flag is set. Activates the per-module AOT dispatch
// tables (registered via aot_register_game_modules) by writing each section's
// runtime base address into the module's base-slot array; modules that vanish
// from the queue get their slots zeroed so generated code degrades to the
// interpreter.
namespace AotModuleTracker
{
// Called from AOTCore::Init with the registry's module descriptors for the
// running game (nullptr/0 for games without compiled modules).
void Init(const AotModuleDesc* modules, u32 count);
void Shutdown();

// Schedules a rescan of the OS module queue before the next module dispatch.
// Cheap enough to call from every aot_icbi.
void MarkDirty();
}  // namespace AotModuleTracker

// Module-aware terminal dispatch, musttail-called by generated <ID>_dispatch
// when the DOL fast table misses. Falls back to one interpreter step when the
// pc is not in any active module's table.
extern "C" void aot_module_dispatch(AOTState* s);
