# Dolphin AOT Recompiler — Status Update #2 (2026-04-02)

## Summary

The AOT codegen is now **functionally correct** for CPU operations. A block-level diffing harness (`dolphin-tool diff`) validated 51,000+ blocks with **0 divergences** between AOT and the interpreter. Five critical codegen bugs were found and fixed. However, the game still produces **no video or audio output** — the CPU executes correctly but rendering never appears on screen. The next investigation should focus on the video/audio output path.

## Branch

`instrumentation` — 47 commits ahead of `master`

## Codegen Bugs Found & Fixed

### 1. CR Lookup Table (CRITICAL)
`aot_cr_table[16]` in `AotCommand.cpp` had completely wrong values. Every comparison instruction (cmp/cmpi/cmpl/cmpli/fcmp) wrote corrupt CR fields, causing all conditional branches to take wrong paths. Fixed with correct values from `ConditionRegister::PPCToInternal()`.

### 2. CR Logical Opcode Mapping (CRITICAL)
All 8 CR logical opcodes were mapped to wrong instruction numbers. `crxor` (used by the `crclr` mnemonic) was mapped to `crnor`'s opcode (193 vs 385), `crand` was in `crxor`'s slot, etc. Fixed all mappings: crnor=33, crandc=129, crnand=225, crand=257, creqv=289, crxor=385, crorc=417, cror=449.

### 3. mtmsr Not Advancing PC
`EmitMtmsr` emitted `aot_mtmsr(s, val); return;` which exited the block function before the fallthrough epilogue could set `s->pc`. PC was left at the block start address. Fixed by setting `s->pc` and `s->downcount` before calling `aot_mtmsr`.

### 4. subfze Missing Bitwise NOT
`subfze` (subtract from zero extended) was mapped to `EmitAddzex` which computes `rA + carry`. The correct semantics are `~rA + carry`. Added `EmitSubfzex` with the `~` operator.

### 5. FPU Availability Not Checked
AOT FP instructions executed without checking `MSR.FP`. The GameCube OS temporarily clears `MSR.FP` for lazy FPU context switching. Added `aot_check_fpu()` runtime helper called before every FP/PS instruction, which triggers `EXCEPTION_FPU_UNAVAILABLE` when FP is disabled.

## Other Runtime Fixes

### Exception Handling in Dispatch Loop
- Added `npc = pc` sync before `core_timing.Advance()` so `CheckExternalExceptions()` saves the correct return address into SRR0
- Added `CheckExceptions()` after each `m_dispatch()` call in `AOTCore::Run()` to catch DSI/PROGRAM exceptions

### aot_rfi / aot_msr_updated Incomplete
Both were calling the full `PowerPCManager::MSRUpdated()` which includes `JitInterface::UpdateMembase()` → `UpdateDBATMappings()` — a 32K-entry BAT array rebuild. The AOT doesn't use the JIT's membase pointer (it accesses RAM via `GetRAMPtr()` directly). Fixed to only update feature flags and page tables, skipping the expensive BAT remapping. This was consuming 99.6% of CPU time (confirmed via profiler/`atos` → `Memory::MemoryManager::UpdateDBATMappings`).

### aot_mtmsr Runtime Function
New `aot_mtmsr()` that sets MSR, calls lightweight `aot_msr_updated()`, then `CheckExceptions()`. The emitter uses this with `return` to properly end the block.

## Diff Harness: `dolphin-tool diff`

A block-level AOT-vs-interpreter comparison tool. New files:
- `Source/Core/DolphinTool/DiffCommand.{h,cpp}` — CLI entry, boots emulator headlessly
- Comparison logic in `AOTCore::RunDiff()`

### Usage
```bash
dolphin-tool diff \
  --iso /path/to/game.iso \
  --cfg /path/to/cfg.db \
  --max-divergences 5 \
  --max-blocks 200000
```

### Architecture
- **Single-block comparison**: Uses `GALE01_lookup_block(pc)` to get individual AOT block function pointers. `aot_single_block_mode` flag prevents dispatch from chaining. Downcount trick (`downcount = num_instr`) prevents direct tail calls.
- **Full RAM snapshot/restore**: 24MB save before AOT, 24MB restore before interpreter. Required for correctness (AOT and interpreter must see identical memory).
- **MMIO detection**: Scans block instructions from RAM (not iCache) for `lis rN, 0xCC00` patterns and timebase/decrementer SPR reads. MMIO blocks run through AOT single-block without comparison.
- **Validation skip**: After 100 successful comparisons, blocks run through full AOT dispatch without comparison. Self-looping blocks (PC == block start after comparison) are immediately validated.
- **Signal handling**: SIGINT/SIGTERM properly shut down via `Core::QueueHostJob` + `cpu.Break()`.

### Key Design Decisions
- Interpreter path can deadlock on certain memory reads in single-block context. All non-comparison paths (validated blocks, MMIO blocks, filtered blocks) use AOT execution.
- Validated blocks use full `m_dispatch()` (not single-block mode) so polling loops can burn downcount naturally and exit when hardware events fire via `Advance()`.
- Config uses `Config::SetCurrent` (not `SetBaseOrCurrent`) to avoid persisting diff settings to `Dolphin.ini`.

## Current State: What Works, What Doesn't

### Works
- Full pipeline: trace → CFG → translate → compile → link → AOT core runs
- CPU execution is correct: 51,000+ blocks verified with 0 divergences
- AOT core initializes, dispatches blocks, handles exceptions
- Interpreter fallback for untranslated blocks via `aot_interpreter_single_step()`
- Status bar shows "AOT SC"
- CPU runs at reasonable speed (BAT remapping bottleneck fixed)

### Doesn't Work
- **No video output** — black screen with real video backend
- **No audio output**
- Game eventually crashes with "Invalid read from 0xffffffff, PC = 0x80003238" — a memcpy loop reading past a buffer, suggesting some game state corruption from timing differences or an undiscovered codegen edge case

### Key Observation
The CachedInterpreter (CPUCore=5) **does** render correctly for the same game. The AOT core's CPU execution is verified correct at the instruction level. The problem is likely in how the AOT core interacts with Dolphin's video/audio subsystems — not in the PPC instruction translation itself.

## Hypotheses for No Video/Audio

1. **GPU command processor not being fed**: The game writes display lists to the GP FIFO via stores to `0xCC008000`. These writes go through `aot_write_u32` → slow path → `PowerPC::WriteFromJit` → MMIO handler → GPU command processor. If these writes aren't reaching the GPU, no rendering happens. Check if `ProcessFifoEvents` is being called.

2. **VI (Video Interface) interrupts not firing correctly**: The VI generates interrupts at vsync to signal frame completion. These go through `ProcessorInterface::SetInterrupt(INT_CAUSE_VI)` → `CheckExternalExceptions()` → exception handler at 0x500. If the interrupt handler doesn't run properly, the game's main loop stalls.

3. **Timing mismatch**: The AOT decrements downcount by instruction count, but the interpreter uses cycle counts from `PPCTables::GetOpInfo()`. If downcount runs down faster/slower than expected, hardware events fire at wrong times, causing the game to miss vsync or audio DMA deadlines.

4. **GP FIFO gather pipe not flushed**: The GameCube uses a write-gather pipe at 0xCC008000 that buffers GPU commands. Dolphin tracks this via `gather_pipe_ptr` in PowerPCState. The AOT doesn't update `gather_pipe_ptr` — if the gather pipe isn't being flushed, GPU commands accumulate but never execute.

5. **Video backend initialization**: The AOT core might not properly trigger video backend setup that the JIT/CachedInterpreter does during boot.

## Build Instructions

Same as before (see `AOT_STATUS.md`), with one addition for the diff tool:

```bash
# Run the diff harness
./Binaries/dolphin-tool diff \
  --iso "/path/to/game.iso" \
  --cfg trace_output/melee_cfg.db \
  --max-divergences 5 \
  --max-blocks 200000
```

The AOT library must be compiled with the latest `dolphin-tool translate` output (CR table fix, mtmsr fix, CR logical fix, subfze fix, FPU check all affect generated code).

## File Map (New/Modified)

```
Source/Core/Core/PowerPC/
  AOTCore.{h,cpp}            — RunDiff(), single-block comparison, MMIO detection
  AotRuntime.cpp             — aot_mtmsr(), aot_check_fpu(), lightweight MSR updates

Source/Core/DolphinTool/
  DiffCommand.{h,cpp}        — dolphin-tool diff subcommand
  AotCommand.cpp             — Fixed CR table, added lookup_block + single_block_mode
  AotCEmitter.{h,cpp}        — Fixed CR logical opcodes, subfze, mtmsr, FPU check
  ToolMain.cpp               — Registered diff command
  CMakeLists.txt             — Added DiffCommand sources

Source/Core/Core/Config/
  MainSettings.{h,cpp}       — Added diff mode config entries
```

## Commit History (recent, on `instrumentation` branch)

```
487123aeaa Use Config::SetCurrent instead of SetBaseOrCurrent in diff tool
f6010261da Fix progress reporting: count all block visits, not just comparisons
a45a483fb9 Skip expensive BAT remapping in AOT MSR updates
ce121746a1 Add FPU availability check before all FP/PS instructions
d6a5ad0cac Fix subfze instruction: was using addze logic (missing NOT)
494084162b Fix CR logical opcode mapping and filter timebase divergences
3ac22bbe08 Fix single-block comparison and mtmsr PC advancement
1d4a51b415 Fix critical AOT codegen bugs and add block-level diff harness
```
