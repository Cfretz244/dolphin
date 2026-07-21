# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Dolphin is a GameCube/Wii emulator (GPLv2+). This fork adds an **ahead-of-time (AOT) recompilation pipeline** that statically transpiles PPC code to C, compiles it with Clang, and links against Dolphin's runtime — enabling iOS deployment without JIT (no `MAP_JIT`). SSBM runs at full speed on macOS ARM64. Development happens on `master` of the Cfretz244/dolphin fork (the historical `ios-aot` branch was merged into it 2026-07-18 and is dormant), which also carries iOS platform support for integration with the [Delta emulator](https://github.com/rileytestut/Delta) frontend — ~200 commits / ~230K lines ahead of upstream.

## Build Commands

```bash
# Prerequisites: macOS ARM64, Qt6 (brew install qt), standard Dolphin deps
# Pull submodules first: git submodule update --init --recursive

# Configure + build (macOS)
# NOTE: MoltenVK (bundled Vulkan) fails to build on recent Xcode/macOS due to
# a self-linking dylib error. Add -DENABLE_VULKAN=OFF to work around this.
# Metal is the primary backend anyway, so Vulkan is not needed.
# NOTE: An existing build cache errors with "The selection for <LIB> changed
# from 'Bundled' to 'System'" when homebrew has since installed a matching lib
# (e.g. fmt, xxhash). Pin the affected libs back to bundled with
# -DUSE_SYSTEM_<LIB>=OFF (and clear stale detection results with -U "<LIB>_*"
# if a failed configure already cached them), or reconfigure from a fresh
# build directory.
mkdir -p build && cd build
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -DENABLE_VULKAN=OFF
make -j$(sysctl -n hw.ncpu) dolphin-emu dolphin-tool

# Quick rebuild (runtime changes only)
cd build && make -j$(sysctl -n hw.ncpu) dolphin-emu

# Build with AOT library linked in (with LTO for cross-boundary optimization)
# Single game:
cmake .. -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DENABLE_LTO=ON -DAOT_STATIC_LIB=/path/to/libGALE01_aot.a
# Multiple games (semicolon-separated):
cmake .. -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DENABLE_LTO=ON \
  -DAOT_STATIC_LIBS="/path/to/libGALE01_aot.a;/path/to/libGZLE01_aot.a"
make -j$(sysctl -n hw.ncpu) dolphin-emu

# iOS build (as a static library for Delta integration)
cmake .. -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DDOLPHIN_IOS_BUILD=ON -DENABLE_LTO=ON \
  -DAOT_STATIC_LIBS="/path/to/libGALE01_aot.a"

# Run tests
cd build && cmake --build . --target unittests
```

## AOT Pipeline

The pipeline has four phases, each producing artifacts consumed by the next:

**Phase 1 — Trace Collection:** Play a game with instrumented JIT to record executed blocks/edges.
```bash
./Binaries/DolphinQt.app/Contents/MacOS/DolphinQt -e game.iso \
  -C Dolphin.Debug.TraceCollection=True \
  -C Dolphin.Debug.TraceOutputPath=trace.dpht
```

**Phase 2 — CFG Extraction:** Recursive descent disassembly seeded by traces + DOL entry point. Output is a SQLite database. `--trace` is repeatable: multiple trace files union into one CFG (keep the broad base trace, add targeted traces for problem scenes; v4 snapshot timelines are rebased sequentially). Overlay-image clustering is opt-in via `--overlays` — pass it ONLY for Camelot-style overlay games (MPT); REL-based games would get garbage images from their REL-resident snapshots. Delete a pre-existing output DB before rerunning — the writer does not drop stale tables.
```bash
./Binaries/dolphin-tool cfg --iso game.iso --trace trace.dpht --trace boss_fight.dpht --output cfg.db
```

**Phase 3 — PPC-to-C Translation:** Emits C functions per block + O(1) flat dispatch table.
```bash
./Binaries/dolphin-tool translate --cfg cfg.db --iso game.iso --output aot_output/
```

**Phase 4 — Compile + Link:** Compile C to .a with ThinLTO, rebuild Dolphin with `-DAOT_STATIC_LIB=` (or `-DAOT_STATIC_LIBS=` for multiple games) and `-DENABLE_LTO=ON`.
```bash
cd aot_output
# Option A: use the generated build script (includes -flto=thin)
bash build.sh

# Option B: manual compilation with ThinLTO
for f in GALE01_*.c; do clang -c -O2 -flto=thin -arch arm64 -I. "$f" -o "${f%.c}.o" & done; wait
ar rcs libGALE01_aot.a GALE01_*.o
```

**Run with AOT core:** `-C Dolphin.Core.CPUCore=6`

### Multi-Game AOT

Multiple AOT libraries can be linked into a single Dolphin binary. Each library self-registers via `__attribute__((constructor))` at startup. At runtime, `AOTCore::Init()` reads `SConfig::GetGameID()` and selects the matching dispatch table from the `AotRegistry`. If no AOT library exists for the loaded game, it falls back to the interpreter.

```bash
# Build AOT libraries for each game independently (phases 1-4)
# Then link them all together:
cmake .. -DENABLE_LTO=ON \
  -DAOT_STATIC_LIBS="/path/to/libGALE01_aot.a;/path/to/libGZLE01_aot.a;/path/to/libGMPE01_aot.a"
make -j$(sysctl -n hw.ncpu) dolphin-emu

# Dolphin auto-selects the right AOT backend per game:
./dolphin-emu -e ssbm.iso -C Dolphin.Core.CPUCore=6   # uses GALE01 AOT
./dolphin-emu -e zelda.iso -C Dolphin.Core.CPUCore=6   # uses GZLE01 AOT
./dolphin-emu -e other.iso -C Dolphin.Core.CPUCore=6   # no AOT → interpreter fallback
```

## Code Formatting

- clang-format 19.1 required. Run on staged files:
  ```bash
  git diff --cached --name-only | grep -E '[.](cpp|h|mm)$' | xargs -I {} clang-format -i {}
  ```
- Lint hook: `ln -s ../../Tools/lint.sh .git/hooks/pre-commit`
- See `Contributing.md` for full coding style. Key points: 100-char line limit, 2-space indent, left-aligned pointers (`int* var`), opening brace on next line for namespaces/classes/functions.

## Architecture: AOT-Specific Code

### Key Files

```
Source/Core/Core/PowerPC/
  TraceCollector.{h,cpp}     — Phase 1: hooks into JitArm64, records blocks/edges/SMC
  AOTCore.{h,cpp}            — CPUCoreBase impl (CPUCore::AOT=6), dispatch loop, diff harness
  AotRegistry.{h,cpp}        — Multi-game registry: maps game_id → dispatch/lookup function pointers
  AotRuntime.cpp             — extern "C" helpers called by generated C (memory, FP, SPR, cache)
  MMIOCapture.h              — MMIO write tracking for diff harness

Source/Core/DolphinTool/
  CfgCommand.{h,cpp}         — Phase 2: CFG extraction from traces + disassembly
  AotCEmitter.{h,cpp}        — Phase 3: PPC instruction → C code emitter
  AotCommand.{h,cpp}         — Phase 3: orchestrator + dispatch table generation
  VertexLoaderCEmitter.{h,cpp} — Vertex format → C vertex loader emitter
  VtxAotCommand.{h,cpp}      — `dolphin-tool vtxaot`: vertex loader AOT orchestrator
  DiffCommand.{h,cpp}        — Offline AOT-vs-interpreter comparison tool
  PPCMemoryImage.h           — Shared ROM memory image class

Source/Core/VideoCommon/
  VertexLoaderAOT.{h,cpp}    — VertexLoaderBase impl wrapping AOT-compiled C loaders
  VertexLoaderAotRegistry.{h,cpp} — Per-game vertex loader registry (constructor-based registration)

Source/Core/Core/Config/MainSettings.{h,cpp} — Config entries for trace/diff/compare
Source/Core/DolphinQt/Settings/AdvancedPane.cpp — AOT in CPU core dropdown
Source/Core/Core/PowerPC/PowerPC.{h,cpp} — CPUCore enum (AOT=6), InitializeCPUCore

# iOS / Delta integration
Source/Core/Core/HW/GCPad.{h,cpp}           — SetExternalProvider() for injecting input from Delta
Source/Core/AudioCommon/DeltaSoundStream.{h,cpp} — Pull-based audio backend (HAVE_DELTA_AUDIO)
Source/Core/Common/WindowSystemInfo.h        — WindowSystemType::IOS enum value
Source/Core/InputCommon/ControllerInterface/ControllerInterface.h — ForceSetInit() for iOS
Source/Core/VideoBackends/Metal/MTLGfx.{h,mm} — Blit-based frame capture for iOS
```

### AOTState and PowerPCState

`AOTState` is layout-compatible with `PowerPCState` via `reinterpret_cast`, verified by `static_assert`s. Generated C code operates on `AOTState*`; the runtime casts it back to access Dolphin internals.

### Dispatch

- O(1) flat lookup: `{PREFIX}_fast_table[(pc - TABLE_BASE) >> 2]`
- NULL entries fall back to `aot_interpreter_single_step()` (one PPC instruction via interpreter)
- Known static targets use direct C tail calls between blocks
- Downcount check before chaining: `if (s->downcount <= 0) { s->pc = TARGET; return; }`
- Each game's dispatch.c includes an `__attribute__((constructor))` that registers with `AotRegistry` at startup
- `AOTCore::Init()` queries the registry by game ID to select the correct dispatch table

### Memory Access in Runtime Helpers

- **Fast path:** Direct RAM access for 0x80000000-0x81800000 (and uncached mirror 0xC0000000+), byte-swapped
- **Slow path:** `PowerPC::ReadFromJit<T>()` / `WriteFromJit<T>()` for MMIO, EFB, locked cache, gather pipe

### CR Internal Representation

Dolphin uses an optimized 64-bit format per CR field (NOT standard PPC 4-bit). AOT code uses `aot_cr_get_bit()` / `aot_cr_set_field()` helpers that match this format exactly.

### FP/Paired-Singles

All FP arithmetic delegates to Dolphin's interpreter methods via macro-generated `extern "C"` wrappers in `AotRuntime.cpp`. Critical: `ps_neg`/`ps_abs`/`ps_nabs` operate on BOTH PS0 and PS1 (unlike scalar `fneg`/`fabs`/`fnabs` which only affect PS0).

### Vertex Loader AOT

On iOS, the ARM64 vertex loader JIT is disabled. The vertex loader AOT pipeline generates pre-compiled C vertex loaders to replace the slow software fallback (~20x faster). Games use only 5-30 unique vertex formats.

**Pipeline:**
1. **Trace:** Vertex format UIDs (5 u32 register values) are recorded alongside PPC traces during gameplay (`.dpht` v3 format).
2. **CFG extraction:** `dolphin-tool cfg` extracts vertex formats into a `vertex_formats` SQLite table.
3. **C generation:** `dolphin-tool vtxaot --cfg cfg.db --output aot_output/ --prefix GALE01` generates monolithic C vertex loader functions.
4. **Compile + link:** Output `.c` files compile into the same `.a` as PPC AOT code.

**Runtime:** `VertexLoaderAotRegistry` receives registrations via `__attribute__((constructor))`. `CreateVertexLoader()` in `VertexLoaderBase.cpp` checks the registry before falling back to JIT/software. `VertexLoaderAOT` wraps the C function, passing arraybases/strides/z-freeze caches as parameters.

**Validation:** Use `VertexLoaderType::Compare` (`-C Dolphin.GFX.VertexLoaderType=2`) to compare AOT vs JIT output byte-for-byte.

## Diagnostic Tools

| Env Var / Flag | Purpose |
|---|---|
| `AOT_INTERP_ONLY=1` | Bypass AOT, interpreter-only (baseline, ~4fps) |
| `AOT_COMPARE=1` | Per-block AOT-vs-interpreter comparison with 24MB RAM save/restore |
| `AOT_LOAD_STATE=/path` | Load savestate from CLI after boot warmup |
| `AOT_SWITCH_AT=N` | Interpreter for first N dispatches, then AOT |
| `AOT_DUMP_FRAME=/path` | Dump 24MB RAM after first VI frame |
| `AOT_LOG_PC=/path` | Log every dispatch PC for diffing |
| `AOT_TRACK_FALLBACKS=1` | Log interpreter fallback stats per-PC at shutdown |
| `dolphin-tool diff --iso --cfg` | Offline block-level AOT-vs-interpreter diff |

Config entries via `-C`: `Dolphin.Debug.AOTCfgDbPath`, `Dolphin.Debug.AOTDiffMode`, `Dolphin.Debug.AOTDiffSavestatePath`, `Dolphin.Debug.AOTDiffLogPath`.

## Important Patterns for AOT Development

1. **Always match the interpreter.** Runtime helpers must exactly replicate Dolphin's interpreter behavior including all side effects. Implement from `Source/Core/Core/PowerPC/Interpreter/` — not from PPC ISA docs.

2. **SPR writes have side effects.** DBAT/IBAT writes must call `DBATUpdated()`/`IBATUpdated()`. DMAL triggers DMA. WPAR resets gather pipe. Always check the interpreter's `mtspr` handler.

3. **Opcode table numbering matters.** Dolphin uses SUBOP10 (bits 1-10) and SUBOP5 (bits 1-5) for different sub-tables in `AotCEmitter`. Wrong sub-table = silent interpreter fallback with double cycle counting.

4. **Config flags use "Dolphin" prefix**, not "Main". Example: `-C Dolphin.Core.CPUCore=6`.

## iOS Platform Support

This fork adds support for building Dolphin as a library for integration with the Delta emulator on iOS. Key adaptations:

### Build Differences (`DOLPHIN_IOS_BUILD=ON`)

- **Disabled deps:** libusb (no IOKit), CURL, HIDAPI, Quartz-based ControllerInterface init
- **JIT vertex loader disabled:** ARM64 JIT code generation unavailable on iOS; uses software vertex loader fallback
- **SecTranslocate guarded:** macOS-only API excluded via `TARGET_OS_OSX` check
- **Metal backend:** Accepts `WindowSystemType::IOS`, uses blit-based frame capture (`MTLGfx::CaptureLastFrame()`) instead of swap-chain presentation

### Delta Integration Points

- **Input:** `Pad::SetExternalProvider(callback)` / `Pad::ClearExternalProvider()` — Delta injects `GCPadStatus` directly, bypassing ControllerInterface
- **Audio:** `DeltaSoundStream` — pull-based backend where Delta calls `PullSamples(buffer, num_frames)` at 48kHz to retrieve mixed audio
- **Video:** `MTLGfx::CaptureLastFrame()` captures the last rendered frame via blit for Delta to display
- **Controller init:** `ControllerInterface::ForceSetInit()` skips Quartz-dependent initialization path

## Dolphin Core Architecture (Non-AOT)

- **CPU backends:** `CPUCoreBase` interface with JIT64, JitArm64, Interpreter, CachedInterpreter, and AOT implementations. Selected via `CPUCore` enum in `PowerPC.h`.
- **Video backends:** OpenGL, Vulkan, D3D11/12, Metal, Software, Null — in `Source/Core/VideoBackends/`.
- **Config system:** Layered config with `-C` CLI overrides. Settings defined in `Source/Core/Core/Config/`.
- **Frontends:** DolphinQt (GUI), DolphinNoGUI (headless), DolphinTool (CLI utilities).
- **Externals:** Bundled third-party libs in `Externals/` (gtest, fmt, sqlite3, cubeb, etc.). System libs used when available via `USE_SYSTEM_LIBS`.
- **Tests:** Google Test in `Source/UnitTests/`, organized by component (Common, Core, VideoCommon).
