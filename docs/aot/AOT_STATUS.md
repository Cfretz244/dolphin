# Dolphin AOT Recompiler — Project Status & Handoff Document

## Project Goal

Statically recompile GameCube PPC code to ARM64 for iOS, where JIT is unavailable due to `MAP_JIT` restrictions. The approach: play a game on desktop to collect execution traces, build a control flow graph, translate PPC blocks to C source code, compile with Clang, and link against a Dolphin runtime harness.

## Branch

`instrumentation` — 14 commits ahead of `master`

## What's Complete

### Phase 1: Instrumented Trace Collection (commit f5060f26)
- **TraceCollector** class hooks into JitArm64 to record blocks, edges, SMC events during gameplay
- Records static branch targets from `linkData` after `FinalizeBlock()`
- Records dynamic branch targets (bctr/blr) via `WriteTraceLogDynamicBranch()` using the same ABI_CallFunction pattern as BranchWatch
- SMC detection via `InvalidateICacheInternal()` hook
- Config: `-C Dolphin.Debug.TraceCollection=True -C Dolphin.Debug.TraceOutputPath=/path/to/trace.dpht`
- **Important**: Config system name is `"Dolphin"` not `"Main"` (System::Main maps to "Dolphin" in CLI flags)
- Binary format (.dpht), version 2, merges across sessions
- Files: `Source/Core/Core/PowerPC/TraceCollector.{h,cpp}`, config in `MainSettings.{h,cpp}`
- Python converter: `Tools/trace_to_sqlite.py`

### Phase 2: CFG Extraction (commit 9a64273f)
- `dolphin-tool cfg --iso game.iso --trace trace.dpht --output cfg.db`
- Recursive descent disassembly seeded by trace data + DOL entry point
- Uses `PPCTables::GetOpInfo()` and `IsValidInstruction()` for instruction decoding (statically initialized, no runtime deps)
- Reads DOL from disc via `DiscIO::CreateDisc()` + `GetBootDOLOffset()` + `DolReader`
- Output: SQLite database with tables: blocks, edges, functions, smc_regions, metadata
- SSBM results: 101k blocks, 124k edges, 5.2k functions, 41.6% code coverage of 3.7MB text sections
- Added SQLite3 as external dependency (amalgamation copied from mGBA)
- Added public accessors to `DolReader.h` for text section metadata
- Files: `Source/Core/DolphinTool/CfgCommand.{h,cpp}`, `PPCMemoryImage.h`

### Phase 3: AOT Translation Backend (commits 71860c0b → d8f63ae2)
- `dolphin-tool translate --cfg cfg.db --iso game.iso --output aot_output/ [--prefix GALE01]`
- Translates each PPC block to a C function: `void GALE01_block_XXXXXXXX(AOTState* s)`
- **100% instruction coverage for SSBM** (0 unhandled opcodes in final build)
- Game-ID-prefixed symbols (e.g., `GALE01_block_80003200`) for multi-game iOS support
- Output: ~33MB of C source across 59 block files + dispatch table + headers
- Compiles cleanly with `clang -O2 -arch arm64`

#### Instruction handling:
- **Integer arithmetic**: Emitted as inline C (add, sub, mul, div, shift, rotate, compare, logical, carry, overflow, CR0 update)
- **Load/store**: Calls to `aot_read_u32()` / `aot_write_u32()` etc.
- **FP arithmetic**: Calls to runtime helpers that delegate to Interpreter methods (e.g., `aot_faddsx()` → `Interpreter::faddsx()`)
- **Paired singles**: Calls to runtime helpers
- **Branches**: Direct branches → C tail calls (`GALE01_block_TARGET(s); return;`). Indirect → `GALE01_dispatch(s); return;`
- **System/SPR**: Inline for simple SPRs (LR, CTR, XER), runtime helpers for special ones (DEC, TL, TU, WPAR)
- **CR logical**: Runtime helper `aot_cr_logical()`
- **Cache ops**: Mostly no-ops, runtime for dcbz/icbi

#### Known code generation issues (current state):
- Blocks that don't end with a branch instruction (e.g., `mtmsr` sequences) now emit a fallthrough: `s->downcount -= N; s->pc = NEXT_ADDR;` at the end of every block
- Conditional branches that are NOT taken fall through to this same epilogue
- The `I()` macro (`#define I(x) (+(x))`) is used throughout `AotCEmitter.cpp` to promote bitfield members for `fmt::format` compatibility
- `fselx` and `sthbrx` are the only 2 unhandled opcodes (1 occurrence each)

#### Files:
- `Source/Core/DolphinTool/AotCommand.{h,cpp}` — CLI command, reads CFG DB + ROM, orchestrates translation
- `Source/Core/DolphinTool/AotCEmitter.{h,cpp}` — The PPC→C emitter class
- `Source/Core/DolphinTool/PPCMemoryImage.h` — Shared memory image class (also used by CfgCommand)

### Phase 4: Runtime Helper Functions (commit f6348c53)
- `Source/Core/Core/PowerPC/AotRuntime.cpp` — All `extern "C"` `aot_*` functions
- Memory access: `aot_read/write_u{8,16,32,64}()` with fast RAM path for 0x80000000-0x81800000 (bypasses MMU for main RAM, falls back for MMIO)
- FP: Delegates to `Interpreter::faddsx(GetInterpreter(), inst)` etc. via macro-generated wrappers
- PS: Same delegation pattern
- System: `aot_sc()`, `aot_rfi()`, `aot_msr_updated()`, `aot_mfspr_special()`, `aot_mtspr_special()`
- Interpreter fallback: `aot_interpreter_single_step()` calls `Interpreter::SingleStepInner()`
- `aot_convert_to_double()` / `aot_convert_to_single()` for FP load/store format conversion
- Cache: `aot_dcbz()`, `aot_icbi()`, `aot_dcbt()` (no-op)

### Phase 4.5: macOS AOT Core Integration (commit 5677260d → current)
- `Source/Core/Core/PowerPC/AOTCore.{h,cpp}` — New `CPUCoreBase` implementation
- `CPUCore::AOT = 6` added to enum
- Select via `-C Dolphin.Core.CPUCore=6` or Settings → Advanced → "AOT (pre-compiled)"
- CMake option: `-DAOT_STATIC_LIB=/path/to/libGALE01_aot.a`
- When set, defines `DOLPHIN_HAS_AOT` preprocessor macro
- AOTCore is NOT a JitBase subclass — standalone CPUCoreBase
- `static_assert` checks verify PowerPCState/AOTState layout compatibility at compile time

## Current State: What Works, What Doesn't

### Works:
- The full pipeline: trace → CFG → translate → compile → link → AOT core runs
- AOT core initializes correctly and dispatches to compiled block functions
- CPU runs at full speed with Null video backend (profiling showed 78% idle time = CPU keeping up)
- Status bar correctly shows "AOT SC" (after ApplyMode fix)

### Doesn't work:
- **Game shows black screen with real video backend** — no rendering, no audio
- The CachedInterpreter (CPUCore=5) DOES render correctly for the same game
- This means there's a **correctness bug** in the AOT translated code that prevents the game from progressing through boot to the point where it starts rendering

### Key debugging insight:
A hybrid approach was tested: interpreter for first 50M instructions (boot), then switch to AOT. This was not conclusively tested before the session ended. This is a promising debugging avenue — if the game boots with interpreter and then runs with AOT, the bug is in boot-time code; if it breaks immediately after switching, it's in common gameplay code.

## Build Instructions

### Prerequisites
- macOS ARM64 (Apple Silicon)
- Qt6: `brew install qt`
- Standard Dolphin build deps

### Full pipeline from scratch:

```bash
cd /Users/christopherfretz/git/dolphin

# 1. Configure
mkdir -p build && cd build
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt

# 2. Build dolphin-tool and run trace collection on a game
make -j$(sysctl -n hw.ncpu) dolphin-emu dolphin-nogui dolphin-tool

# 3. Collect trace (play the game, quit with Cmd+Q)
./Binaries/DolphinQt.app/Contents/MacOS/DolphinQt \
  -e "/path/to/game.iso" \
  -C Dolphin.Debug.TraceCollection=True \
  -C "Dolphin.Debug.TraceOutputPath=$(pwd)/trace_output/melee_trace.dpht"

# 4. Build CFG
./Binaries/dolphin-tool cfg \
  --iso "/path/to/game.iso" \
  --trace trace_output/melee_trace.dpht \
  --output trace_output/melee_cfg.db

# 5. Translate to C
./Binaries/dolphin-tool translate \
  --cfg trace_output/melee_cfg.db \
  --iso "/path/to/game.iso" \
  --output aot_output/

# 6. Compile AOT static library
cd aot_output
for f in GALE01_*.c; do
  clang -c -O2 -arch arm64 -I. "$f" -o "${f%.c}.o" &
done
wait
ar rcs libGALE01_aot.a GALE01_*.o
cd ..

# 7. Reconfigure with AOT library and rebuild
cmake .. -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DAOT_STATIC_LIB=$(pwd)/aot_output/libGALE01_aot.a
make -j$(sysctl -n hw.ncpu) dolphin-emu dolphin-nogui

# 8. Run with AOT core
./Binaries/DolphinQt.app/Contents/MacOS/DolphinQt \
  -e "/path/to/game.iso" \
  -C Dolphin.Core.CPUCore=6
```

### Quick rebuild after code changes:

If only `AotRuntime.cpp` or `AOTCore.cpp` changed (no AOT C code changes):
```bash
cd build && make -j$(sysctl -n hw.ncpu) dolphin-emu
```

If `AotCEmitter.cpp` or `AotCommand.cpp` changed (need to regenerate C code):
```bash
cd build
make -j$(sysctl -n hw.ncpu) dolphin-tool
rm -rf aot_output
./Binaries/dolphin-tool translate --cfg trace_output/melee_cfg.db --iso "/path/to/game.iso" --output aot_output/
cd aot_output && for f in GALE01_*.c; do clang -c -O2 -arch arm64 -I. "$f" -o "${f%.c}.o" & done && wait && ar rcs libGALE01_aot.a GALE01_*.o && cd ..
make -j$(sysctl -n hw.ncpu) dolphin-emu
```

## SSBM Test Files
- ISO: `/Users/christopherfretz/Downloads/GALE01 - Super Smash Brothers Melee/game.iso`
- Trace: `build/trace_output/melee_trace.dpht` (28k blocks from gameplay session)
- CFG DB: `build/trace_output/melee_cfg.db` (101k blocks after static analysis)
- AOT output: `build/aot_output/` (59 .c files + dispatch + headers)
- AOT library: `build/aot_output/libGALE01_aot.a` (~34MB)

## Key Architecture Details

### AOTState struct
Defined in the generated `aot_runtime.h` and mirrored in `AOTCore.cpp` for `static_assert` checks. Must be layout-compatible with `PowerPC::PowerPCState` (PowerPC.h:120). Key fields:
- `gpr[32]` at same offset as PowerPCState
- `ps[32]` (paired singles) — ARM64 layout (immediately after gpr, unlike x86_64)
- `cr_fields[8]` — Dolphin's optimized 64-bit CR representation (NOT standard PPC 32-bit CR)
- `spr[1024]` — SPR_LR=8, SPR_CTR=9, SPR_XER=1, SPR_DEC=22, SPR_TL=268, SPR_TU=269
- `downcount` — timing budget, decremented by each block
- `xer_ca`, `xer_so_ov` — decomposed XER fields

### CR Internal Representation (ConditionRegister.h)
Dolphin does NOT use standard PPC 4-bit CR fields. Instead, each of the 8 CR fields is stored as a 64-bit value:
- SO: bit 59 set
- EQ: lower 32 bits == 0
- GT: (s64)value > 0
- LT: bit 62 set
Conversion to/from PPC format uses `ConditionRegister::s_crTable[16]` lookup table.
The AOT `aot_runtime.h` includes inline helpers `aot_cr_get_bit()` and `aot_cr_set_field()` that replicate this encoding.

### Dispatch
The generated `GALE01_dispatch.c` contains a flat direct-mapped array:
```c
static AOTBlockFunc GALE01_fast_table[964832] = { ... };
void GALE01_dispatch(AOTState* s) {
    uint32_t idx = (s->pc - 0x80003100u) >> 2;
    if (idx < 964832u) {
        AOTBlockFunc fn = GALE01_fast_table[idx];
        if (fn) { fn(s); return; }
    }
    aot_interpreter_single_step(s);
}
```
O(1) lookup. NULL entries fall back to interpreter.

### Block exit pattern
Every block ends with either:
1. A branch that does `s->downcount -= N; ... GALE01_block_TARGET(s); return;` (taken path)
2. A fallthrough epilogue: `s->downcount -= N; s->pc = NEXT_ADDR;` (not-taken or no branch)

The AOTCore::Run() loop calls `m_dispatch(aot_state)` repeatedly while `downcount > 0`.

### Memory access
Fast path for main RAM: `addr & 0x3FFFFFFF < 0x01800000` → direct read/write to `GetRAMPtr()[]` with byte swap.
Slow path: `PowerPC::ReadFromJit<T>(GetMMU(), addr)` — full MMU translation for MMIO, EFB, locked cache, etc.

### ApplyMode bug (fixed in commit 6763a295)
`PowerPCManager::ApplyMode()` is called whenever `SetMode()` is called (which happens on the first frame for breakpoint checking). It replaces `m_cpu_core_base` with `JitInterface::GetCore()`, which returns nullptr for AOT (not a JitBase), causing fallback to interpreter. Fix: check `m_aot_core` first in `ApplyMode()`.

## Lessons Learned

1. **Config system naming**: `System::Main` maps to `"Dolphin"` in CLI `-C` flags, NOT `"Main"`. See `Common/Config/Config.cpp:system_to_name`.

2. **Bitfield + fmt::format**: UGeckoInstruction fields are bitfields that can't bind to non-const refs. Use `I(x)` macro (`#define I(x) (+(x))`) for unary-plus promotion.

3. **Pre-existing bug**: `DebugModeEnabled=True + JitEnableProfiling=True` crashes on ARM64 macOS (heap corruption in malloc). Profiling requires both flags but the combination is broken in upstream Dolphin.

4. **ApplyMode() silent replacement**: The AOT core was silently replaced by the interpreter on the very first frame. This was the root cause of early "performance" issues — AOT code was never actually running.

5. **Blocks without branches**: Blocks that end with non-branch ENDBLOCK instructions (mtmsr, etc.) or conditional branches that fall through need explicit downcount decrement and pc set. Without this, the dispatch loop spins indefinitely.

6. **Idle loop detection**: Multi-block polling loops (e.g., 3 blocks cycling) burn CPU. Attempted detection via PC history ring buffer at dispatch level, but tail calls between blocks bypass the dispatcher, making detection ineffective. This remains an unsolved optimization problem.

7. **The real blocker**: There is a **correctness bug** in the AOT-translated code. The game runs at full CPU speed but never gets past a black screen (no video output, no audio). The CachedInterpreter works correctly for the same game. The bug is likely in one of the heavily-used instruction translations (integer arithmetic, CR update, or branch logic) that corrupts game state during boot.

## What Needs to Be Built Next: Emulator State Comparison Tool

The highest-priority next step is a **block-by-block comparison tool** that:
1. Runs both AOT and the interpreter for each block
2. Compares the full PowerPCState after each block
3. Reports the first divergence with full context (which block, which registers differ, what the block's PPC instructions were)

This would be implemented as a mode in `AOTCore::Run()` that:
- Saves PowerPCState before dispatch
- Runs one block through AOT dispatch
- Saves the AOT result
- Restores the saved state
- Runs the same block through the interpreter (instruction by instruction until the block's end address)
- Compares all state: gpr[32], cr_fields[8], spr[LR/CTR/XER], xer_ca, xer_so_ov, pc, fpscr, ps[32]
- On first mismatch: dump both states and the PPC disassembly of the block

The challenge is that AOT runs a whole block at once while the interpreter runs one instruction at a time. To compare, you need to know where the block ends (the next pc after the block). The block's end can be determined from the CFG database or by counting instructions.

An alternative simpler approach: run the interpreter for every block but also run the AOT function, comparing the results. If they match for the first N blocks, the AOT code is correct for those blocks. When they diverge, you've found the bug.

## File Map

```
Source/Core/Core/PowerPC/
  TraceCollector.{h,cpp}     — Phase 1: trace recording
  AotRuntime.cpp             — Phase 4: extern "C" runtime helpers
  AOTCore.{h,cpp}            — Phase 4.5: CPU core backend

Source/Core/Core/Config/
  MainSettings.{h,cpp}       — Config entries for trace collection

Source/Core/DolphinTool/
  CfgCommand.{h,cpp}         — Phase 2: CFG extraction
  AotCommand.{h,cpp}         — Phase 3: AOT translation orchestrator  
  AotCEmitter.{h,cpp}        — Phase 3: PPC→C emitter
  PPCMemoryImage.h           — Shared memory image class
  ToolMain.cpp               — Subcommand registration

Source/Core/Core/PowerPC/
  PowerPC.{h,cpp}            — CPUCore enum, InitializeCPUCore, ApplyMode
  
Source/Core/Core/Boot/
  DolReader.h                — Added text section accessors

Source/Core/DolphinQt/Settings/
  AdvancedPane.cpp           — AOT entry in CPU core dropdown

Externals/sqlite3/           — SQLite amalgamation

Tools/
  trace_to_sqlite.py         — .dpht → SQLite converter
```

## Commit History (on `instrumentation` branch)

```
5916214488 Add idle loop detection to AOT dispatch loop
1f616f373e Fix blocks without branches not decrementing downcount
c8d6a78c27 Remove debug prints from AOT core and runtime
6763a2957f Fix AOT core being silently replaced by interpreter on first frame
7e67858db5 Fix critical bug: blocks translated with wrong instruction count
3ba6af01db Add fast RAM path to AOT memory helpers, bypassing MMU
fe22862f79 Replace binary search dispatch with O(1) flat lookup table
5677260d3a Add AOT CPU core backend for macOS validation (Phase 4.5)
f6348c53a3 Add AOT runtime helper functions (Phase 4)
d8f63ae2fe Complete PPC instruction coverage for AOT translator (M4-M5)
fe76fe4fc3 Add load/store, FP load/store, SPR, CR, and system instruction emitters (M2-M3)
71860c0b56 Add AOT PPC-to-C translation backend (Phase 3, Milestone 1)
9a64273fe8 Add CFG extraction tool for AOT recompilation (Phase 2)
f5060f2628 Add instrumented trace collection for AOT recompilation (Phase 1)
```
