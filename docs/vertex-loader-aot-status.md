# Vertex Loader AOT — Implementation Status

## What This Is

An ahead-of-time compilation pipeline for Dolphin's vertex loader system, targeting iOS where JIT is unavailable. Generates optimized C vertex loader functions from recorded vertex format configurations, compiles them with clang, and links as a static library — same approach as the existing PPC AOT pipeline.

## Current State: Compiles and links, but video output broken

The full pipeline is implemented end-to-end and builds cleanly. 21 vertex formats were successfully generated for Metroid Prime (GM8E01). However, when running with the AOT vertex loaders linked in, video output dies. The issue needs debugging — likely a mismatch between the AOT-generated `PortableVertexDeclaration` and what the runtime expects, or a bug in the generated C vertex loading code.

**One bug was found and fixed:** The `VB_HAS_*` constants in `VertexLoaderCEmitter.cpp` were wrong (e.g., `VB_HAS_NORMAL` was `1<<14` instead of `1<<10`). This caused an assertion failure on `native_components` mismatch. After fixing, the components match, but video still doesn't render.

## What Was Built

### Phase 1 — Trace Collection (complete)

**Modified files:**
- `Source/Core/Core/PowerPC/TraceCollector.h` — Bumped FORMAT_VERSION 2→3, added `vtx_format_count` to FileHeader, added `std::set<std::array<u32, 5>> m_vertex_formats`, added `RecordVertexFormat()` method
- `Source/Core/Core/PowerPC/TraceCollector.cpp` — Extended `FlushToDisk`/`MergeFromDisk`/`Clear` for vertex format records. Backward-compatible: v2 files still merge (seek-back on short header)
- `Source/Core/Core/PowerPC/JitInterface.h/cpp` — Added `RecordVertexFormat()` forwarding method so VideoCommon can reach the trace collector
- `Source/Core/VideoCommon/VertexLoaderManager.cpp` — In `GetOrCreateLoader()`, calls `RecordVertexFormat()` when a new loader is created

### Phase 2 — CFG Database (complete)

**Modified files:**
- `Source/Core/DolphinTool/CfgCommand.cpp` — Extended `ReadTraceFile()` for v2/v3 compatibility, added `vertex_formats` SQLite table to schema, added `TraceData::VertexFormat` struct, writes vertex format records to DB

### Phase 3 — C Code Generation (complete, likely has bugs)

**New files:**
- `Source/Core/DolphinTool/VertexLoaderCEmitter.h` — Class that takes 5 register values (TVtxDesc + VAT) and generates a C vertex loader function
- `Source/Core/DolphinTool/VertexLoaderCEmitter.cpp` — Parses register bitfields, computes input/output layout, emits C code for each component type:
  - Position matrix index (1 byte → u32, z-freeze cache)
  - Texture matrix indices (1 byte each, stored as locals)
  - Position (direct/indexed, UByte/Byte/UShort/Short/Float, 2-3 elements, dequantization, z-freeze, vertex skip)
  - Normal + Tangent + Binormal (NTB mode, NormalIndex3, scaling exponents)
  - Colors (RGB565, RGB888, RGB888x, RGBA4444, RGBA6666, RGBA8888 — bit manipulation)
  - Texture coordinates (1-2 elements, dequantization, texmatidx as 3rd component)
- `Source/Core/DolphinTool/VtxAotCommand.h/cpp` — `dolphin-tool vtxaot` command: reads `vertex_formats` from SQLite, generates `{prefix}_vtx_loaders.c`, `{prefix}_vtx_dispatch.c`, `vtx_aot_runtime.h`, `build_vtx.sh`

### Phase 4 — Runtime Dispatch (complete, likely has bugs)

**New files:**
- `Source/Core/VideoCommon/VertexLoaderAotRegistry.h/cpp` — Maps `(game_id, format_key)` → `{func, decl, vertex_size, components}`. Registration via `extern "C" vtx_aot_register_loader()` called from `__attribute__((constructor))` in generated code. Includes `ConvertDecl()` to convert C struct `VtxPortableDeclC` → Dolphin's `PortableVertexDeclaration`.
- `Source/Core/VideoCommon/VertexLoaderAOT.h/cpp` — `VertexLoaderBase` subclass. `RunVertices()` populates a `VtxLoaderZFreezeState` with pointers to `VertexLoaderManager`'s z-freeze caches, then calls the AOT function with `(src, dst, count, arraybases, strides, &zf)`.

**Modified files:**
- `Source/Core/VideoCommon/VertexLoaderBase.cpp` — `CreateVertexLoader()` checks AOT registry before JIT/software fallback. In Compare mode, pairs AOT vs JIT in `VertexLoaderTester`.
- `Source/Core/VideoCommon/CMakeLists.txt` — Added new source files
- `Source/Core/DolphinTool/CMakeLists.txt` — Added new source files
- `Source/Core/DolphinTool/ToolMain.cpp` — Registered `vtxaot` command

## Known Issues / What Needs Debugging

1. **Video dies when AOT vertex loaders are linked.** Even with the `VB_HAS_*` fix, something is wrong. Likely candidates:
   - **PortableVertexDeclaration mismatch**: The AOT emitter computes the declaration independently from Dolphin's runtime. If stride, offsets, or attribute formats differ from what the JIT computes, the GPU gets wrong vertex layouts. The `ConvertDecl()` in the registry or the emitter's layout computation could be wrong.
   - **Generated C code bugs**: The vertex loading logic might produce different output from the JIT. The comparison mode (`VertexLoaderType::Compare`) should catch this, but video dies before comparison can run.
   - **Z-freeze state struct layout mismatch**: The `VtxLoaderZFreezeState` in VertexLoaderAOT.cpp must match the `VtxLoaderState` typedef in the generated C header.

2. **Debugging approach**: The cleanest way to validate is to:
   - First, test with comparison mode to get byte-for-byte output comparison (fix whatever prevents video init first)
   - Or, write a standalone unit test that creates a `VertexLoaderAOT` and a `VertexLoaderARM64` for the same format, feeds them identical input, and compares output
   - Check that `VertexLoaderAOT::m_native_vtx_decl` exactly matches `VertexLoaderARM64::m_native_vtx_decl` for the same format config

3. **VAT bitfield parsing**: The `VertexLoaderCEmitter::ParseConfig()` manually extracts bitfields from the raw u32 register values. Any mistake here (wrong bit position or width) would silently produce wrong vertex data. Should be validated against Dolphin's `TVtxDesc`/`UVAT_group0/1/2` bitfield definitions in `CPMemory.h`.

## How to Test

```bash
# Regenerate (after fixing emitter bugs):
./build/Binaries/dolphin-tool vtxaot \
  --cfg build/trace_output/metroid_prime_cfg.db \
  --output build/aot_output_metroid/ \
  --prefix GM8E01

# Compile vertex loaders:
cd build/aot_output_metroid
clang -c -O2 -flto=thin -arch arm64 -I. GM8E01_vtx_loaders.c -o GM8E01_vtx_loaders.o
clang -c -O2 -flto=thin -arch arm64 -I. GM8E01_vtx_dispatch.c -o GM8E01_vtx_dispatch.o

# Add to existing PPC AOT archive:
ar rcs ../aot_output_metroid_prime/libGM8E01_aot.a GM8E01_vtx_loaders.o GM8E01_vtx_dispatch.o

# Rebuild Dolphin with AOT lib:
cd ../
cmake .. -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -DENABLE_VULKAN=OFF \
  -DENABLE_LTO=ON -DAOT_STATIC_LIB=$(pwd)/aot_output_metroid_prime/libGM8E01_aot.a
make -j$(sysctl -n hw.ncpu) dolphin-emu

# Run with comparison mode:
./Binaries/DolphinQt.app/Contents/MacOS/DolphinQt \
  -e "/path/to/Metroid Prime (USA).iso" \
  -C Dolphin.Core.CPUCore=6
```

## File Inventory

### New files (8)
```
Source/Core/DolphinTool/VertexLoaderCEmitter.h
Source/Core/DolphinTool/VertexLoaderCEmitter.cpp
Source/Core/DolphinTool/VtxAotCommand.h
Source/Core/DolphinTool/VtxAotCommand.cpp
Source/Core/VideoCommon/VertexLoaderAOT.h
Source/Core/VideoCommon/VertexLoaderAOT.cpp
Source/Core/VideoCommon/VertexLoaderAotRegistry.h
Source/Core/VideoCommon/VertexLoaderAotRegistry.cpp
```

### Modified files (16)
```
CLAUDE.md
Source/Core/Core/PowerPC/AOTCore.cpp          (pre-existing changes, not from this work)
Source/Core/Core/PowerPC/AotRuntime.cpp       (pre-existing changes, not from this work)
Source/Core/Core/PowerPC/JitInterface.cpp
Source/Core/Core/PowerPC/JitInterface.h
Source/Core/Core/PowerPC/TraceCollector.cpp
Source/Core/Core/PowerPC/TraceCollector.h
Source/Core/DolphinTool/AotCEmitter.cpp       (pre-existing changes, not from this work)
Source/Core/DolphinTool/AotCEmitter.h         (pre-existing changes, not from this work)
Source/Core/DolphinTool/AotCommand.cpp        (pre-existing changes, not from this work)
Source/Core/DolphinTool/CMakeLists.txt
Source/Core/DolphinTool/CfgCommand.cpp
Source/Core/DolphinTool/ToolMain.cpp
Source/Core/VideoCommon/CMakeLists.txt
Source/Core/VideoCommon/VertexLoaderBase.cpp
Source/Core/VideoCommon/VertexLoaderManager.cpp
```

### Generated output (for Metroid Prime, in build/aot_output_metroid/)
```
vtx_aot_runtime.h           — Runtime header with helpers and type definitions
GM8E01_vtx_loaders.c        — 21 vertex loader C functions (~39KB)
GM8E01_vtx_dispatch.c       — Lookup table + __attribute__((constructor)) registration
build_vtx.sh                — Compilation script
```
