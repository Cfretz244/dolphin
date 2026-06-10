# AOT Project Restoration — June 2026

Notes from the June 2026 effort to restore the AOT pipeline to a functional, correct state after
the project was shelved in April 2026. The last known-good point was `fdcb0a1e24`; everything
after `6f2b89fa10` ("AOT codegen optimization pass") was a premature-optimization era that ended
with Melee running but with all colors wrong, and a failed attempt to re-establish a clean build.
Safety tag `pre-restoration-2026-06` marks the pre-cleanup HEAD (`09daba5e8b`).

## What the April history actually did (vs. what was remembered)

- **Block merging** (added in `6f2b89fa10`) **was fully removed** in `a939771b1e`. Done.
- **The FP fast paths** (`081fdc50fe`) **were never removed** — they were repeatedly *patched*
  (`a939771b1e` FPSCR FPRF, `cea30caa63` FR/FI, `09daba5e8b` negative-zero FPRF + Force25Bit)
  and were still live at the pre-restoration HEAD. Removed in this restoration: all FP ops now
  delegate to the interpreter wrappers in `AotRuntime.cpp` again.
- **musttail / noinline** (from `6f2b89fa10`): sound, kept. musttail actually fixed a real
  unbounded-stack-growth hazard in block chaining. (noinline is for code size/section
  attribution, not TCO, despite the commit message.)
- **Hot/cold section attributes**: purely perf, no correctness risk, kept.
- **Downcount batching** (from `6f2b89fa10`): **unsound as shipped.** The commit message claims
  checks happen "at backward edges and indirect dispatches", but the generated dispatch never
  checked downcount. Any execution cycle whose only backward jumps are indirect (`blr`/`bctr`
  dispatch loops) never returned to the Run loop → CoreTiming never advanced → interrupts never
  delivered → hard hang. The AOT_COMPARE harness overwrites downcount, so this entire bug class
  was invisible to validation. Likely the source of mystery bugs chased in other ROMs.
  Fixed in this restoration: dispatch checks downcount, and forward static edges check again
  (pre-optimization behavior). Re-batching can be revisited with A/B perf data.
- **Cached RAM pointer fast path**: sound lifecycle (re-cached in `AOTCore::Init()`), but the
  range check also accepted the 0x0/0x4 quadrants where the interpreter would raise a DSI.
  Tightened to the 0x8/0xC quadrants in this restoration.

## The "all colors wrong" mystery (Melee, April 2026)

Ruled out:
- **AOT vertex loaders**: `ar t` shows zero `vtx` objects in every game archive (including the
  iOS archives in `~/git/GCDeltaCore/aot-libs/`). `vtxaot` for GALE01 crashed mid-generation
  (only `vtx_aot_runtime.h` was written; no loaders/dispatch/build_vtx.sh exist anywhere).
  No AOT vertex loader ever ran for Melee.
- **FP fast paths**: Melee's color data (vertex colors, TEV/material/light registers) never
  passes through CPU FP math. FP bugs would show as geometry/physics divergence, not uniform
  color errors.

Prime remaining suspect: the hand-rolled iOS frame-capture chain —
`MTLGfx.mm PresentBackbuffer()` blit capture (BGRA8) → Delta's display path — a classic
R/B-swap surface. Platform where the symptom was observed is not remembered; the desktop
isolation test (same build, interpreter vs `CPUCore=6`, normal Metal presentation) settles it.

## Known bugs left in place (deferred)

1. **Vertex loader AOT emitter** (`Source/Core/DolphinTool/VertexLoaderCEmitter.cpp`) — fix
   before reviving the vtx pipeline:
   - `:150` Tex4Frac misparse: ORs g1 bit 31 (`VCacheEnhance`, always 1) in as frac bit 0 and
     shifts the real bits. Should be `m_tex_frac[4] = (g2 >> 0) & 0x1F;` per `CPMemory.h`.
   - `GenerateLoaderFunction`: the static per-vertex offset `cur_src` is never advanced past the
     1-2 bytes consumed by an *indexed* attribute, so any *direct* attribute following an indexed
     one reads from a short offset (likely cause of Metroid Prime's broken video).
   - Also: `vtxaot` died mid-run on Melee's 1268 recorded formats — find out why (it prints
     per-format progress). Validate with `-C GFX.Settings.VertexLoaderType=2` (Compare mode)
     before linking into iOS.
2. **Cycle double-count on interpreter fallback** (predates the optimization era):
   `TranslateBlock` charges fallback instructions' cycles into the block total
   (`AotCEmitter.cpp` ~:43) and `aot_interpreter_single_step` charges them again
   (`AotRuntime.cpp` ~:288). Fix if timing accuracy becomes a concern.
3. **`-mcpu=apple-a14`** in generated build scripts: fine for the chosen device floor
   (iPhone 12+/A14). If older devices ever need support, lower the flag — pre-A14 chips will
   SIGILL on v8.4+ instructions.

## Deferred work

- **`aot-instruction-capture-and-icbi-invalidation` branch** (`bc278b6951`, unmerged, based on
  `5b24d3c9a5`): v4 trace format capturing actual runtime instruction bytes (CRC32-deduplicated)
  + real `icbi` invalidation. Root-cause fix for the DOL/runtime-mismatch problem that block
  merging tripped over. Evaluate/merge once the restored baseline is trusted.
- **iOS/Delta reintegration**: other repos are `~/git/GCDeltaCore` and `~/git/Delta`. Includes
  the capture-chain color check (dump `s_capture_buffer` one frame, view as BGRA vs RGBA).
- A stale stash exists on the `instrumentation` branch ("Skip already-validated blocks after 100
  successful comparisons") — debug tooling WIP, probably discardable.

## Harness false positives (fixed June 2026)

The compare/diff harness ran the interpreter until PC left the block's address range, but a
single-block AOT run can exit mid-range (internal unconditional `b`, `mtmsr`, first backward
edge of an internal loop). Every such block was a guaranteed false-positive divergence —
this is where much of April's `0x8034xxxx` "divergence" noise came from. Fixed by stopping
the interpreter at the AOT run's exit PC (with backward-jump disambiguation for loop blocks).
Offline Melee diff: 25 divergences/59k comparisons before → 0 divergences/135k comparisons
after.

## Validation assets

- Traces/CFG DBs: `build/trace_output/` (melee, wind_waker, metroid_prime, metroid_prime_2,
  kirby — `.dpht` + `_cfg.db` each).
- Melee ISO: `~/Library/Mobile Documents/com~apple~CloudDocs/Downloads/Super Smash Bros. Melee
  (USA) (En,Ja) (Rev 2).nkit.iso`.
- Savestates: `~/working/melee testing/` (`start_match.sav`, `before_crash.sav`).
