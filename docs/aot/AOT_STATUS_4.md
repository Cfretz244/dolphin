# Dolphin AOT Recompiler — Full Project Summary (2026-04-03)

## Project Goal

Statically recompile GameCube PPC code to ARM64 for iOS, where JIT (`MAP_JIT`) is unavailable. The approach: play a game on desktop to collect execution traces, build a control flow graph, translate PPC blocks to C source code, compile with Clang, and link against a modified Dolphin runtime. No runtime code generation occurs — everything is signed at build time.

## Branch

`instrumentation` — 56 commits ahead of `master`, 43 files changed, ~218K lines added.

## Current State: SSBM Fully Playable

Super Smash Bros. Melee runs with correct video, audio, and camera positioning via the AOT CPU core (`-C Dolphin.Core.CPUCore=6`). Gameplay runs at full speed (faster than the original JIT on Apple Silicon). The pre-rendered intro video is the slowest part. The game is ready for iOS deployment.

---

## Architecture Overview

### Pipeline

```
game.iso  →  Trace Collection  →  CFG Extraction  →  PPC-to-C Translation  →  Clang  →  .a library
                (Phase 1)           (Phase 2)            (Phase 3)              ↓
                                                                           Dolphin Runtime
                                                                           + AOT Core (Phase 4)
                                                                               ↓
                                                                         macOS/iOS App
```

### Phase 1: Instrumented Trace Collection
- **TraceCollector** hooks into JitArm64, records blocks, edges, SMC events during gameplay
- Config: `-C Dolphin.Debug.TraceCollection=True -C Dolphin.Debug.TraceOutputPath=trace.dpht`
- Binary format (.dpht), version 2, merges across sessions
- SSBM trace: ~28K blocks from gameplay sessions

### Phase 2: CFG Extraction
- `dolphin-tool cfg --iso game.iso --trace trace.dpht --output cfg.db`
- Recursive descent disassembly seeded by trace data + DOL entry point
- Output: SQLite database (blocks, edges, functions, smc_regions, metadata)
- SSBM results: 101K blocks, 124K edges, 5.2K functions

### Phase 3: AOT Translation Backend
- `dolphin-tool translate --cfg cfg.db --iso game.iso --output aot_output/`
- Translates each PPC block to a C function: `void GALE01_block_XXXXXXXX(AOTState* s)`
- Game-ID-prefixed symbols for multi-game iOS support
- Output: ~33MB of C source across 59 block files + dispatch table + headers
- **100% instruction coverage** — only `sthbrx` (1 occurrence) falls back to interpreter
- Dispatch: O(1) flat direct-mapped array (~7.4MB, 964K entries)
- Block linking: direct C tail calls between blocks, return to dispatch when downcount ≤ 0

### Phase 4: Runtime Harness & AOT Core
- **AOTCore** (`CPUCore::AOT = 6`): standalone `CPUCoreBase` implementation
- **AotRuntime.cpp**: extern "C" helpers called by generated C code
  - Memory: fast RAM path (direct access for 0x80000000-0x81800000), slow MMU path for MMIO
  - FP/PS arithmetic: delegates to Interpreter methods via macro-generated wrappers
  - System: sc, rfi, mtmsr, mfspr/mtspr with full side effects
  - Cache: dcbz/dcbz_l via ClearDCacheLineFromJit
- Select via `-C Dolphin.Core.CPUCore=6` or Settings → Advanced → "AOT (pre-compiled)"
- CMake: `-DAOT_STATIC_LIB=/path/to/libGALE01_aot.a` defines `DOLPHIN_HAS_AOT`

---

## Build Instructions

### Prerequisites
- macOS ARM64 (Apple Silicon)
- Qt6: `brew install qt`
- Standard Dolphin build deps

### Full pipeline from scratch

```bash
cd /Users/christopherfretz/git/dolphin
mkdir -p build && cd build

# 1. Configure
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt

# 2. Build everything
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
make -j$(sysctl -n hw.ncpu) dolphin-emu

# 8. Run with AOT core
./Binaries/DolphinQt.app/Contents/MacOS/DolphinQt \
  -e "/path/to/game.iso" \
  -C Dolphin.Core.CPUCore=6
```

### Quick rebuild (runtime changes only)
```bash
cd build && make -j$(sysctl -n hw.ncpu) dolphin-emu
```

### Quick rebuild (emitter changes — requires AOT regen)
```bash
cd build
make -j$(sysctl -n hw.ncpu) dolphin-tool
rm -rf aot_output
./Binaries/dolphin-tool translate --cfg trace_output/melee_cfg.db \
  --iso "/path/to/game.iso" --output aot_output/
cd aot_output && for f in GALE01_*.c; do \
  clang -c -O2 -arch arm64 -I. "$f" -o "${f%.c}.o" & done && wait && \
  ar rcs libGALE01_aot.a GALE01_*.o && cd ..
make -j$(sysctl -n hw.ncpu) dolphin-emu
```

---

## SSBM Test Files

- ISO: `/Users/christopherfretz/Downloads/GALE01 - Super Smash Brothers Melee/game.iso`
- Trace: `build/trace_output/melee_trace.dpht`
- CFG DB: `build/trace_output/melee_cfg.db` (101K blocks)
- AOT output: `build/aot_output/` (59 .c files + dispatch + headers)
- AOT library: `build/aot_output/libGALE01_aot.a` (~43MB)

---

## All Bugs Found & Fixed (Chronological)

### Phase 4 — Initial AOT Core (commits 5677260d → 5916214488)
1. **Blocks translated with wrong instruction count** — off-by-one in block boundary calculation
2. **AOT core silently replaced by interpreter on first frame** — `ApplyMode()` replaced `m_cpu_core_base` with `JitInterface::GetCore()` which returns nullptr for AOT
3. **Blocks without branches not decrementing downcount** — dispatch loop spun indefinitely
4. **Fast RAM path** — added direct RAM access bypassing full MMU for 0x80000000-0x81800000

### Phase 5a — Diff Harness & Codegen Fixes (commits 1d4a51b4 → 487123ae)
5. **CR lookup table completely wrong** — every comparison instruction wrote corrupt CR fields
6. **CR logical opcode mapping wrong** — all 8 opcodes mapped to wrong instruction numbers
7. **mtmsr not advancing PC** — block exited before fallthrough epilogue could set PC
8. **subfze missing bitwise NOT** — was computing `rA + carry` instead of `~rA + carry`
9. **FPU availability not checked** — FP instructions executed without MSR.FP check
10. **BAT remapping bottleneck** — `aot_msr_updated()` calling full BAT rebuild (99.6% CPU time)

### Phase 5b — Video & Audio (commits fed36e70 → c5bafa1e)
11. **Idle loop detection too aggressive** — normal game loops triggered it, consuming full timeslices
12. **`sc` instruction missing npc** — syscall handler got stale SRR0, infinite loop at exception vector
13. **`mftb` SPR decode bug** — didn't decode swapped 5-bit halves, timebase always returned 0
14. **Missing SPR handlers** — WPAR, HID0/ICFI, HID2 read-only bits, HID4/BAT updates
15. **stfs float conversion** — custom ConvertToSingle replaced with Dolphin's Gekko-accurate version

### Phase 5c — Video Corruption Fix (commit 66956c18)
16. **DBAT/IBAT SPR writes not triggering BAT table rebuilds** — locked cache at 0xE0000000 unmappable, GPU display list data lost, garbled green video
17. **Missing SPR_DMAL handler** — locked cache DMA transfers never fired
18. **dcbz/dcbz_l using wrong code path** — WriteFromJit (with translation) instead of ClearDCacheLineFromJit (bypasses translation for already-translated addresses)
19. **dcbz_l missing HID2.LCE and HID0.DCE checks** — no program/alignment exception on disabled cache
20. **WPAR read returning hardcoded wrong value** — 0x40000000 (bit 30) instead of IsBNE() on bit 0
21. **dcbz/dcbz_l emitter not handling RA=0** — effective address was GPR[0]+rB instead of 0+rB

### Phase 5d — Camera Bug Fix (commits fb28a56a → 351d2be0)
22. **crxor at wrong SUBOP10** — 385 instead of 193, causing 1172 blocks to double-count cycles
23. **fselx in wrong sub-table** — SUBOP10=846 (unreachable) instead of SUBOP5=23
24. **Custom aot_convert_to_double** — replaced with Dolphin's ConvertToDouble
25. **ps_neg/ps_abs/ps_nabs only operating on PS0** — must operate on BOTH PS0 and PS1. Scalar fneg/fabs/fnabs only affect PS0, but paired-singles versions affect both. This was the camera bug: wrong sign in cross product Y component pushed camera up on every frame.

---

## Diagnostic Tools

### Built into AOTCore::Run() (env vars)
- `AOT_INTERP_ONLY=1` — bypass all AOT blocks, run interpreter only (correct but slow ~4fps)
- `AOT_COMPARE=1` — inline per-block comparison with full 24MB RAM save/restore, MMIO block skip. Reports first register or RAM divergence with full context.
- `AOT_LOAD_STATE=/path/to/save.sav` — load savestate from CLI after boot warmup
- `AOT_SWITCH_AT=N` — run interpreter for first N dispatches, then switch to AOT
- `AOT_DUMP_FRAME=/path/to/output.bin` — dump 24MB RAM after first complete VI frame
- `AOT_LOG_PC=/path/to/output.txt` — log every dispatch PC (pre→post) for diffing between modes

### dolphin-tool diff
- `dolphin-tool diff --iso game.iso --cfg cfg.db [--savestate save.sav] [--max-divergences N]`
- Block-level AOT-vs-interpreter comparison (headless, Null video backend)
- Full HW state save/restore for MMIO blocks, RAM comparison, MMIO write capture

### Config entries (set via -C)
- `Dolphin.Debug.AOTCfgDbPath=/path/to/cfg.db` — for AOT_COMPARE block size loading
- `Dolphin.Debug.AOTDiffMode=True` — enables diff harness in AOTCore
- `Dolphin.Debug.AOTDiffSavestatePath=/path/to/save.sav` — savestate for diff harness

---

## Key Architecture Details

### AOTState ↔ PowerPCState Layout
- Layout-compatible via `reinterpret_cast`, verified by static_asserts for: pc, gather_pipe_ptr, gather_pipe_base_ptr, gpr, downcount, xer_ca, spr, ps, cr, msr, Exceptions, fpscr
- ARM64 layout: ps immediately after gpr (unlike x86_64), alignas(16)

### Memory Access
- Fast path: `(addr & 0x3FFFFFFF) < RAM_SIZE` → direct read/write to `GetRAMPtr()[]` with byte swap
- Slow path: `PowerPC::ReadFromJit<T>()` / `WriteFromJit<T>()` → full MMU translation for MMIO, EFB, locked cache, gather pipe

### Dispatch
- O(1) flat lookup table: `GALE01_fast_table[(pc - 0x80003100) >> 2]`
- NULL entries → `aot_interpreter_single_step()` (one instruction fallback)
- Block chaining: direct C tail calls `GALE01_block_TARGET(s); return;`
- Downcount check before chain: `if (s->downcount <= 0) { s->pc = TARGET; return; }`

### CR Internal Representation
- Dolphin's optimized 64-bit format per field (NOT standard PPC 4-bit)
- SO: bit 59, EQ: lower 32 bits == 0, GT: (s64)value > 0, LT: bit 62
- AOT uses inline `aot_cr_get_bit()` / `aot_cr_set_field()` helpers

---

## File Map

```
Source/Core/Core/PowerPC/
  TraceCollector.{h,cpp}     — Phase 1: trace recording
  AotRuntime.cpp             — Runtime helpers (memory, FP, SPR, cache, PS moves)
  AOTCore.{h,cpp}            — CPU core backend + diff harness + diagnostic modes
  MMIOCapture.h              — MMIO write capture for diff harness

Source/Core/Core/Config/
  MainSettings.{h,cpp}       — Config entries for trace/diff/compare modes

Source/Core/DolphinTool/
  CfgCommand.{h,cpp}         — Phase 2: CFG extraction
  AotCommand.{h,cpp}         — Phase 3: translation orchestrator + dispatch table gen
  AotCEmitter.{h,cpp}        — Phase 3: PPC→C emitter (all instruction handlers)
  DiffCommand.{h,cpp}        — diff harness CLI
  PPCMemoryImage.h           — Shared ROM memory image class
  ToolMain.cpp               — Subcommand registration

Source/Core/Core/PowerPC/
  PowerPC.{h,cpp}            — CPUCore enum (AOT=6), InitializeCPUCore, ApplyMode
  MMU.cpp                    — MMIO capture hooks in WriteToHardware

Source/Core/Core/Boot/
  DolReader.h                — Added text section accessors for Phase 2

Source/Core/DolphinQt/Settings/
  AdvancedPane.cpp           — AOT entry in CPU core dropdown

Source/Core/Core/CMakeLists.txt — sqlite3 linked to core library

Externals/sqlite3/           — SQLite amalgamation

Tools/
  trace_to_sqlite.py         — .dpht → SQLite converter
```

---

## Lessons Learned

1. **Always cross-reference with the interpreter.** The AOT runtime helpers must exactly match Dolphin's interpreter behavior, including side effects. The recurring bug pattern: implementing from PPC ISA docs instead of copying from the interpreter.

2. **Paired-singles ≠ scalar.** ps_neg/ps_abs/ps_nabs operate on BOTH PS0 and PS1. Scalar fneg/fabs/fnabs only operate on PS0. This subtle difference caused the camera bug.

3. **SPR writes have side effects.** Writing to DBAT/IBAT registers must call DBATUpdated()/IBATUpdated(). Writing to DMAL triggers DMA. Writing to WPAR resets the gather pipe. Always check the interpreter's mtspr handler for every SPR.

4. **Opcode table numbering.** Dolphin uses SUBOP10 (bits 1-10) and SUBOP5 (bits 1-5) for different instruction sub-tables. Getting the wrong sub-table or wrong opcode number causes silent fallback to interpreter with double cycle counting.

5. **Block-level comparison has blind spots.** Per-block comparison (running AOT and interpreter from the same state) can't detect issues that only manifest through cumulative execution or PS1 sign propagation. Full RAM comparison after real execution was needed to find the camera bug.

6. **Diagnostic tools pay for themselves.** The AOT_COMPARE, AOT_LOAD_STATE, and AOT_DUMP_FRAME tools were essential for narrowing down the camera bug from "something is wrong" to "byte 0x804ec840 differs by one sign bit."

---

## What's Next: iOS Deployment

The AOT recompiler is verified working on macOS ARM64. The next phase is deploying to iOS:

1. **Strip JIT from Dolphin build** — remove MAP_JIT, block cache, code emitter
2. **iOS app structure** — main binary (AOT .a + Dolphin runtime), game.iso as data asset
3. **Metal backend** — pass CAMetalLayer via WindowSystemInfo for rendering
4. **Audio** — DeltaSoundStream or direct AudioUnit integration
5. **Input** — controller skin or MFi controller mapping
6. **Signing** — everything is statically compiled, no runtime codegen

The ROM is still needed as a data asset (textures, audio, models, etc.). The AOT library contains all translated code. The interpreter fallback handles the 1 remaining untranslated instruction (sthbrx).
