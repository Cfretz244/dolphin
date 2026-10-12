// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// On-device coverage tracing for the AOT runtime (#22 trace1, 2026-10-12).
//
// Every instruction that reaches aot_interpreter_single_step is, by definition,
// code the linked AOT library did not translate. This recorder turns that stream
// into a Core::TraceCollector (.dpht v4, with instruction snapshots), the same
// format the JIT's TraceCollection writes, so `dolphin-tool cfg --trace
// <file> --dol-images` can union it into a game's CFG and the next translate
// covers it. Straight-line fallback runs become blocks; taken branches become
// edges (b/bc = static or call, bclr/bcctr = dynamic). A gap where native code
// ran in between only starts a new run (never an edge).
//
// Threading: Before/After run on the CPU thread only. The lifecycle C API
// serialises itself and must be called either on the CPU thread (the periodic
// flush from a VI hook) or while the CPU thread is quiescent (before boot, after
// Core::Stop has returned, AOTCore::Shutdown).

#include <atomic>

#include "Common/CommonTypes.h"

namespace PowerPC
{
class MMU;
}

namespace AotFallbackTrace
{
// True while a trace is open and the current session records (has an AOT library).
extern std::atomic<bool> g_recording;

inline bool IsRecording()
{
  return g_recording.load(std::memory_order_relaxed);
}

// Around Interpreter::SingleStepInner in the AOT fallback path.
void Before(PowerPC::MMU& mmu, u32 pc);
void After(u32 pc, u32 npc);

// AOTCore::Init: a session without an AOT library would route ALL guest code
// through the fallback; record only sessions that have one.
void SetSessionHasLibrary(bool has_library);
}  // namespace AotFallbackTrace

extern "C" {
// Open (or reopen) the trace at `path`: clears the collector, merges the
// existing file (accumulates across sessions), starts recording.
void aot_fallback_trace_begin(const char* path);
// Close the open run and, if anything changed since the last flush, write the
// whole collector to the path (NOTICE "AOT fallback trace: ..."). No-op when
// no trace is open.
void aot_fallback_trace_flush(void);
// Flush, then stop recording and free the collector. No-op when not open.
void aot_fallback_trace_end(void);
// 1 while a trace is open (begin without end), else 0.
int aot_fallback_trace_is_open(void);
}
