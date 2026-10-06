#ifndef AOT_IMAGES_H
#define AOT_IMAGES_H

// C ABI between Dolphin's AOT image tracker and libraries generated in trusted
// multi-image mode (dolphin-tool translate on a --dol-images CFG).
//
// Like aot_runtime.h this checked-in file is embedded into dolphin-tool and
// emitted verbatim into every generated multi-image aot-src tree. It is
// deliberately separate from aot_runtime.h: single-image libraries never see
// it, so changing it is NOT an AOT_ABI_VERSION break. Bump AOT_IMAGES_VERSION
// instead; aot_register_game_images rejects libraries built against another
// version (the game then runs on the interpreter).
//
// Trust model (identical to every single-image game and to the JIT): native
// code for an image executes without per-block instruction checks. Which
// image is loaded at the shared addresses is decided by the runtime tracker,
// re-evaluated only after an instruction-cache invalidation event
// (icbi / HID0.ICFI / cache write / savestate load), by comparing a sparse set
// of discriminator words against RAM. Code that changes without any such
// event is undetected — exactly the JIT's exposure.

#include <stdint.h>
#include "aot_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2: AOT_IMAGE_REAL_MODE images + the aot_active_image_real dispatch slot.
 * 3: Wii-SDK RSO modules (AotRsoModuleDesc, aot_register_game_rso_modules,
 *    aot_rso_dispatch as the dispatch's last resort). */
#define AOT_IMAGES_VERSION 3

/* One expected instruction word at a fixed address. */
typedef struct AotImageWord {
    uint32_t addr;
    uint32_t word;
} AotImageWord;

/* A runtime code patch site (PrimeHack). Bit i of the patch mask is set when
 * RAM holds `replacement` at patches[i].addr; any other word is invalid. */
typedef struct AotImagePatchSite {
    uint32_t addr;
    uint32_t original;
    uint32_t replacement;
} AotImagePatchSite;

/* A table entry rewritten by the tracker whenever the patch mask changes.
 * by_mask[k] / size_by_mask[k] correspond to masks[k] of the owning image;
 * NULL means "no native code for this pc under that mask" (interpreter).
 * base/base_size apply when the mask is zero (all patches restored). */
typedef struct AotImageOverride {
    uint32_t pc;
    uint32_t base_size;
    AOTBlockFunc base;
    const AOTBlockFunc* by_mask;
    const uint32_t* size_by_mask;
} AotImageOverride;

/* Block boundary metadata (harness builds only; NULL/0 otherwise). Sorted by
 * addr. */
typedef struct AotImageBlockSize {
    uint32_t addr;
    uint32_t num_instructions;
} AotImageBlockSize;

typedef struct AotImageDesc {
    const char* name;
    uint32_t base;              /* first table address */
    uint32_t size;              /* table entries (4-byte granularity) */
    AOTBlockFunc* table;        /* mutable: overrides are written in place */
    const AotImageWord* words;  /* discriminators; ALL must match RAM */
    uint32_t word_count;
    const AotImagePatchSite* patches;
    uint32_t patch_count;
    const uint32_t* masks;      /* supported non-zero patch masks */
    uint32_t mask_count;
    const AotImageOverride* overrides;
    uint32_t override_count;
    const AotImageBlockSize* block_sizes;
    uint32_t block_size_count;
    uint32_t flags;             /* AOT_IMAGE_AUXILIARY: may be active alongside a DOL */
} AotImageDesc;

/* Auxiliary images (trace-sourced code such as a launcher's relocated loader
 * stub) never overlap a DOL's text and are selected into a second slot, so
 * the DOL image they load stays active as soon as it matches. */
#define AOT_IMAGE_AUXILIARY 1u

/* Real-mode images (the OS's exception vectors at physical 0x100-0x1700,
 * entered with MSR.IR=DR=0): table indices are PHYSICAL pcs (base is a
 * physical address), and the code was emitted with every memory access on the
 * MMU slow path (which honours MSR.DR). They are selected into a third slot,
 * independently of the DOL and auxiliary slots. Their discriminator addresses
 * are the cached virtual alias (0x80000000 | physical) of the vector words, so
 * the tracker reads them like any other image's; every translated word is a
 * discriminator (the vectors are a few hundred words). */
#define AOT_IMAGE_REAL_MODE 2u

/* Runtime state read by the generated dispatch. */
typedef struct AotActiveImage {
    uint32_t base;
    uint32_t size;
    AOTBlockFunc* table;
} AotActiveImage;
extern AotActiveImage aot_active_image;
extern AotActiveImage aot_active_image_aux;
extern AotActiveImage aot_active_image_real;
/* Points at the emulated instruction cache's invalidation counter; dispatch
 * rescans when it differs from aot_images_seen. */
extern const uint64_t* aot_images_generation;
extern uint64_t aot_images_seen;

/* ------------------------------------------------------------------------
 * Wii-SDK RSO modules (Prime 2/3 / launcher): position-independent code that
 * the game loads into heap buffers and locates in place at bases that move.
 * Blocks are emitted base-relative (pc expressions read <sym>_base[sect]) and
 * found at runtime by "miss-triggered identification": when dispatch misses a
 * MEM1/MEM2 pc with MSR.IR set, the tracker scans backwards for a located RSO
 * header (word[3] == base + 0x58, 10 <= numSections <= 40), matches the build
 * path basename the header's name pointer names, and verifies section sizes
 * plus `words` (offsets from the module base: unrelocated text words and
 * relocation sites whose value is base-independent) before activating the
 * module's table. Active modules are re-verified after every
 * instruction-cache invalidation event.
 * ------------------------------------------------------------------------ */
typedef struct AotRsoWord {
    uint32_t offset;  /* from the module base (= RSO file offset) */
    uint32_t word;
} AotRsoWord;

typedef struct AotRsoModuleDesc {
    const char* name;               /* e.g. "RSO_FishCloud.plf" */
    uint32_t num_sections;
    const uint32_t* section_sizes;  /* [num_sections], as in the file */
    uint32_t text_section;          /* index of the code section */
    uint32_t text_size;             /* = section_sizes[text_section] */
    const AOTBlockFunc* table;      /* [text_size / 4], by text offset >> 2 */
    uint32_t* base_slots;           /* [num_sections], written by the tracker */
    const AotRsoWord* words;
    uint32_t word_count;
    const AotImageBlockSize* block_sizes;  /* harness only; addr = text offset */
    uint32_t block_size_count;
} AotRsoModuleDesc;

/* Last resort of the generated dispatch when the library has RSO modules. */
extern void aot_rso_dispatch(AOTState* s);
extern void aot_register_game_rso_modules(const char* game_id, const AotRsoModuleDesc* modules,
                                          uint32_t count, uint32_t images_version);

extern void aot_images_rescan(void);
extern AOTBlockFunc aot_images_lookup(uint32_t pc);
extern uint32_t aot_images_block_size(uint32_t pc);
extern void aot_register_game_images(const char* game_id, const AotImageDesc* images,
                                     uint32_t count, uint32_t images_version);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AOT_IMAGES_H
