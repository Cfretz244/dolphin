# Dolphin AOT Recompiler — Status Update #3 (2026-04-02)

## Summary

Three critical runtime/codegen bugs were found and fixed. The game now **plays recognizable audio** (SSBM opening title music) and produces **partial video output** (garbled colors, eventually green). This is the first time the AOT core has produced meaningful game output.

## Branch

`instrumentation` — 48 commits ahead of `master`

## Bugs Found & Fixed This Session

### 1. Idle Loop Detection (CRITICAL — AOTCore.cpp)

The `AOTCore::Run()` loop had an 8-slot PC history that detected "idle loops" by checking if any dispatch-return PC appeared twice. This was catastrophically aggressive — normal game loops (any subroutine call returning to the same call site) triggered it. When triggered, `downcount` was set to 0, but `CoreTiming::Advance()` consumed the full timeslice worth of ticks:

```
cyclesExecuted = slice_length - DowncountToCycles(0) = slice_length
```

The game made almost no progress per timeslice while `global_timer` advanced at full speed. Boot/initialization never completed.

**Fix:** Replaced with simple `do { dispatch } while (downcount > 0)` loop matching the CachedInterpreter.

### 2. `sc` Instruction Missing npc (CRITICAL — AotCEmitter.cpp)

The `sc` (syscall) emitter generated:
```c
aot_sc(s); return;
```

Without setting `s->pc` or `s->npc` first. `CheckExceptions()` for EXCEPTION_SYSCALL saves `SRR0 = npc`, so SRR0 got a stale value. The syscall handler's `rfi` then jumped to the wrong address — typically back to the exception vector (0xC00), creating an infinite loop.

**Fix:** Emitter now generates:
```c
s->downcount -= N; s->pc = PC+4; s->npc = s->pc; aot_sc(s); return;
```

### 3. `mftb` SPR Decode Bug (CRITICAL — AotRuntime.cpp)

`aot_mftb()` had two bugs:
1. It always read `SPR_TL` regardless of whether the instruction was `mftbl` or `mftbu`
2. It didn't decode the PPC SPR field (which has upper/lower 5-bit halves swapped)

The raw `inst.SPR` field for TBL=268 encodes as 392, and for TBU=269 encodes as 424. Passing these directly to `aot_mfspr_special` missed the `case SPR_TL/SPR_TU` handlers, returning uninitialized SPR values (0).

**Result:** Every timebase read returned 0, corrupting all timing-dependent game logic (frame pacing, random seeds, synchronization).

**Fix:**
```cpp
uint32_t aot_mftb(AOTState* s, uint32_t spr_encoded)
{
  u32 spr = ((spr_encoded & 0x1F) << 5) | ((spr_encoded >> 5) & 0x1F);
  return aot_mfspr_special(s, spr);
}
```

### 4. Missing SPR Handlers (MODERATE — AotRuntime.cpp)

`aot_mtspr_special` was ignoring writes to several important SPRs:

| SPR | Fix | Why It Matters |
|-----|-----|----------------|
| WPAR | `GPFifo::ResetGatherPipe()` | Clears partial gather pipe data on redirect |
| HID0 | ICFI cache flush | Instruction cache invalidation |
| HID2 | Read-only bit masking | Preserves hardware-protected fields |
| HID4 | `IBATUpdated() + DBATUpdated()` | BAT translation table changes |

## Diff Harness Improvements

The block-level diff tool (`dolphin-tool diff`) was significantly enhanced:

### Full Hardware State Save/Restore for MMIO Blocks
MMIO-accessing blocks (stores/loads to 0xCC000000+ hardware registers) previously skipped comparison entirely. Now uses Dolphin's `DoState` serialization (`HW::DoState`, `PowerPC::DoState`, `CoreTiming::DoState`) to save and restore the complete emulated hardware state between AOT and interpreter runs. This eliminates false positives from non-deterministic MMIO reads.

### MMIO Write Capture
New `MMIOCapture.h` provides a global write log hooked into `MMU::WriteToHardware`. Captures all writes to hardware registers (0x08000000+ physical) and GP FIFO (0x0C008000). Both AOT and interpreter MMIO writes are captured and compared per-block.

### RAM Comparison
Non-MMIO blocks now compare full 24MB RAM contents between AOT and interpreter results, catching memory write divergences that register-only comparison missed.

### Timebase Block Handling
Blocks containing `mftb`/`mftbu`/`mftbl` or `mfspr TL/TU/DEC` are correctly identified and skipped. These are inherently non-comparable because `GetFakeTimeBase()` depends on `downcount` which is consumed at different rates by AOT vs interpreter within a single block.

### Other Improvements
- Validation-skip threshold removed (every block compared every time)
- Self-loop optimization: compare once, then interpreter runs remaining iterations
- VI XFB address monitoring in periodic progress output
- Exception field added to snapshot comparison
- Per-block status printed to stderr

## Current State

### Works
- Full boot sequence completes
- Game enters main loop, plays opening title music correctly
- VI interrupts fire and are handled (~60Hz)
- Syscalls work (sc/rfi cycle completes)
- Timebase reads return correct TBU/TBL values
- `AOT_INTERP_ONLY=1` mode renders full video+audio at ~4fps (proves infrastructure is correct)

### Partially Works
- Video output: garbled (random colors, flashing, eventually solid green)
- GPU commands appear to be partially reaching the video backend

### Known Issues
- `Invalid write to 0xe0000000, PC = 0x8034308c` — locked L1 cache not enabled. Game continues after crash.
- Video corruption — likely remaining codegen bug in GPU command generation or MMIO register writes
- Diff harness shows one unresolved MMIO divergence at block 0x80348204 (interpreter doesn't execute final `bl` instruction in an MMIO block — under investigation)

## How to Test

```bash
cd build

# Normal AOT mode
./Binaries/DolphinQt.app/Contents/MacOS/DolphinQt \
  -e "/path/to/game.iso" \
  -C Dolphin.Core.CPUCore=6

# Interpreter-only mode (slow but correct — proves infrastructure works)
AOT_INTERP_ONLY=1 ./Binaries/DolphinQt.app/Contents/MacOS/DolphinQt \
  -e "/path/to/game.iso" \
  -C Dolphin.Core.CPUCore=6

# Diff harness (finds codegen bugs)
./Binaries/dolphin-tool diff \
  --iso "/path/to/game.iso" \
  --cfg trace_output/melee_cfg.db \
  --max-divergences 5 \
  --max-blocks 0
```

## Build Instructions

Same as `AOT_STATUS_2.md`, with regeneration required for the `sc` emitter fix:

```bash
cd build

# Regenerate AOT code (sc fix is in emitter)
rm -rf aot_output
./Binaries/dolphin-tool translate \
  --cfg trace_output/melee_cfg.db \
  --iso "/path/to/game.iso" \
  --output aot_output/

# Compile AOT library
cd aot_output
for f in GALE01_*.c; do
  clang -c -O2 -arch arm64 -I. "$f" -o "${f%.c}.o" &
done
wait
ar rcs libGALE01_aot.a GALE01_*.o
cd ..

# Reconfigure and rebuild
cmake .. -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DAOT_STATIC_LIB=$(pwd)/aot_output/libGALE01_aot.a
make -j$(sysctl -n hw.ncpu) dolphin-emu
```

If only `AotRuntime.cpp` or `AOTCore.cpp` changed (no emitter changes):
```bash
make -j$(sysctl -n hw.ncpu) dolphin-emu
```

## File Map (New/Modified This Session)

```
Source/Core/Core/PowerPC/
  AOTCore.cpp              — Run() loop rewrite, diff harness improvements
  AOTCore.h                — m_interp_dispatch, m_state_buffer, BlockReadsTimebase
  AotRuntime.cpp           — mftb fix, SPR handlers (WPAR, HID0, HID2, HID4)
  MMIOCapture.h            — NEW: global MMIO write capture for diff harness
  MMU.cpp                  — MMIO capture hooks in WriteToHardware

Source/Core/DolphinTool/
  AotCEmitter.cpp          — sc instruction npc/pc/downcount fix
```

## Next Steps

1. **Investigate video corruption** — GPU commands reach the backend but are garbled. Could be:
   - Remaining codegen bug in blocks that write to GP FIFO (0xCC008000)
   - Byte order issue in GPU command data
   - Missing or incorrect paired-singles quantized store handling
   - A block not in the AOT table that handles GPU setup via interpreter fallback incorrectly

2. **Investigate locked cache crash** — `0xE0000000` write fails because locked cache isn't set up. May need to handle `dcbz_l` properly or check HID2 WPE (Write Pipe Enable) bit.

3. **Continue diff harness investigation** — the MMIO block divergence at 0x80348204 needs resolution. May reveal the source of video corruption.

4. **Performance** — once correctness is achieved, measure AOT vs CachedInterpreter speed.
