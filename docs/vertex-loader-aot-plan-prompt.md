# Vertex Loader AOT: Plan Agent Prompt

Investigate and design an ahead-of-time compilation pipeline for Dolphin's vertex loader system, targeting iOS where JIT is unavailable. The goal is to eliminate the ~20x performance penalty of the software vertex loader fallback on iOS by transpiling the vertex loader JIT logic to C, compiling with clang, and linking as a static library — the same approach used by the existing PPC AOT pipeline.

## Background

The vertex loader converts GC/Wii vertex data (big-endian, mixed integer/float/fixed-point formats) into a GPU-friendly format. It runs on **every draw call**, making it one of the hottest paths in the renderer. Each unique vertex format (determined by hardware register state) gets a specialized loader.

On macOS/desktop, `VertexLoaderARM64` JIT-compiles tight ARM64 machine code for each format at runtime — fast, but requires executable memory allocation. On iOS, this JIT is disabled (`#if !TARGET_OS_IOS` guard), forcing the slow `VertexLoader` software fallback which uses a pipeline of function pointer calls. This is ~20x slower than the JIT and is the suspected cause of single-digit FPS on iOS for geometry-heavy games like Metroid Prime, despite the PPC AOT core running well.

Games use only **5-30 unique vertex formats**, each producing ~4KB of machine code. The format space is finite and discoverable per-game via profiling. This makes AOT compilation practical with minimal binary size overhead.

## Approach: C Codegen (NOT binary blob extraction)

The approach must be C transpilation — generate C source code that implements each vertex loader, compile it with clang, and link as a static library. Do NOT extract ARM64 binary blobs. This matches the existing PPC AOT pipeline's architecture and ensures portability.

The existing `VertexLoaderARM64.cpp` is the **correctness reference** for what operations each loader must perform, but the output should be C functions, not ARM64 assembly.

## Key Files to Study

### Vertex Loader Architecture
- `Source/Core/VideoCommon/VertexLoaderARM64.cpp` (534 lines) — ARM64 JIT codegen. This is the reference for what each loader does: byte swapping, format conversion, dequantization, indexed lookups, Z-freeze caching. Study this to understand every operation that must be replicated in C.
- `Source/Core/VideoCommon/VertexLoaderARM64.h` — JIT class interface, inherits VertexLoaderBase.
- `Source/Core/VideoCommon/VertexLoaderX64.cpp` (612 lines) — X64 JIT, useful as a second reference.
- `Source/Core/VideoCommon/VertexLoaderBase.h` — Abstract interface. `RunVertices(count, src, dst)` is the entry point. `CreateVertexLoader()` factory method.
- `Source/Core/VideoCommon/VertexLoaderBase.cpp` — Loader factory. Line 29: iOS guard that disables JIT. Line 236: `CreateVertexLoader()` selects JIT vs software.
- `Source/Core/VideoCommon/VertexLoader.cpp` (275 lines) — Software fallback. `CompileVertexTranslator()` builds the function pointer pipeline. `RunVertices()` iterates pipeline stages per vertex. This is what we're replacing.

### Vertex Format Configuration
- `Source/Core/VideoCommon/CPMemory.h` — `TVtxDesc` (vertex descriptor: which components are present, direct vs indexed), `UVAT_group0/1/2` (VAT: component formats, element counts, scale factors). These 5 register values fully determine a loader.
- `Source/Core/VideoCommon/VertexLoaderManager.h` — `VertexLoaderUID` (hash of the 5 register values), used as cache key.
- `Source/Core/VideoCommon/VertexLoaderManager.cpp` — `s_vertex_loader_map` (line 50): runtime cache of loaders keyed by UID. `GetOrCreateLoader()` (line 217): lookup or create. `RunVertices()` (line 399): main entry point per draw call.

### Component Loaders (software reference implementations)
- `Source/Core/VideoCommon/VertexLoader_Position.cpp` — Position loading (direct/indexed, u8/s8/u16/s16/float, 2-3 elements, dequantization)
- `Source/Core/VideoCommon/VertexLoader_Normal.cpp` — Normal loading (1 or 3 normals, s8/s16/float)
- `Source/Core/VideoCommon/VertexLoader_Color.cpp` — Color loading (RGB565, RGB888, RGBA4444, RGBA6666, RGBA8888)
- `Source/Core/VideoCommon/VertexLoader_TextCoord.cpp` — Texture coordinate loading (1-2 elements, multiple formats)

### Existing PPC AOT Pipeline (pattern to follow)
- `Source/Core/DolphinTool/AotCommand.cpp` — Orchestrator: reads trace data, generates C files, writes build script, generates dispatch/header
- `Source/Core/DolphinTool/AotCEmitter.cpp` — PPC-to-C emitter (pattern for the vertex-to-C emitter)
- `Source/Core/Core/PowerPC/AotRuntime.cpp` — Runtime helpers called by generated C code
- `Source/Core/Core/PowerPC/AotRegistry.h` — Multi-game registry pattern (reuse for vertex loaders)

## Design the Pipeline in 3 Phases

### Phase 1 — Trace: Record Vertex Format UIDs

Instrument `VertexLoaderManager::GetOrCreateLoader()` to record every unique `VertexLoaderUID` encountered during gameplay. Each UID encodes the 5 register values (TVtxDesc.low, TVtxDesc.high, UVAT_group0, UVAT_group1, UVAT_group2) that fully determine a loader.

Output: a file listing all unique format configurations with their register values. This is analogous to the PPC trace phase that records executed block addresses.

Consider whether this can piggyback on the existing PPC trace collection phase (same gameplay session) or needs a separate pass.

### Phase 2 — Translate: Emit C Vertex Loaders

For each recorded format configuration, generate a C function that:
1. Reads vertex components from a source buffer (big-endian GC format)
2. Performs byte swapping, format conversion (int→float, fixed-point dequantization), and indexed array lookups as needed
3. Writes converted vertex data to a destination buffer in the format expected by the rendering backend

The C emitter should be driven by the same register values that drive the JIT. Study `VertexLoaderARM64::GenerateVertexLoader()` to understand the exact sequence of operations for each component type and format combination. The generated C should:
- Use the same byte-swap and format conversion logic
- Handle all component types: position, normal, color (5 formats), texcoord, matrix indices
- Handle direct vs Index8 vs Index16 data sourcing
- Apply dequantization scale factors from the VAT registers
- Handle Z-freeze (position/normal caching for line/point rendering)

Output: C source files + header with function signatures, analogous to the PPC block files.

### Phase 3 — Runtime: Dispatch to Pre-compiled Loaders

On iOS, when `VertexLoaderManager::GetOrCreateLoader()` would normally create a software `VertexLoader`, instead look up the UID in a registry of pre-compiled AOT loaders. If found, return the AOT version; if not found, fall back to the software interpreter (graceful degradation).

Integration approach:
- Create a `VertexLoaderAOT` class inheriting `VertexLoaderBase` that wraps a pre-compiled C function
- Register AOT loaders via constructor (`__attribute__((constructor))`) like the PPC AOT registry
- Modify `CreateVertexLoader()` in `VertexLoaderBase.cpp` to check the AOT registry before falling back to software

## Constraints

- **C codegen only** — no binary blob extraction, no runtime code generation
- **Must work on iOS** — no MAP_JIT, no writable+executable pages
- **Per-game libraries** are acceptable (same model as PPC AOT: `libGALE01_vtx.a`)
- **Correctness reference** is `VertexLoaderARM64.cpp` — the generated C must produce identical output
- **Integrate with existing build** — `DOLPHIN_IOS_BUILD=ON`, Delta emulator frontend
- The existing `VertexLoaderTest.cpp` has tests that can validate correctness
