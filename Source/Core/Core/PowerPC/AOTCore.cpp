// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOTCore.h"

#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef DOLPHIN_HAS_AOT

// AOTState definition — must be layout-compatible with PowerPCState.
// This is the struct that AOT-generated C code operates on.
// Defined here for the static_assert checks; the actual definition used by
// generated code is in the emitted aot_runtime.h header.
struct AOTState
{
  uint32_t pc;
  uint32_t npc;
  void* stored_stack_pointer;
  void* gather_pipe_ptr;
  void* gather_pipe_base_ptr;
  uint32_t gpr[32];
  struct
  {
    uint64_t ps0;
    uint64_t ps1;
  } ps[32] __attribute__((aligned(16)));
  uint64_t cr_fields[8];
  uint32_t msr;
  uint32_t fpscr;
  uint32_t feature_flags;
  uint32_t exceptions;
  int32_t downcount;
  uint8_t xer_ca;
  uint8_t xer_so_ov;
  uint16_t xer_stringctrl;
  uint32_t reserve_address;
  uint8_t reserve;
  uint8_t pagetable_update_pending;
  uint8_t m_enable_dcache;
  uint8_t _pad0;
  uint32_t sr[16];
  uint32_t spr[1024] __attribute__((aligned(8)));
};

// Verify layout compatibility between PowerPCState and AOTState.
// If any of these fail, the AOT code will silently corrupt state.
static_assert(offsetof(PowerPC::PowerPCState, pc) == offsetof(AOTState, pc),
              "pc offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, gpr) == offsetof(AOTState, gpr),
              "gpr offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, downcount) == offsetof(AOTState, downcount),
              "downcount offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, xer_ca) == offsetof(AOTState, xer_ca),
              "xer_ca offset mismatch");
static_assert(offsetof(PowerPC::PowerPCState, spr) == offsetof(AOTState, spr),
              "spr offset mismatch");

// The AOT dispatch function — defined in the pre-compiled AOT static library.
// This symbol is resolved at link time.
extern "C" void GALE01_dispatch(AOTState* s);

AOTCore::AOTCore(Core::System& system)
    : m_system(system), m_ppc_state(system.GetPPCState())
{
}

AOTCore::~AOTCore() = default;

void AOTCore::Init()
{
  m_dispatch = &GALE01_dispatch;
  INFO_LOG_FMT(POWERPC, "AOTCore: Initialized with GALE01 dispatch table");
}

void AOTCore::Shutdown()
{
  m_dispatch = nullptr;
}

void AOTCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();

  u64 dispatch_count = 0;
  auto start = std::chrono::steady_clock::now();

  while (cpu.GetState() == CPU::State::Running)
  {
    core_timing.Advance();

    while (m_ppc_state.downcount > 0 && cpu.GetState() == CPU::State::Running)
    {
      auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
      m_dispatch(aot_state);
      dispatch_count++;
    }

    if ((dispatch_count & 0x3FFF) == 0 && dispatch_count > 0)
    {
      auto now = std::chrono::steady_clock::now();
      double secs = std::chrono::duration<double>(now - start).count();
      fprintf(stderr, "AOT: %.1fM dispatches in %.1fs (%.1fM/s)\n",
              dispatch_count / 1e6, secs, dispatch_count / secs / 1e6);
    }
  }

  auto end = std::chrono::steady_clock::now();
  double total = std::chrono::duration<double>(end - start).count();
  fprintf(stderr, "AOT total: %llu dispatches in %.1fs (%.1fM/s)\n",
          dispatch_count, total, dispatch_count / total / 1e6);
}

void AOTCore::SingleStep()
{
  auto& core_timing = m_system.GetCoreTiming();
  core_timing.Advance();

  auto* aot_state = reinterpret_cast<AOTState*>(&m_ppc_state);
  m_dispatch(aot_state);

  m_ppc_state.downcount = 0;
}

#else  // !DOLPHIN_HAS_AOT

// Stub implementation when AOT is not enabled
AOTCore::AOTCore(Core::System& system)
    : m_system(system), m_ppc_state(system.GetPPCState())
{
}
AOTCore::~AOTCore() = default;
void AOTCore::Init() {}
void AOTCore::Shutdown() {}
void AOTCore::Run() {}
void AOTCore::SingleStep() {}

#endif  // DOLPHIN_HAS_AOT
