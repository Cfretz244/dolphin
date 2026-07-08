# AOT Recompilation Backend

Dolphin's AOT backend runs GameCube PowerPC code as ahead-of-time-compiled
native code — no runtime code generation — which makes full-speed emulation
possible on platforms that prohibit JIT (`MAP_JIT`), primarily iOS. Game code
is statically translated to C, compiled with Clang, and linked into Dolphin;
at runtime a flat dispatch table maps `pc` to the compiled block function.

Historical working notes from the bring-up live next to this file
(`AOT_STATUS*.md`, `IMPLEMENTATION_LOG.md`, `AOT_REASSEMBLER_PLAN.md`); this
README is the current-state reference.

## Pipeline

Four phases; each produces the next phase's input. Toolchain lives in
`Source/Core/DolphinTool/`, runtime in `Source/Core/Core/PowerPC/AOT/`.

1. **Trace collection** (desktop, manual gameplay): play with
   `-C Dolphin.Debug.TraceCollection=True -C Dolphin.Debug.TraceOutputPath=<f>.dpht`.
   An instrumented JIT records executed blocks and edges (mirrors the
   BranchWatch pattern). Coverage = what you played; untraced code falls back
   to the interpreter at runtime.
2. **CFG extraction**: `dolphin-tool cfg --iso game.iso --trace trace.dpht
   --output cfg.db` — recursive-descent disassembly seeded by traces + the DOL
   entry point, into SQLite.
3. **Translation**: `dolphin-tool translate --cfg cfg.db --iso game.iso
   --output aot-src/` — one C function per block, a flat O(1) dispatch table
   (`<ID>_fast_table[(pc - BASE) >> 2]`), per-REL-module tables for games that
   load relocatable modules, and a self-registration constructor.
4. **Compile + link**: the generated `build.sh` compiles with ThinLTO into
   `lib<ID>_aot.a`; CMake links every configured library into the executables
   with WHOLE_ARCHIVE (`AOT_STATIC_LIBS=...`), and `-C Dolphin.Core.CPUCore=6`
   selects the AOT core.

Multiple games link into one binary; `AOTCore::Init` picks the dispatch table
registered for the running game ID (`AotRegistry`), falling back to the
interpreter when none matches.

## The ABI header and its version

`Source/Core/Core/PowerPC/AOT/aot_runtime.h` is the single source of truth
for the C ABI between generated code and the runtime: the `AOTState` view of
`PowerPCState`, the inline RAM fast paths, every `aot_*` helper signature,
and the registration entry points. dolphin-tool embeds the header's bytes at
build time (`CMake/StringifyHeader.cmake`) and emits a verbatim copy into
every generated tree; the C++ runtime includes the same file (`AotState.h`),
so a signature mismatch is a compile error and a layout mismatch trips the
`offsetof` static_asserts in `AotRuntime.cpp`.

**Rule: any change to `aot_runtime.h` bumps `AOT_ABI_VERSION` and requires
re-translating and rebuilding every game's library.** Generated constructors
pass the version they were built against; `AotRegistry` rejects mismatches
with an ERROR log and the game runs on the interpreter — a stale `.a` can
never silently corrupt state.

## Runtime architecture

- `AOTCore` (CPUCore::AOT = 6): the CPUCoreBase implementation. Production
  `Run()` is a pure fast loop — dispatch, exception check, downcount.
- `AotRuntime.cpp`: `extern "C"` helpers generated code calls for everything
  that isn't inline (slow-path memory, SPR/MSR side effects, FP/paired-single
  ops). Correctness golden rule: helpers replicate
  `Interpreter/` behavior exactly — paired-single math and GQR quantization
  are literally the interpreter's own code, shared via
  `Interpreter_PairedUtils.h` / `Interpreter_PairedTables.h`.
- `AotRegistry`: game ID → dispatch/lookup/module descriptors/(harness) block
  sizes, populated before `main()` by each library's constructor. The
  libraries must be linked WHOLE_ARCHIVE or the constructors are stripped.
- `AotModuleTracker`: walks the OS module queue (REL games) whenever an icbi
  marks it dirty, activating per-module dispatch tables at their runtime load
  addresses. Savestate loads also mark it dirty.
- FP/paired-single delegate through interpreter-shared code; there are no
  independent FP "fast paths" (removed 2026-06 as unsound), and host FP mode
  bits must never leak into compiler-built AOT code (see `ArmFPURoundMode.cpp`
  — FPCR.AH broke clang's NaN-constant idiom; Wind Waker RNG incident).

## Diagnostics (DOLPHIN_AOT_HARNESS builds)

The compare/diff harness compiles only when the `DOLPHIN_AOT_HARNESS` cmake
option is on (desktop AOT builds: ON; iOS: OFF). Production carries zero
instrumentation. Env switches are documented in `AotHarness.h`; the big ones:

| Switch | Purpose |
|---|---|
| `AOT_COMPARE=1` | Live per-block AOT-vs-interpreter comparison during gameplay |
| `AOT_COMPARE_RAM_INTERVAL=N` | Full-RAM compare every Nth comparison (default 16; `1` = always) |
| `AOT_INTERP_ONLY=1` | Bypass AOT entirely (baseline) |
| `AOT_TRACK_FALLBACKS=1` | Per-PC interpreter-fallback counts at shutdown |
| `dolphin-tool diff --iso ...` | Offline block-level comparison (savestate-driven) |

Block boundary metadata for the harness is compiled into the game library
itself when `build.sh` runs with `-DAOT_HARNESS=1` (the desktop default);
without it, comparison modes fail loudly.

### Known false-positive classes in AOT_COMPARE

Both are harness replay artifacts, not codegen bugs — check for them before
chasing a "divergence":

1. **Register-only replay on read-modify-write blocks** (`ram=unchecked` in
   the banner): between full-RAM samples the interpreter re-runs against RAM
   the AOT block already advanced (e.g. popping the next queue entry). Verify
   with `AOT_COMPARE_RAM_INTERVAL=1`, which restores RAM before the replay.
2. **Runtime-loaded MMIO pointers**: a block that loads a hardware-register
   pointer from memory (SDK timer/audio-counter reads) evades the static MMIO
   heuristic, and the two runs read the counter at different downcounts. The
   divergence report auto-annotates this case (`NOTE: rN = 0xccxxxxxx ...`).

## Verification recipe

```bash
# after any AOT change: build, unit tests
cmake --build build --target dolphin-emu unittests
# ABI change? re-translate + rebuild every game first (see version rule above)
# soak each game (expect zero divergences at RAM_INTERVAL=1 modulo class 2 above):
AOT_COMPARE=1 AOT_COMPARE_RAM_INTERVAL=1 ./build/Binaries/DolphinQt.app/Contents/MacOS/DolphinQt \
  -e game.iso -b -C Dolphin.Core.CPUCore=6
# plain gameplay smoke (production fast loop, no env vars)
```

The ISO must be the exact image the traces were recorded from (revision
differences silently mark large fractions of blocks untranslatable).

## Style note

New AOT code follows Dolphin style (clang-format 19.1 per `Contributing.md`);
run the pinned formatter over `Source/Core/Core/PowerPC/AOT/` before
submitting upstream.
