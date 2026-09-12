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

#define AOT_IMAGES_VERSION 1

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
} AotImageDesc;

/* Runtime state read by the generated dispatch. */
typedef struct AotActiveImage {
    uint32_t base;
    uint32_t size;
    AOTBlockFunc* table;
} AotActiveImage;
extern AotActiveImage aot_active_image;
/* Points at the emulated instruction cache's invalidation counter; dispatch
 * rescans when it differs from aot_images_seen. */
extern const uint64_t* aot_images_generation;
extern uint64_t aot_images_seen;

extern void aot_images_rescan(void);
extern AOTBlockFunc aot_images_lookup(uint32_t pc);
extern uint32_t aot_images_block_size(uint32_t pc);
extern void aot_register_game_images(const char* game_id, const AotImageDesc* images,
                                     uint32_t count, uint32_t images_version);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AOT_IMAGES_H
