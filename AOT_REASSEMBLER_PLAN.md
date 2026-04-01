Claude Plan
# Dolphin AOT Reassembler: Project Plan

## Static Binary Translation of GameCube Titles for iOS

-----

## Overview

The end product is an iOS app containing a statically compiled ARM64 translation of (most of) a GameCube title’s PPC code, linked against a modified Dolphin runtime that provides the memory map, HW emulation, and an interpreter fallback for untranslated paths. The ROM ships as a data asset for the interpreter fallback and for all non-code data (textures, audio, models, etc.). No runtime code generation occurs — everything is signed at build time.

-----

## Phase 1: Instrumented Trace Collection

**Goal:** Build a modified Dolphin that logs every PPC instruction address and block entry point that executes during gameplay.

Patch Dolphin’s JIT (or use its interpreter mode with instrumentation) to record a trace log. The key data per block:

- **Block entry address** (PPC virtual address)
- **Block length** (number of instructions before the next branch/terminator)
- **Branch targets** — both static (known at translate time) and dynamic (register-indirect), where for dynamic branches you log the actual resolved target each time
- **Call stack context** — which function called which, to reconstruct a call graph
- **Memory access patterns for code regions** — flags self-modifying code (any writes to addresses that are also executed)

The output is a **trace database** — a SQLite or flat file that accumulates across play sessions. Play through the entire game: main menu, every level, every cutscene, every edge case you can trigger. The more coverage, the fewer interpreter fallbacks at runtime.

### Trace DB Schema

```
blocks:
  ppc_addr  |  length  |  hit_count  |  static_targets[]  |  observed_dynamic_targets[]

edges:
  from_addr  |  to_addr  |  edge_type (call/branch/indirect)  |  hit_count

smc_regions:
  addr_range  |  write_sources[]  |  frequency
```

### Deliverable

A desktop Dolphin fork that produces this trace DB. Note that there is no meaningful “coverage percentage” at this stage — a GameCube disc image is an opaque blob with no metadata distinguishing code from data, so there is no ground truth denominator. The trace DB is a best-effort seed; its completeness is validated later in Phase 5 by measuring interpreter fallback frequency during actual AOT execution.

-----

## Phase 2: Code Discovery & CFG Construction

**Goal:** Take the trace DB plus static analysis of the ROM image and build a complete control flow graph of all known code.

Start with the trace data as the seed — every observed block entry is a confirmed code address. Then run recursive descent disassembly from each seed to discover additional blocks the trace might have missed (dead code, error handlers, rare branches that never fired during play sessions).

### Block Terminator Classification

For each block, classify the terminator:

- **Direct branch/call:** Target is known statically. Add edge to CFG.
- **Conditional branch:** Both targets known. Add both edges.
- **Indirect branch (blr, bctr):** Target is a register value. Use the trace DB’s observed targets as the known set. Flag as “open” — might hit untranslated code at runtime.
- **System call / exception:** These route into Dolphin’s HLE. They become calls into the runtime harness.

### Function Identification

Identify functions — contiguous groups of blocks with a single entry point (or a small number of entries for tail-call-optimized code). Function boundaries matter because they’re the unit of translation and the linkage points.

### SMC Region Flagging

Any PPC address range that was both executed and written to (from Phase 1) gets flagged as non-translatable — it must go through the interpreter at runtime.

### Deliverable

A CFG data structure (serialized to disk) representing every known function, block, and edge. A classification of each block as translatable vs. interpreter-required. A comparison metric: of blocks discovered via static analysis (recursive descent), how many were also confirmed by the runtime trace? The gap represents statically-reachable code that wasn’t exercised during play — candidates for targeted playtesting or manual inspection.

-----

## Phase 3: The AOT Translation Backend

**Goal:** Build an offline tool that consumes the CFG and the ROM image, and emits ARM64 machine code for each translatable block.

This is the core of the project and where Dolphin’s existing JIT gets co-opted. The key architectural decision: **no new code generator.** Repurpose JitArm64’s existing `Emit*` methods, but instead of writing into an executable memory buffer at runtime, write into an object file.

### 3a. Extract the Translation Core

Dolphin’s JitArm64 class inherits from JitBase and has methods like `EmitInstruction()`, register allocation logic, block linking, etc. Refactor this into a library that can be called offline. The main change: instead of using `ARM64XEmitter` to write into a `MAP_JIT` buffer, write into a flat byte buffer that gets wrapped in an object file. All instruction selection, register mapping, and optimization logic stays the same.

### 3b. Handle the Runtime Interface

In the JIT, generated code accesses the emulator state (registers, memory) through hardcoded offsets from a base pointer (typically a dedicated ARM64 register points to `PowerPCState`). For AOT, do exactly the same thing — the translated code assumes a register (say x19) holds a pointer to the emulator state struct, and accesses PPC GPRs, FPRs, CR, etc. at fixed offsets. The difference is that instead of this being set up by the JIT dispatcher, it’s set up by a C calling convention — translated functions receive the state pointer as a parameter or pick it up from a global.

### 3c. Memory Access Translation

PPC loads/stores go through Dolphin’s memory map (BAT/page table translation). The JIT already emits code to do this — either fastmem (direct pointer arithmetic with signal handler fallback) or explicit address translation calls. For AOT on iOS, emit calls to Dolphin’s `Memory::Read_U32()` / `Write_U32()` etc. for every memory access. This is slower but correct and signable. Hot paths can be optimized later.

### 3d. Block Linking and Dispatch

In the JIT, blocks are linked by patching branch instructions to point to the next compiled block. For AOT, all static targets are known at build time, so emit normal ARM64 `bl`/`b` instructions with relocations that the linker resolves. For indirect branches, emit a call to a **dispatch function** that looks up the target PPC address in a table mapping PPC addresses to translated function pointers. If the target isn’t in the table, the dispatch function falls into the interpreter.

The dispatch function is the critical glue:

```c
typedef void (*TranslatedBlock)(PowerPCState* state);

// Generated at build time from the CFG
extern const std::unordered_map<u32, TranslatedBlock> aot_block_table;

void aot_dispatch(PowerPCState* state) {
    while (true) {
        auto it = aot_block_table.find(state->pc);
        if (it != aot_block_table.end()) {
            it->second(state);  // run translated code
        } else {
            interpreter_step(state);  // fallback
        }
    }
}
```

### 3e. Emit as a Linkable Object

The translated ARM64 code gets written into a `.o` file (Mach-O for iOS) with proper symbols and relocations. Each translated function becomes a symbol (`_aot_ppc_80003100`, etc.). The block table becomes a data section. This `.o` links against Dolphin’s runtime library and the interpreter.

### Deliverable

A command-line tool: `reassembler --rom game.iso --cfg cfg.db --output translated.o`. Also a header file declaring the block table and dispatch entry point.

-----

## Phase 4: The Dolphin Runtime Harness

**Goal:** Strip Dolphin down to a runtime library suitable for iOS, with the AOT translated code as its CPU backend.

### Components to Keep

- **Memory subsystem** — address space setup, BAT translation, memory-mapped I/O routing. Stays mostly as-is.
- **HLE / LLE hardware** — GPU (graphics command processor, display lists → existing video backends), DSP (audio), SI (controllers), EXI (memory cards, etc.), VI (video timing). These all stay.
- **Interpreter** — Dolphin’s PPC interpreter, kept as the fallback for untranslated blocks and SMC regions.
- **AOT dispatch loop** — replaces the JIT as the CPU execution engine. Calls translated blocks when available, falls back to interpreter otherwise.

### Components to Remove

- JIT compiler, block cache, code emitter, `MAP_JIT` allocations — none of this ships. The translated `.o` is linked at build time.

### iOS App Structure

```
MyGameApp.app/
  MyGameApp              (main binary, signed — contains translated.o + dolphin runtime)
  game.iso               (ROM data — loaded as read-only asset)
  Frameworks/
    MoltenVK.framework   (or whatever Dolphin uses for Vulkan→Metal)
```

The ROM file is still needed even though the code is translated, because the emulator reads data from it constantly — texture data, audio samples, level geometry, string tables, etc. Code pages in the ROM are also needed for the interpreter fallback. The ROM is just data now, not executed.

### Deliverable

A modified Dolphin that builds as an iOS app, uses AOT dispatch instead of JIT, links the translated object, and loads the ROM as a data asset.

-----

## Phase 5: Per-Title Hardening

**Goal:** Get a specific title running correctly and optimize it.

### 5a. Coverage Iteration

Play through on the instrumented desktop build. Check coverage. Hit new code paths. Re-export the trace DB. Re-run the reassembler. Repeat until coverage stabilizes. Expect 95–99% of executed code to be captured this way. The remaining 1–5% hits the interpreter at runtime — just slow, not broken.

### 5b. SMC Handling

If the title does self-modifying code, identify the patterns. Common ones: overlay loading (game loads a new code module from disc into RAM and jumps to it). For these, pre-translate all overlays and add them to the block table keyed by their load address. The dispatch function handles it transparently.

### 5c. Timing and Compatibility Fixes

AOT changes the execution timing profile. Some games are sensitive to instruction timing (tight polling loops, raster effects timed to CPU cycles). Insert cycle count bookkeeping into the translated code — the JIT already does this for Dolphin’s cycle counting, so the mechanism exists. May need per-title tuning.

### 5d. Performance Optimization

Once correct, optimize. Low-hanging fruit: replace memory access helper calls with direct loads/stores where the address provably maps to a fixed RAM region (most game code accesses main RAM at a constant BAT mapping). This is a huge speedup — going from a function call per load/store to a single `ldr` off a base pointer. Do this conservatively: only for addresses in the main RAM range, with bounds check elimination through guard page mapping.

### Deliverable

A signed, distributable iOS build of the specific title, running primarily on translated code with interpreter fallback for edge cases.

-----

## Phase 6: Tooling & Iteration Infrastructure

Runs parallel to everything above.

### Correctness Diffing Tool

Runs the same input sequence through both the interpreter and the AOT path and compares PPC register state after each block. This is the correctness oracle.

### Coverage Visualizer

Takes the trace DB and shows a color-coded map of the ROM’s code sections: translated / observed-but-not-translated / undiscovered.

### Incremental Rebuild Pipeline

When the trace DB is updated, hash each block’s PPC bytes, only retranslate changed blocks, link incrementally.

### macOS Test Harness

Run the translated code on ARM64 Mac before deploying to iOS. Same architecture, much faster iteration loop.

-----

## Effort Estimates & Risk Assessment

Assuming familiarity with C++ and Dolphin’s codebase, targeting a single well-behaved title first:

|Phase                                 |Estimated Time    |
|--------------------------------------|------------------|
|Phase 1: Instrumented Trace Collection|1–2 weeks         |
|Phase 2: CFG Construction             |2–3 weeks         |
|Phase 3: AOT Translation Backend      |2–3 months        |
|Phase 4: Dolphin Runtime Harness      |~1 month          |
|Phase 5: First Title Boot & Debug     |~1 month          |
|Phase 6: Tooling                      |Ongoing / parallel|

### Primary Risks

**Phase 3 — JIT extraction** is the biggest risk. The JIT code is deeply entangled with runtime assumptions (block cache invalidation, fastmem signal handling, lazy register allocation that assumes it can patch code after the fact). Disentangling that into a static pipeline is the hard engineering problem.

**Memory access translation** is the second biggest risk. Getting the address mapping right for every edge case (MMIO, uncached regions, locked cache) without fastmem is fiddly.

### Precedent

The core concept is proven. The N64 static recompilation community has demonstrated this approach works. The GameCube scale is larger (bigger binaries, more complex hardware), and Dolphin’s codebase is more mature — which is both a blessing (more infrastructure to reuse) and a curse (more entanglement to work around).
