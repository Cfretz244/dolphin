# GCDeltaCore Implementation Log

## What Was Done

### Goal
Integrate Dolphin's GameCube emulator as a new core in the Delta iOS emulator, following the same plugin pattern used by existing cores (N64/mupen64plus, GBA/VBA-M, etc.).

### Architecture
- **GCDeltaCore** is a standalone git repo with Dolphin as a nested submodule
- Added as a submodule to Delta under `Cores/GCDeltaCore/`
- Follows Delta's `DeltaCoreProtocol` + `EmulatorBridging` pattern
- Bridge uses semaphore-gated frame synchronization via Dolphin's `vi_end_field_event` (same pattern as N64DeltaCore)

### What Was Built

#### GCDeltaCore repo structure:
```
GCDeltaCore/
  dolphin/                    # Git submodule (with iOS patches)
  GCDeltaCore/
    GC.swift                  # DeltaCoreProtocol (48kHz audio, 640x480, 22 inputs)
    Bridge/
      GCEmulatorBridge.mm     # Main bridge: boot, frame sync, input, saves
      GCEmulatorBridge.h
      DolphinIOSStubs.mm      # Stubs for macOS-only APIs
      DolphinStateHelper.cpp  # Isolates Dolphin's State namespace from ObjC
    Host/DeltaHost.cpp        # Host.h callback stubs
    Audio/DeltaSoundStream.*  # Custom SoundStream → Delta's RingBuffer
    Types/GCTypes.h/m         # GameType/CheatType constants
    Controller Skin/          # GameCube on-screen layout (portrait+landscape)
    Standard.deltamapping     # Physical controller mapping
  scripts/
    ios-arm64-toolchain.cmake
    build-dolphin-ios.sh
  build-ios/                  # 37 pre-built static libraries
  project.yml                 # XcodeGen spec
```

#### Delta integration:
- `System.swift`: Added `.gc` case with full metadata
- `AppDelegate.swift`: GC core registered in BETA builds
- `DeltaCoreProtocol+Delta.swift`: Dolphin metadata
- `Info.plist`: File type registration (.iso, .gcm, .gcz, .rvz, .ciso, .wbfs)
- `Delta.xcworkspace`: GCDeltaCore.xcodeproj added
- `project.pbxproj`: GCDeltaCore.framework linked + embedded
- Exhaustive switch cases updated across 4 Swift files
- Bundle ID changed to `com.christopherfretz.DeltaDev` for dev builds

### Dolphin Patches (within submodule)

#### iOS build support:
- `CMakeLists.txt`: `DOLPHIN_IOS_BUILD` flag guards curl, libusb, hidapi
- `Common/CMakeLists.txt`: Guards AppKit, AGL, watcher, HttpRequest
- `InputCommon/CMakeLists.txt`: Guards Quartz input, LibUSB link
- `InputCommon/GCAdapter.cpp`: Check `__LIBUSB__` instead of just `!ANDROID`

#### Metal backend iOS fixes:
- `MTLMain.mm`: Accept `WindowSystemType::IOS`, guard `#if TARGET_OS_OSX`
- `MTLUtil.h`: Guard `AppKit/NSScreen.h` include
- `MTLUtil.mm`: Guard NSScreen HDR detection
- `MTLGfx.mm`: Guard `setDisplaySyncEnabled` (macOS-only)

#### Memory/JIT:
- `MemoryUtil.cpp`: iOS `pthread_jit_write_protect_np` bypass, `TARGET_OS_IOS` guard for Security framework translocation
- `WindowSystemInfo.h`: Added `WindowSystemType::IOS` enum
- `GCPad.h/cpp`: `SetExternalProvider()` for Delta input injection
- `ControllerInterface.h`: Added `ForceSetInit()` for iOS

#### iOS-compatible stubs (in source files):
- `HttpRequest.cpp`: Full stub implementation when `TARGET_OS_IOS`
- `FilesystemWatcher.cpp/.h`: Stub when `TARGET_OS_IOS` (no FSEventStream)
- `FileUtil.cpp`: Guard macOS Security framework translocation check

### CMake Build
- Successfully cross-compiles 37 static libraries for `arm64-apple-ios16.0`
- Total ~30MB of static libraries (libcore.a = 11MB)
- Build script: `scripts/build-dolphin-ios.sh` with all required flags
- Key flags: `-DDOLPHIN_IOS_BUILD=ON -DENABLE_CUBEB=OFF -DENABLE_SDL=OFF`

---

## What Worked

### Compilation & Linking ✅
- Delta builds successfully (`BUILD SUCCEEDED`) with GCDeltaCore integrated
- All 37 Dolphin static libraries compile for ARM64 iOS
- GCDeltaCore.framework compiles and links in Xcode
- App installs and launches on device

### Controller skin ✅
- `.deltaskin` ZIP format with CoreGraphics-generated PDFs loads correctly
- `Updated default skin (com.delta.gc.standard) for system: gc`

### Dolphin subsystem initialization ✅
- `Config::Init()` + `Config::AddLayer(GenerateBaseConfigLoader())` + `SConfig::Init()`
- `File::GetBundleDirectory()` returns valid path on iOS
- `Pad::Initialize()` + `ForceSetInit()` satisfies controller interface assertions
- `BootParameters::GenerateFromFile()` successfully parses ISO files
- `BootManager::BootCore()` successfully starts the emulation

### Dolphin boot sequence ✅
- All core threads spawn: CPU-GPU thread, DVD thread, Memcard thread, Savestate Worker, AsyncShaderCompiler, Analytics, Play Time Tracker, Mach exception thread, Asset Loaders
- `SConfig::SetRunningGameMetadata()` reads game metadata from disc
- Video backend (Null) initializes successfully
- The emulation loop reaches `JitArm64::Run()` — GameCube code begins executing

### ARM64 JIT code generation ✅ (partial)
- `AllocateExecutableMemory()` succeeds with `PROT_RWX` (no MAP_JIT) — 256MB allocated
- JIT generates ARM64 machine code and writes it to the code buffer
- The JIT code buffer has dirty pages (code was written)
- `JitArm64::GenerateAsm()` completes and `JitArm64::Run()` is called

---

## What Didn't Work

### JIT Execution ❌ — The blocking issue

**The fundamental problem:** iOS 26 does not allow executing JIT-compiled code under any configuration we tried.

#### Attempts:

1. **`mmap(PROT_RWX)` without `MAP_JIT`** — mmap succeeds, but pages are `rw-` (no execute). `memory region` in lldb confirmed: `[0x...] rw-`. Code written but execution faults with `EXC_BAD_ACCESS (code=2)`.

2. **`mmap(PROT_RWX | MAP_JIT)`** — Fails with `errno=1 (EPERM)` for any size (tried 256MB, 32MB). Even with debugger attached and `get-task-allow=true`.

3. **`mmap(PROT_RW | MAP_JIT)`** — Also fails with EPERM.

4. **Entitlements tried:**
   - `com.apple.security.cs.allow-jit` — macOS-only, no effect on iOS
   - `com.apple.developer.kernel.increased-memory-limit` — provisioning profile issue, needs Apple Developer portal setup
   - `com.apple.developer.kernel.extended-virtual-addressing` — same
   - `get-task-allow` — present in debug builds, doesn't help

5. **`CS_DEBUGGED` flag** — Running from Xcode debugger should set this flag which historically enabled JIT on iOS. On iOS 26, this appears insufficient for `MAP_JIT`.

#### Analysis:
- iOS 26 (beta) appears to have tightened JIT restrictions compared to earlier iOS versions
- `MAP_JIT` is categorically denied with EPERM regardless of allocation size
- Without `MAP_JIT`, pages allocated as RWX have execute permission stripped by the kernel
- The `extended-virtual-addressing` and `increased-memory-limit` entitlements require explicit provisioning profile capabilities from the Apple Developer portal
- Even with a paid developer account, these may not be sufficient for MAP_JIT on iOS 26

### Video rendering ❌ (not reached)
- Used Null backend to get past video init
- Never reached the point of actual frame rendering
- Metal backend would need a CAMetalLayer surface passed via WindowSystemInfo

### Fastmem arena ❌ (non-blocking)
- `Memory::InitFastmemArena(): Failed finding a memory base.`
- iOS can't allocate the large contiguous virtual address region Dolphin needs
- Workaround: `Config::MAIN_FASTMEM=false` + `Config::MAIN_FASTMEM_ARENA=false`
- JIT falls back to slower page-mapped memory access

---

## Lessons Learned

### iOS JIT is the critical path
The entire feasibility of running Dolphin on iOS hinges on JIT execution. Without JIT, the CachedInterpreter is the only option, and it's likely too slow for GameCube emulation at playable speeds. The JIT entitlement situation on iOS 26 is murky — it may require specific entitlements that aren't yet documented for the iOS 26 beta, or Apple may have intentionally restricted this.

### Other iOS emulators that use JIT (Delta's N64 core, PPSSPP, etc.) work because:
- They may use AltStore/TrollStore which bypass code signing
- They may target older iOS versions where `CS_DEBUGGED` was sufficient
- They may use different JIT allocation strategies

### Dolphin's architecture is actually iOS-ready
The core emulation separates cleanly from the UI. The `Host.h` interface, the `WindowSystemInfo` abstraction, and the config system all work. The Metal backend has `TARGET_OS_IOS` support paths. The ARM64 JIT is production-quality.

### The initialization chain is deep
Getting from `BootManager::BootCore()` to actual code execution required fixing ~10 sequential issues:
1. Config layer not created → null pointer in `Config::SetBaseOrCurrent`
2. SConfig not initialized → crash in `SetPathsAndGameMetadata`
3. `GetBundleDirectory()` calling macOS Security translocation API → null function pointer
4. `ControllerInterface::IsInit()` assertion → needed `ForceSetInit()`
5. `ControllerInterface::RefreshDevices()` → Quartz input backend crash
6. Controller skin not a ZIP → app crash on startup
7. Controller skin PDFs invalid → CoreGraphics error
8. Video backend "OGL" → no rendering surface → switched to "Null"
9. JIT memory allocation → MAP_JIT EPERM
10. Fastmem arena failure → disabled via config

### DeltaCore's "State" ObjC enum conflicts with Dolphin's "State" C++ namespace
Required isolating Dolphin's State API calls into a separate `.cpp` file (`DolphinStateHelper`) that doesn't include any ObjC headers.

### UICommon is deeply tied to macOS
Almost every function in UICommon.cpp uses IOKit, AppKit, or other macOS-only frameworks. A complete stub replacement was needed rather than trying to conditionally compile individual functions.

### `pthread_jit_write_protect_np` needs careful handling
Apple's SDK marks it `API_UNAVAILABLE(ios)` even though it exists at runtime. Our stub must use `dlsym(RTLD_NEXT)` to find the real implementation when MAP_JIT is in use, or be a no-op when MAP_JIT is not used.

---

## Possible Next Steps

### For JIT:
1. **Try on iOS 18.x** — The JIT restrictions may be specific to iOS 26 beta
2. **Use AltStore/TrollStore** — These bypass code signing and enable JIT
3. **Investigate `csops()` syscall** — Some apps use this to check/set CS flags
4. **Check if Apple added a new entitlement** — iOS 26 may have a new JIT-specific entitlement
5. **Contact Apple DTS** — Ask about the correct entitlement for MAP_JIT on iOS 26

### For CachedInterpreter fallback:
- Set `Config::MAIN_CPU_CORE = PowerPC::CPUCore::CachedInterpreter`
- Include `Core/PowerPC/PowerPC.h` (needs MachineContext.h iOS fix)
- Will be very slow but proves the full pipeline works

### For video:
- Switch from Null to Metal backend
- Pass a `CAMetalLayer*` via `WindowSystemInfo.render_surface`
- Or extend Delta's `VideoFormat` with a `.metal` case

### For audio:
- The `DeltaSoundStream` + `pullAudioSamples()` approach should work once the emulation loop runs
- Pulls from Dolphin's `Mixer` into Delta's `RingBuffer`
