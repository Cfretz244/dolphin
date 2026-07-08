// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT/AOTCore.h"

#include <string>

#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/AOT/AotModuleTracker.h"
#include "Core/PowerPC/AOT/AotRegistry.h"
#include "Core/PowerPC/AOT/AotState.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef DOLPHIN_AOT_HARNESS
#include "Core/PowerPC/AOT/AotHarness.h"
#endif

#include "Core/PowerPC/AOT/AotEmbedding.h"

static_assert(static_cast<int>(PowerPC::CPUCore::AOT) == 6,
              "CPUCore::AOT is frozen at 6 for deployed-config compatibility "
              "(update AotEmbedding.h consumers if this ever changes)");
extern "C" const int kDolphinCPUCoreAOT = static_cast<int>(PowerPC::CPUCore::AOT);

extern "C" void aot_init_fast_mem();
extern "C" void aot_shutdown();
extern "C" void aot_dump_fallback_stats();

static void interp_only_dispatch(AOTState* s)
{
  aot_interpreter_single_step(s);
}

AOTCore::AOTCore(Core::System& system) : m_system(system), m_ppc_state(system.GetPPCState())
{
}

AOTCore::~AOTCore() = default;

void AOTCore::Init()
{
  // Cache RAM pointer and size for fast memory access
  aot_init_fast_mem();

  m_interp_dispatch = &interp_only_dispatch;

  // Look up the AOT library for the currently loaded game.
  const std::string game_id = SConfig::GetInstance().GetGameID();
  auto entry = AotRegistry::Instance().Find(game_id);
  if (entry)
  {
    m_dispatch = entry->dispatch;
    AotModuleTracker::Init(entry->modules, entry->module_count);
    INFO_LOG_FMT(AOT, "AOTCore: Found AOT library for game {} ({} REL modules)", game_id,
                 entry->module_count);
  }
  else
  {
    m_dispatch = m_interp_dispatch;
    const auto registered = AotRegistry::Instance().GetRegisteredGameIDs();
    std::string available;
    for (size_t i = 0; i < registered.size(); i++)
    {
      if (i > 0)
        available += ", ";
      available += registered[i];
    }
    WARN_LOG_FMT(AOT,
                 "AOTCore: No AOT library for game {}. Available: [{}]. "
                 "Falling back to interpreter.",
                 game_id, available);
  }

#ifdef DOLPHIN_AOT_HARNESS
  m_harness = AotHarness::MaybeCreate(m_system, entry, m_dispatch, m_interp_dispatch);
#endif
}

void AOTCore::Shutdown()
{
  aot_dump_fallback_stats();
#ifdef DOLPHIN_AOT_HARNESS
  m_harness.reset();
#endif
  AotModuleTracker::Shutdown();
  m_dispatch = nullptr;
  aot_shutdown();
}

void AOTCore::ClearCache()
{
  AotModuleTracker::MarkDirty();
}

void AOTCore::Run()
{
#ifdef DOLPHIN_AOT_HARNESS
  if (m_harness)
  {
    m_harness->Run();
    return;
  }
#endif

  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();
  auto& power_pc = m_system.GetPowerPC();

  while (cpu.GetState() == CPU::State::Running)
  {
    m_ppc_state.npc = m_ppc_state.pc;
    core_timing.Advance();

    auto* aot_state = ToAot(m_ppc_state);
    do
    {
      m_dispatch(aot_state);
      if (m_ppc_state.Exceptions != 0)
      {
        m_ppc_state.npc = m_ppc_state.pc;
        power_pc.CheckExceptions();
      }
    } while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running);
  }
}

void AOTCore::SingleStep()
{
  auto& core_timing = m_system.GetCoreTiming();
  core_timing.Advance();

  // Force the first block terminator's downcount guard to trip so a "step" is
  // one block, not a whole chained slice (blocks chain across fall-through and
  // taken edges until downcount exhausts).
  auto* aot_state = ToAot(m_ppc_state);
  m_ppc_state.downcount = 1;
  m_dispatch(aot_state);

  m_ppc_state.downcount = 0;
}
