// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT/AOTCore.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <mbedtls/sha256.h>

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/PowerPC/AOT/AotModuleTracker.h"
#include "Core/PowerPC/AOT/AotRegistry.h"
#include "Core/PowerPC/AOT/AotState.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"

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
    m_expected_dol_sha256 = entry->dol_sha256;
    m_image_checked = false;
    m_image_blocked = false;
    AotModuleTracker::Init(entry->modules, entry->module_count);
    INFO_LOG_FMT(AOT, "AOTCore: Found AOT library for game {} ({} REL modules{})", game_id,
                 entry->module_count,
                 m_expected_dol_sha256.empty() ? ", NO image hash -- pre-v24 library" : "");
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

bool AOTCore::VerifyImageIdentity()
{
  if (m_expected_dol_sha256.empty())
  {
    if (m_dispatch != m_interp_dispatch)
    {
      WARN_LOG_FMT(AOT, "AOTCore: library carries no source-image hash (pre-v24) -- cannot "
                        "verify the mounted disc matches it. Re-run `stack.sh translate` to "
                        "embed one.");
    }
    return true;
  }

  // Private cloned reader: never touch the mounted volume's blob cursor
  // (same pattern as the jukebox / AchievementManager).
  const std::unique_ptr<DiscIO::Volume> volume = m_system.GetDVDThread().CloneDiscForHostReads();
  if (!volume)
  {
    WARN_LOG_FMT(AOT, "AOTCore: no disc to verify the AOT library against (direct DOL/ELF "
                      "boot?) -- proceeding unverified.");
    return true;
  }

  const DiscIO::Partition partition = volume->GetGamePartition();
  const auto dol_offset = DiscIO::GetBootDOLOffset(*volume, partition);
  std::optional<u32> dol_size;
  if (dol_offset)
    dol_size = DiscIO::GetBootDOLSize(*volume, partition, *dol_offset);
  std::vector<u8> dol(dol_size.value_or(0));
  if (dol.empty() || !volume->Read(*dol_offset, dol.size(), dol.data(), partition))
  {
    WARN_LOG_FMT(AOT, "AOTCore: could not read the boot DOL for verification -- proceeding "
                      "unverified.");
    return true;
  }

  u8 digest[32];
  mbedtls_sha256_ret(dol.data(), dol.size(), digest, 0);
  std::string actual;
  actual.reserve(64);
  for (const u8 b : digest)
    actual += fmt::format("{:02x}", b);

  if (actual == m_expected_dol_sha256)
  {
    INFO_LOG_FMT(AOT, "AOTCore: disc image verified against the AOT library (dol sha256 {})",
                 actual);
    return true;
  }

  ERROR_LOG_FMT(AOT,
                "AOTCore: IMAGE MISMATCH -- the AOT library for {} was generated from a disc "
                "whose boot DOL hashes {}, but the mounted disc's DOL hashes {}. Refusing to "
                "run translated code against the wrong image.",
                SConfig::GetInstance().GetGameID(), m_expected_dol_sha256, actual);
  PanicAlertFmtT("The AOT library for {0} was built from a DIFFERENT disc image than the one "
                 "you just launched (boot-DOL checksum mismatch).\n\nRunning it would crash or "
                 "silently misbehave, so emulation has been stopped.\n\nLaunch the exact image "
                 "the library was generated from, or re-run the AOT pipeline "
                 "(stack.sh translate) against this image.",
                 SConfig::GetInstance().GetGameID());
  return false;
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

  if (!m_image_checked)
  {
    m_image_checked = true;
    if (!VerifyImageIdentity())
    {
      // Hard stop, not a silent interpreter fallback: pause the core (the
      // panic alert above says why) and never dispatch translated code.
      m_image_blocked = true;
      m_dispatch = m_interp_dispatch;
    }
  }
  if (m_image_blocked)
  {
    cpu.Break();
    return;
  }

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
