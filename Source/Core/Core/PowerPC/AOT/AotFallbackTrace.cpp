// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/AOT/AotFallbackTrace.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/TraceCollector.h"

namespace AotFallbackTrace
{
std::atomic<bool> g_recording{false};

namespace
{
// A straight-line interpreted stretch longer than this is closed and restarted,
// so one long run never grows unbounded.
constexpr size_t kMaxRunInstructions = 1024;

// Lifecycle state (begin/flush/end), serialised by s_lifecycle_mutex.
std::mutex s_lifecycle_mutex;
bool s_open = false;
bool s_session_has_library = true;
std::string s_path;

// Own collector instance (never the JIT's).
Core::TraceCollector s_collector;

// Current run (CPU thread). s_run_hash folds every word so a run whose content
// changed (SMC, a different module loaded at the same address) is a new key.
u32 s_run_start = 0;
std::vector<u32> s_run_words;
u64 s_run_hash = 0;
u32 s_expected_next_pc = 0xFFFFFFFFu;
// Word fetched by Before() for the instruction now stepping; false when the
// fetch failed (ISI): that instruction is not recorded.
u32 s_cur_word = 0;
bool s_cur_valid = false;

// RecordBlock appends an epoch to the matching snapshot on EVERY call, so a hot
// interpreted loop would grow its epoch list without bound. Each distinct
// (start, length, content) run is handed to the collector once per session.
std::unordered_set<u64> s_seen_runs;
// Direct-mapped filter in front of the collector's edge map: a hot taken branch
// costs one compare instead of a hash-map probe. Zero = empty slot (an edge key
// is never 0: `to` would have to be 0 and `from` 0).
std::array<u64, 4096> s_edge_filter{};

// Flush bookkeeping: write only when something changed.
bool s_dirty = false;
size_t s_flushed_edges = 0;

constexpr u64 kFnvPrime = 0x100000001b3ULL;
constexpr u64 kFnvOffset = 0xcbf29ce484222325ULL;

void UpdateRecording()
{
  g_recording.store(s_open && s_session_has_library, std::memory_order_relaxed);
}

void CloseRun()
{
  if (s_run_words.empty())
    return;
  const u32 size = static_cast<u32>(s_run_words.size());
  u64 key = s_run_hash ^ (static_cast<u64>(s_run_start) << 20) ^ size;
  key *= kFnvPrime;
  if (s_seen_runs.insert(key).second)
  {
    s_collector.RecordBlock(s_run_start, size, s_run_words.data());
    s_dirty = true;
  }
  s_run_words.clear();
}

void RecordEdge(u32 from, u32 to, int kind)  // kind: 0 static, 1 call, 2 dynamic
{
  const u64 key = (static_cast<u64>(from) << 32) | (to & ~3u) | static_cast<u64>(kind);
  const size_t slot = static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 52);  // 12 bits
  if (s_edge_filter[slot] == key)
    return;
  s_edge_filter[slot] = key;
  if (kind == 2)
    Core::TraceCollector::LogDynamicBranch(&s_collector, from, to);
  else
    s_collector.RecordStaticEdge(from, to, kind == 1);
}

void ResetRunState()
{
  s_run_words.clear();
  s_run_words.reserve(kMaxRunInstructions);
  s_run_hash = kFnvOffset;
  s_expected_next_pc = 0xFFFFFFFFu;
  s_cur_valid = false;
  s_seen_runs.clear();
  s_edge_filter.fill(0);
}

// Caller holds s_lifecycle_mutex.
void FlushLocked()
{
  if (!s_open)
    return;
  CloseRun();
  const size_t edges = s_collector.EdgeCount();
  if (!s_dirty && edges == s_flushed_edges)
    return;
  s_collector.FlushToDisk(s_path);
  s_dirty = false;
  s_flushed_edges = edges;
  const size_t blocks = s_collector.BlockCount();
  const u64 bytes = File::GetSize(s_path);
  std::fprintf(stderr, "AOT fallback trace: %zu blocks %zu edges -> %s (%llu bytes)\n", blocks,
               edges, s_path.c_str(), static_cast<unsigned long long>(bytes));
  NOTICE_LOG_FMT(CORE, "AOT fallback trace: {} blocks {} edges -> {} ({} bytes)", blocks, edges,
                 s_path, bytes);
}
}  // namespace

void Before(PowerPC::MMU& mmu, u32 pc)
{
  if (s_run_words.empty() || pc != s_expected_next_pc ||
      s_run_words.size() >= kMaxRunInstructions)
  {
    // Native code ran in between (or the cap was hit): new run, never an edge.
    CloseRun();
    s_run_start = pc;
    s_run_hash = kFnvOffset;
  }
  // The interpreter's own opcode fetch (Interpreter::SingleStepInner ->
  // MMU::Read_Opcode) minus its ISI side effect. Same address, same icache
  // line: the interpreter's fetch right after this one is a hit and leaves the
  // cache state as a single fetch would.
  const PowerPC::TryReadInstResult r = mmu.TryReadInstruction(pc);
  s_cur_valid = r.valid;
  if (!r.valid)
  {
    CloseRun();
    return;
  }
  s_cur_word = r.hex;
  s_run_words.push_back(r.hex);
  s_run_hash = (s_run_hash ^ r.hex) * kFnvPrime;
}

void After(u32 pc, u32 npc)
{
  s_expected_next_pc = npc;
  if (!s_cur_valid || npc == pc + 4)
    return;

  // Taken branch, exception or rfi: the run ends at this instruction.
  CloseRun();
  const UGeckoInstruction inst(s_cur_word);
  if (inst.OPCD == 18 || inst.OPCD == 16)  // b/bl, bc/bcl
    RecordEdge(pc, npc, inst.LK ? 1 : 0);
  else if (inst.OPCD == 19 && (inst.SUBOP10 == 16 || inst.SUBOP10 == 528))  // bclr / bcctr
    RecordEdge(pc, npc, 2);
  // sc, exceptions (DSI, program, ...), rfi: no edge.
}

void SetSessionHasLibrary(bool has_library)
{
  std::lock_guard lock(s_lifecycle_mutex);
  s_session_has_library = has_library;
  if (s_open && !has_library)
  {
    NOTICE_LOG_FMT(CORE, "AOT fallback trace: no AOT library for this session; not recording");
  }
  UpdateRecording();
}
}  // namespace AotFallbackTrace

using namespace AotFallbackTrace;

extern "C" void aot_fallback_trace_begin(const char* path)
{
  if (path == nullptr || *path == '\0')
    return;
  std::lock_guard lock(s_lifecycle_mutex);
  s_open = false;
  UpdateRecording();
  s_path = path;
  s_collector.Clear();
  ResetRunState();
  const bool existed = File::Exists(s_path);
  if (existed)
    s_collector.MergeFromDisk(s_path);
  s_dirty = false;
  s_flushed_edges = s_collector.EdgeCount();
  s_open = true;
  UpdateRecording();
  std::fprintf(stderr, "AOT fallback trace: begin %s (existing: %zu blocks %zu edges)\n",
               s_path.c_str(), s_collector.BlockCount(), s_flushed_edges);
  NOTICE_LOG_FMT(CORE, "AOT fallback trace: begin {} (existing: {} blocks {} edges)", s_path,
                 s_collector.BlockCount(), s_flushed_edges);
}

extern "C" void aot_fallback_trace_flush(void)
{
  std::lock_guard lock(s_lifecycle_mutex);
  FlushLocked();
}

extern "C" void aot_fallback_trace_end(void)
{
  std::lock_guard lock(s_lifecycle_mutex);
  if (!s_open)
    return;
  FlushLocked();
  s_open = false;
  UpdateRecording();
  s_collector.Clear();
  ResetRunState();
  s_seen_runs = {};
}

extern "C" int aot_fallback_trace_is_open(void)
{
  std::lock_guard lock(s_lifecycle_mutex);
  return s_open ? 1 : 0;
}
