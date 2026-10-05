
#ifndef AOT_RUNTIME_H
#define AOT_RUNTIME_H

// C ABI between Dolphin's AOT runtime and the generated per-game code.
//
// This checked-in file is the single source of truth: dolphin-tool embeds its
// bytes at build time (CMake/StringifyHeader.cmake) and `translate` emits a
// verbatim copy into every generated aot-src tree, where the generated .c
// files include it. The C++ runtime (AotRuntime.cpp and friends) compiles
// against this same file via AotState.h.
//
// ANY change to this header is an ABI break: bump AOT_ABI_VERSION and
// re-translate + rebuild every game's AOT library. AotRegistry rejects
// libraries built against a different version (they fall back to the
// interpreter instead of corrupting state).
#define AOT_ABI_VERSION 3

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// AOTState is layout-compatible with Dolphin's PowerPCState.
// At runtime, a PowerPCState* is cast to AOTState*.
typedef struct AOTState {
    uint32_t pc;
    uint32_t npc;
    void* stored_stack_pointer;
    void* gather_pipe_ptr;
    void* gather_pipe_base_ptr;
    uint32_t gpr[32];

    // Paired singles (ps0 and ps1 as raw uint64_t)
    struct { uint64_t ps0; uint64_t ps1; } ps[32] __attribute__((aligned(16)));

    // CR: 8 x uint64_t in Dolphin's optimized internal representation
    uint64_t cr_fields[8];

    uint32_t msr;
    uint32_t fpscr;
    uint32_t feature_flags;
    uint32_t exceptions;
    int32_t downcount;
    uint8_t xer_ca;
    uint8_t xer_so_ov;  // format: (SO << 1) | OV
    uint16_t xer_stringctrl;
    uint32_t reserve_address;
    uint8_t reserve;
    uint8_t pagetable_update_pending;
    uint8_t m_enable_dcache;
    uint8_t _pad0;

    uint32_t sr[16];
    uint32_t spr[1024] __attribute__((aligned(8)));
} AOTState;

typedef void (*AOTBlockFunc)(AOTState*);

// ============================================================================
// Memory access — RAM fast path inlined into blocks; slow path (MMIO, EFB,
// locked cache, gather pipe) stays in the Dolphin runtime harness, which is the
// single authoritative implementation (PowerPC::ReadFromJit/WriteFromJit).
//
// The range check must match the runtime's IsRAMAddress exactly: cached RAM
// (0x80000000-) or the uncached mirror (0xC0000000-) only. Low-memory
// (0x0xxxxxxx) and 0x4xxxxxxx accesses must take the slow path — with MSR.DR=1
// the interpreter raises a DSI for them (BAT miss), so the fast path must not
// silently satisfy them from RAM.
// ============================================================================

// Effective-address layout of the mirror pairs (see MMU.cpp IsRAMAddress):
// MEM1 is 0x80000000+ cached / 0xC0000000+ uncached; Wii MEM2 is 0x90000000+
// cached / 0xD0000000+ uncached. Clearing the mirror bit folds each pair onto
// its cached range. MEM1 offsets index the host RAM buffer directly; MEM2 is a
// SEPARATE host buffer (EXRAM), so its folded address is rebased by the
// 0x10000000 gap between the folded bases — the low-30-bits mask trick is
// only valid for MEM1. On GC boots exram is null and exram_size is 0, so the
// MEM2 arm is a never-taken compare.
//
// Like the MEM1 path, the MEM2 fast path assumes the SDK's standard identity
// BAT mapping; games that remap take the slow path via the range miss.
#define AOT_MEM_CACHED_BASE    0x80000000u
#define AOT_MEM_UNCACHED_BIT   0x40000000u
#define AOT_MEM_OFFSET_MASK    0x3FFFFFFFu
#define AOT_MEM2_FOLDED_OFFSET 0x10000000u  /* folded MEM2 base - folded MEM1 base */

typedef struct {
    uint8_t* ram;         /* MEM1 host buffer */
    uint32_t size;        /* MEM1 real size */
    uint8_t* exram;       /* Wii MEM2 host buffer, NULL on GC */
    uint32_t exram_size;  /* 0 on GC */
} AotFastMem;
extern AotFastMem aot_fast_mem;  // filled by aot_init_fast_mem() before any block runs

extern uint32_t aot_read_u8_slow(AOTState* s, uint32_t addr);
extern uint32_t aot_read_u16_slow(AOTState* s, uint32_t addr);
extern uint32_t aot_read_u32_slow(AOTState* s, uint32_t addr);
extern uint64_t aot_read_u64_slow(AOTState* s, uint32_t addr);
extern void aot_write_u8_slow(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u16_slow(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u16_br_slow(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u32_slow(AOTState* s, uint32_t val, uint32_t addr);
extern void aot_write_u64_slow(AOTState* s, uint64_t val, uint32_t addr);

// Resolve an effective address to a host pointer: MEM1 first (identical cost
// to the pre-v3 single-region check), then MEM2. NULL = slow path. Low-memory
// (0x0xxxxxxx) and 0x4xxxxxxx accesses miss both ranges and stay on the slow
// path — with MSR.DR=1 the interpreter raises a DSI for them (BAT miss), so
// the fast path must not silently satisfy them from RAM.
static inline uint8_t* aot_host_ptr(uint32_t addr) {
    uint32_t off1 = (addr & ~AOT_MEM_UNCACHED_BIT) - AOT_MEM_CACHED_BASE;
    if (__builtin_expect(off1 < aot_fast_mem.size, 1))
        return aot_fast_mem.ram + off1;
    uint32_t off2 = off1 - AOT_MEM2_FOLDED_OFFSET;
    if (off2 < aot_fast_mem.exram_size)
        return aot_fast_mem.exram + off2;
    return 0;
}
static inline int aot_is_ram(uint32_t addr) {
    return aot_host_ptr(addr) != 0;
}

static inline uint32_t aot_read_u8(AOTState* s, uint32_t addr) {
    uint8_t* p = aot_host_ptr(addr);
    if (__builtin_expect(p != 0, 1))
        return *p;
    return aot_read_u8_slow(s, addr);
}
static inline uint32_t aot_read_u16(AOTState* s, uint32_t addr) {
    uint8_t* p = aot_host_ptr(addr);
    if (__builtin_expect(p != 0, 1)) {
        uint16_t v; __builtin_memcpy(&v, p, 2);
        return __builtin_bswap16(v);
    }
    return aot_read_u16_slow(s, addr);
}
static inline uint32_t aot_read_u16_se(AOTState* s, uint32_t addr) {  // sign-extended half
    return (uint32_t)(int32_t)(int16_t)aot_read_u16(s, addr);
}
static inline uint32_t aot_read_u32(AOTState* s, uint32_t addr) {
    uint8_t* p = aot_host_ptr(addr);
    if (__builtin_expect(p != 0, 1)) {
        uint32_t v; __builtin_memcpy(&v, p, 4);
        return __builtin_bswap32(v);
    }
    return aot_read_u32_slow(s, addr);
}
static inline uint64_t aot_read_u64(AOTState* s, uint32_t addr) {
    uint8_t* p = aot_host_ptr(addr);
    if (__builtin_expect(p != 0, 1)) {
        uint64_t v; __builtin_memcpy(&v, p, 8);
        return __builtin_bswap64(v);
    }
    return aot_read_u64_slow(s, addr);
}
static inline void aot_write_u8(AOTState* s, uint32_t val, uint32_t addr) {
    uint8_t* p = aot_host_ptr(addr);
    if (__builtin_expect(p != 0, 1)) {
        *p = (uint8_t)val;
        return;
    }
    aot_write_u8_slow(s, val, addr);
}
static inline void aot_write_u16(AOTState* s, uint32_t val, uint32_t addr) {
    uint8_t* p = aot_host_ptr(addr);
    if (__builtin_expect(p != 0, 1)) {
        uint16_t v = __builtin_bswap16((uint16_t)val);
        __builtin_memcpy(p, &v, 2);
        return;
    }
    aot_write_u16_slow(s, val, addr);
}
static inline void aot_write_u16_br(AOTState* s, uint32_t val, uint32_t addr) {
    uint8_t* p = aot_host_ptr(addr);
    if (__builtin_expect(p != 0, 1)) {
        uint16_t v = (uint16_t)val;  // no swap — byte-reversed store
        __builtin_memcpy(p, &v, 2);
        return;
    }
    aot_write_u16_br_slow(s, val, addr);
}
static inline void aot_write_u32(AOTState* s, uint32_t val, uint32_t addr) {
    uint8_t* p = aot_host_ptr(addr);
    if (__builtin_expect(p != 0, 1)) {
        uint32_t v = __builtin_bswap32(val);
        __builtin_memcpy(p, &v, 4);
        return;
    }
    aot_write_u32_slow(s, val, addr);
}
static inline void aot_write_u64(AOTState* s, uint64_t val, uint32_t addr) {
    uint8_t* p = aot_host_ptr(addr);
    if (__builtin_expect(p != 0, 1)) {
        uint64_t v = __builtin_bswap64(val);
        __builtin_memcpy(p, &v, 8);
        return;
    }
    aot_write_u64_slow(s, val, addr);
}

// ============================================================================
// Registration — each game's dispatch.c registers itself from an
// __attribute__((constructor)) before main(). The abi_version argument is
// emitted as the AOT_ABI_VERSION macro, so a library carries the version of
// the header it was generated against; AotRegistry rejects mismatches.
// ============================================================================

// Per-REL-module dispatch tables and runtime section base slots, activated by
// the module tracker when the game loads a module.
typedef struct AotModuleSectionDesc {
    uint32_t size;
    uint32_t executable;
    const AOTBlockFunc* table;  /* NULL for non-executable sections */
    uint32_t* base_slot;        /* runtime section base, 0 = unloaded */
} AotModuleSectionDesc;
typedef struct AotModuleDesc {
    uint32_t module_id;
    uint32_t num_sections;
    const AotModuleSectionDesc* sections;
    uint32_t flags;  /* reserved (0); headroom for per-game loader modes
                        without another ABI bump. Trails the struct so the
                        emitted positional initializers zero-fill it. */
} AotModuleDesc;

// Block boundary metadata for the AOT_COMPARE/diff harness. Only emitted (and
// only registered) in AOT_HARNESS builds; production and iOS pay nothing.
typedef struct AotBlockSize {
    uint32_t addr;              /* DOL block start address */
    uint32_t num_instructions;
} AotBlockSize;
typedef struct AotModuleBlockSize {
    uint32_t module_id;
    uint32_t section;
    uint32_t offset;            /* section-relative block start */
    uint32_t num_instructions;
} AotModuleBlockSize;

extern void aot_register_game(const char* game_id, void (*dispatch)(AOTState*),
                              AOTBlockFunc (*lookup)(uint32_t), uint32_t abi_version);
extern void aot_register_game_modules(const char* game_id, const AotModuleDesc* modules,
                                      uint32_t count);
extern void aot_register_block_sizes(const char* game_id, const AotBlockSize* blocks,
                                     uint32_t count, const AotModuleBlockSize* module_blocks,
                                     uint32_t module_count);
/* Source-image identity: sha256 (lowercase hex) of the boot DOL of the disc
 * image this library was generated from. AOTCore refuses to run against a
 * disc whose DOL hashes differently -- a mismatched image otherwise executes
 * translated code against the wrong layout (crash at best, silence at worst;
 * the April 2026 Rev0-vs-Rev2 incident). Additive entry point, deliberately
 * NOT an AOT_ABI_VERSION bump: no existing type or entry point changed, and
 * pre-existing libraries that never call this must keep registering (they get
 * a cannot-verify warning instead of the hard gate). */
extern void aot_register_game_image(const char* game_id, const char* dol_sha256_hex);

// Runtime helpers (implemented in the Dolphin runtime harness)
extern void aot_interpreter_single_step(AOTState* s);
// Module-aware terminal dispatch — musttail-called by <ID>_dispatch when the
// DOL fast table misses and the game has compiled REL modules.
extern void aot_module_dispatch(AOTState* s);
extern void aot_sc(AOTState* s);
extern int aot_check_fpu(AOTState* s, uint32_t pc);
extern void aot_rfi(AOTState* s);

// FP conversion helpers
extern uint64_t aot_convert_to_double(uint32_t single_bits);
extern uint32_t aot_convert_to_single(uint64_t double_bits);

// SPR/CR/MSR helpers
extern uint32_t aot_mfspr_special(AOTState* s, uint32_t spr);
extern void aot_mtspr_special(AOTState* s, uint32_t spr, uint32_t val);
extern uint32_t aot_mfcr(AOTState* s);
extern void aot_mtcrf(AOTState* s, uint32_t mask, uint32_t rs_reg);
extern void aot_msr_updated(AOTState* s);
extern void aot_mtmsr(AOTState* s, uint32_t val);
extern void aot_sr_updated(AOTState* s);
extern int aot_twi(AOTState* s, uint32_t TO, int32_t a, int32_t b);

// CR-field logical ops (crand, cror, ...). Operand semantics follow the
// interpreter's Helper_* implementations in Interpreter_SystemRegisters.cpp.
typedef enum AotCrOp {
    AOT_CR_AND,   /* crand:  a & b        */
    AOT_CR_OR,    /* cror:   a | b        */
    AOT_CR_XOR,   /* crxor:  a ^ b        */
    AOT_CR_EQV,   /* creqv:  ~(a ^ b) & 1 */
    AOT_CR_ANDC,  /* crandc: a & ~b       */
    AOT_CR_ORC,   /* crorc:  a | ~b       */
    AOT_CR_NAND,  /* crnand: ~(a & b) & 1 */
    AOT_CR_NOR    /* crnor:  ~(a | b) & 1 */
} AotCrOp;
extern void aot_cr_logical(AOTState* s, int crbD, int crbA, int crbB, AotCrOp op);

// Cache ops
extern void aot_dcbz(AOTState* s, uint32_t addr);
extern void aot_dcbz_l(AOTState* s, uint32_t addr);
extern void aot_dcbt(AOTState* s, uint32_t addr);
extern void aot_icbi(AOTState* s, uint32_t addr);

// Timebase
extern uint32_t aot_mftb(AOTState* s, uint32_t spr);

// FP arithmetic (all via runtime for NaN/exception correctness)
extern void aot_faddx(AOTState* s, int fd, int fa, int fb);
extern void aot_fsubx(AOTState* s, int fd, int fa, int fb);
extern void aot_fmulx(AOTState* s, int fd, int fa, int fc);
extern void aot_fdivx(AOTState* s, int fd, int fa, int fb);
extern void aot_fmaddx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fmsubx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fnmsubx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fnmaddx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_faddsx(AOTState* s, int fd, int fa, int fb);
extern void aot_fsubsx(AOTState* s, int fd, int fa, int fb);
extern void aot_fmulsx(AOTState* s, int fd, int fa, int fc);
extern void aot_fdivsx(AOTState* s, int fd, int fa, int fb);
extern void aot_fmaddsx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fmsubsx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fnmsubsx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fnmaddsx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_fcmpu(AOTState* s, int crfd, int fa, int fb);
extern void aot_fcmpo(AOTState* s, int crfd, int fa, int fb);
extern void aot_frspx(AOTState* s, int fd, int fb);
extern void aot_fctiwx(AOTState* s, int fd, int fb);
extern void aot_fctiwzx(AOTState* s, int fd, int fb);
extern void aot_fresx(AOTState* s, int fd, int fb);
extern void aot_frsqrtex(AOTState* s, int fd, int fb);
extern void aot_fselx(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_mtfsf(AOTState* s, int fm, int fb);
extern void aot_mtfsfi(AOTState* s, int crfd, int imm);
extern void aot_mcrfs(AOTState* s, int crfd, int crfs);

// Paired singles (all via runtime)
extern void aot_ps_add(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_sub(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_mul(AOTState* s, int fd, int fa, int fc);
extern void aot_ps_div(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_madd(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_msub(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_nmadd(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_nmsub(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_sum0(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_sum1(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_muls0(AOTState* s, int fd, int fa, int fc);
extern void aot_ps_muls1(AOTState* s, int fd, int fa, int fc);
extern void aot_ps_madds0(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_madds1(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_sel(AOTState* s, int fd, int fa, int fc, int fb);
extern void aot_ps_res(AOTState* s, int fd, int fb);
extern void aot_ps_rsqrte(AOTState* s, int fd, int fb);
extern void aot_ps_neg(AOTState* s, int fd, int fb);
extern void aot_ps_mr(AOTState* s, int fd, int fb);
extern void aot_ps_abs(AOTState* s, int fd, int fb);
extern void aot_ps_nabs(AOTState* s, int fd, int fb);
extern void aot_ps_merge00(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_merge01(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_merge10(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_merge11(AOTState* s, int fd, int fa, int fb);
extern void aot_ps_cmpu0(AOTState* s, int crfd, int fa, int fb);
extern void aot_ps_cmpo0(AOTState* s, int crfd, int fa, int fb);
extern void aot_ps_cmpu1(AOTState* s, int crfd, int fa, int fb);
extern void aot_ps_cmpo1(AOTState* s, int crfd, int fa, int fb);
extern void aot_psq_l(AOTState* s, int fd, int ra, uint32_t inst);
extern void aot_psq_lu(AOTState* s, int fd, int ra, uint32_t inst);
extern void aot_psq_st(AOTState* s, int fs, int ra, uint32_t inst);
extern void aot_psq_stu(AOTState* s, int fs, int ra, uint32_t inst);
extern void aot_psq_lx(AOTState* s, uint32_t inst);
extern void aot_psq_stx(AOTState* s, uint32_t inst);
extern void aot_psq_lux(AOTState* s, uint32_t inst);
extern void aot_psq_stux(AOTState* s, uint32_t inst);

// ============================================================================
// Single-precision FP fast paths (static inline, generated code calls these).
//
// Bit-identical to the interpreter (Interpreter_FloatingPoint.cpp fmulsx /
// faddsx / fsubsx, Interpreter_PairedUtils.h PS_Add / PS_Sub / PS_Mul /
// PS_Muls0 / PS_Muls1) on the path where the interpreter touches NO FPSCR
// exception state: the double-precision op produced a finite result (add/sub:
// NI_add/NI_sub also clear FI/FR when an operand is infinite, so an infinite
// result is sent to the slow path too; mul: NI_mul only reacts to a NaN
// result). Everything else -- NaN, SNaN, inf-inf, inf*0, VE handling --
// falls back to the exact out-of-line helpers above, which run the
// interpreter's own code. The host FPU already runs in the mode
// RoundingModeUpdated() derived from FPSCR (RN + NI flush-to-zero), exactly as
// it does for the interpreter, so the double arithmetic here rounds/flushes
// identically. Purely additive: the helpers keep their names and semantics,
// so libraries generated against older headers keep linking (no ABI bump).
//
// FPSCR layout (UReg_FPSCR): NI bit 2, FPRF bits 12-16, FI bit 17, FR bit 18.
// ============================================================================
#define AOT_FPSCR_NI        (1u << 2)
#define AOT_FPSCR_FPRF_MASK (0x1Fu << 12)
#define AOT_FPSCR_FI        (1u << 17)
#define AOT_FPSCR_FR        (1u << 18)

static inline double aot_bits_to_double(uint64_t b) { double d; __builtin_memcpy(&d, &b, 8); return d; }
static inline uint64_t aot_double_to_bits(double d) { uint64_t b; __builtin_memcpy(&b, &d, 8); return b; }
static inline float aot_bits_to_float(uint32_t b) { float f; __builtin_memcpy(&f, &b, 4); return f; }
static inline uint32_t aot_float_to_bits(float f) { uint32_t b; __builtin_memcpy(&b, &f, 4); return b; }

// Common::ClassifyFloat -> PPC_FPCLASS_* (the 5-bit FPRF value).
static inline uint32_t aot_classify_float(float f) {
    uint32_t i = aot_float_to_bits(f);
    uint32_t sign = i & 0x80000000u;
    uint32_t exp = i & 0x7F800000u;
    if (exp > 0 && exp < 0x7F800000u)
        return sign ? 0x8u : 0x4u;          /* NN : PN */
    uint32_t mantissa = i & 0x007FFFFFu;
    if (mantissa) {
        if (exp)
            return 0x11u;                   /* QNAN */
        return sign ? 0x18u : 0x14u;        /* ND : PD */
    }
    if (exp)
        return sign ? 0x9u : 0x5u;          /* NINF : PINF */
    return sign ? 0x12u : 0x2u;             /* NZ : PZ */
}

// Interpreter_FPUtils.h ForceSingle(fpscr, value). The interpreter's trailing
// `if (!cpu_info.bFlushToZero && fpscr.NI) FlushToZero(x)` is omitted: every
// host the AOT runtime targets (arm64, x86-64 with SSE) reports bFlushToZero,
// so that branch is dead there and the host FZ bit does the flushing instead.
static inline float aot_force_single(uint32_t fpscr, double value) {
    if (fpscr & AOT_FPSCR_NI) {
        uint64_t bits = aot_double_to_bits(value);
        uint64_t value_without_sign = bits & 0x7FFFFFFFFFFFFFFFull;
        if (value_without_sign < 0x3810000000000000ull)  /* subnormal as a single: flush */
            return aot_bits_to_float((uint32_t)((bits & 0x8000000000000000ull) >> 32));
    }
    return (float)value;
}

// Interpreter_FPUtils.h Force25Bit(d): round the mantissa of frC to 25 bits.
static inline double aot_force_25bit(double d) {
    uint64_t integral = aot_double_to_bits(d);
    uint64_t exponent = integral & 0x7FF0000000000000ull;
    uint64_t fraction = integral & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0 && fraction != 0) {
        /* Subnormals are "normalized" before rounding: shift the keep mask and
           the rounding bit right until the fraction's MSB would reach the
           exponent. */
        int64_t keep_mask = (int64_t)0xFFFFFFFFF8000000ull;
        uint64_t round = 0x8000000ull;
        uint32_t shift = (uint32_t)__builtin_clzll(fraction) - (63 - 52);
        keep_mask >>= shift;
        round >>= shift;
        integral = (integral & (uint64_t)keep_mask) + (integral & round);
    } else {
        integral = (integral & 0xFFFFFFFFF8000000ull) + (integral & 0x8000000ull);
    }
    return aot_bits_to_double(integral);
}

// ps[fd].Fill(float) + UpdateFPRFSingle; `clear` is the extra FPSCR bits the
// instruction zeroes (fmulsx clears FI|FR, faddsx/fsubsx clear nothing).
static inline void aot_fp_fill_single(AOTState* s, int fd, float r, uint32_t clear) {
    uint64_t bits = aot_double_to_bits((double)r);
    s->ps[fd].ps0 = bits;
    s->ps[fd].ps1 = bits;
    s->fpscr = (s->fpscr & ~(AOT_FPSCR_FPRF_MASK | clear)) | (aot_classify_float(r) << 12);
}

static inline void aot_faddsx_fast(AOTState* s, int fd, int fa, int fb) {
    double sum = aot_bits_to_double(s->ps[fa].ps0) + aot_bits_to_double(s->ps[fb].ps0);
    if (__builtin_expect(!__builtin_isfinite(sum), 0)) { aot_faddsx(s, fd, fa, fb); return; }
    aot_fp_fill_single(s, fd, aot_force_single(s->fpscr, sum), 0);
}

static inline void aot_fsubsx_fast(AOTState* s, int fd, int fa, int fb) {
    double diff = aot_bits_to_double(s->ps[fa].ps0) - aot_bits_to_double(s->ps[fb].ps0);
    if (__builtin_expect(!__builtin_isfinite(diff), 0)) { aot_fsubsx(s, fd, fa, fb); return; }
    aot_fp_fill_single(s, fd, aot_force_single(s->fpscr, diff), 0);
}

static inline void aot_fmulsx_fast(AOTState* s, int fd, int fa, int fc) {
    double product = aot_bits_to_double(s->ps[fa].ps0) * aot_force_25bit(aot_bits_to_double(s->ps[fc].ps0));
    if (__builtin_expect(__builtin_isnan(product), 0)) { aot_fmulsx(s, fd, fa, fc); return; }
    aot_fp_fill_single(s, fd, aot_force_single(s->fpscr, product), AOT_FPSCR_FI | AOT_FPSCR_FR);
}

// ps[fd].SetBoth(ps0, ps1) + UpdateFPRFSingle(ps0). Both results are computed
// before anything is written, so fd aliasing fa/fb/fc is fine.
static inline void aot_ps_set_both(AOTState* s, int fd, float r0, float r1) {
    s->ps[fd].ps0 = aot_double_to_bits((double)r0);
    s->ps[fd].ps1 = aot_double_to_bits((double)r1);
    s->fpscr = (s->fpscr & ~AOT_FPSCR_FPRF_MASK) | (aot_classify_float(r0) << 12);
}

static inline void aot_ps_add_fast(AOTState* s, int fd, int fa, int fb) {
    double r0 = aot_bits_to_double(s->ps[fa].ps0) + aot_bits_to_double(s->ps[fb].ps0);
    double r1 = aot_bits_to_double(s->ps[fa].ps1) + aot_bits_to_double(s->ps[fb].ps1);
    if (__builtin_expect(!(__builtin_isfinite(r0) && __builtin_isfinite(r1)), 0)) { aot_ps_add(s, fd, fa, fb); return; }
    aot_ps_set_both(s, fd, aot_force_single(s->fpscr, r0), aot_force_single(s->fpscr, r1));
}

static inline void aot_ps_sub_fast(AOTState* s, int fd, int fa, int fb) {
    double r0 = aot_bits_to_double(s->ps[fa].ps0) - aot_bits_to_double(s->ps[fb].ps0);
    double r1 = aot_bits_to_double(s->ps[fa].ps1) - aot_bits_to_double(s->ps[fb].ps1);
    if (__builtin_expect(!(__builtin_isfinite(r0) && __builtin_isfinite(r1)), 0)) { aot_ps_sub(s, fd, fa, fb); return; }
    aot_ps_set_both(s, fd, aot_force_single(s->fpscr, r0), aot_force_single(s->fpscr, r1));
}

static inline void aot_ps_mul_fast(AOTState* s, int fd, int fa, int fc) {
    double r0 = aot_bits_to_double(s->ps[fa].ps0) * aot_force_25bit(aot_bits_to_double(s->ps[fc].ps0));
    double r1 = aot_bits_to_double(s->ps[fa].ps1) * aot_force_25bit(aot_bits_to_double(s->ps[fc].ps1));
    if (__builtin_expect(__builtin_isnan(r0) || __builtin_isnan(r1), 0)) { aot_ps_mul(s, fd, fa, fc); return; }
    aot_ps_set_both(s, fd, aot_force_single(s->fpscr, r0), aot_force_single(s->fpscr, r1));
}

static inline void aot_ps_muls0_fast(AOTState* s, int fd, int fa, int fc) {
    double c0 = aot_force_25bit(aot_bits_to_double(s->ps[fc].ps0));
    double r0 = aot_bits_to_double(s->ps[fa].ps0) * c0;
    double r1 = aot_bits_to_double(s->ps[fa].ps1) * c0;
    if (__builtin_expect(__builtin_isnan(r0) || __builtin_isnan(r1), 0)) { aot_ps_muls0(s, fd, fa, fc); return; }
    aot_ps_set_both(s, fd, aot_force_single(s->fpscr, r0), aot_force_single(s->fpscr, r1));
}

static inline void aot_ps_muls1_fast(AOTState* s, int fd, int fa, int fc) {
    double c1 = aot_force_25bit(aot_bits_to_double(s->ps[fc].ps1));
    double r0 = aot_bits_to_double(s->ps[fa].ps0) * c1;
    double r1 = aot_bits_to_double(s->ps[fa].ps1) * c1;
    if (__builtin_expect(__builtin_isnan(r0) || __builtin_isnan(r1), 0)) { aot_ps_muls1(s, fd, fa, fc); return; }
    aot_ps_set_both(s, fd, aot_force_single(s->fpscr, r0), aot_force_single(s->fpscr, r1));
}

// CR helpers (inline for performance)
// Values from ConditionRegister::PPCToInternal() — Dolphin's optimized 64-bit CR encoding.
// Index = 4-bit PPC CR field value (LT=8, GT=4, EQ=2, SO=1).
// Internal: SO=bit59, EQ=(low32==0), GT=((s64)val>0), LT=bit62.
static const uint64_t aot_cr_table[16] = {
    0x8000000100000001ULL, 0x8800000100000001ULL, 0x8000000100000000ULL, 0x8800000100000000ULL,
    0x0000000100000001ULL, 0x0800000100000001ULL, 0x0000000100000000ULL, 0x0800000100000000ULL,
    0xC000000100000001ULL, 0xC800000100000001ULL, 0xC000000100000000ULL, 0xC800000100000000ULL,
    0x4000000100000001ULL, 0x4800000100000001ULL, 0x4000000100000000ULL, 0x4800000100000000ULL,
};

static inline void aot_cr_set_field(AOTState* s, int field, uint32_t value) {
    s->cr_fields[field] = aot_cr_table[value & 0xF];
}

static inline uint32_t aot_cr_get_bit(AOTState* s, int bit) {
    int field = bit >> 2;
    int bit_in_field = 3 - (bit & 3);
    uint64_t cr = s->cr_fields[field];
    uint32_t ppc_cr = 0;
    // Reconstruct PPC CR field from internal representation
    ppc_cr |= (cr >> 59) & 0x9;  // LT (bit 62->bit 3) and SO (bit 59->bit 0)
    ppc_cr |= ((cr & 0xFFFFFFFF) == 0) << 1;  // EQ
    ppc_cr |= ((int64_t)cr > 0) << 2;  // GT
    return (ppc_cr >> bit_in_field) & 1;
}

static inline void aot_cmp_signed(AOTState* s, int crfd, int32_t a, int32_t b) {
    uint32_t cr_field;
    if (a < b) cr_field = 8;       // CR_LT
    else if (a > b) cr_field = 4;  // CR_GT
    else cr_field = 2;             // CR_EQ
    if (s->xer_so_ov >> 1) cr_field |= 1; // CR_SO
    aot_cr_set_field(s, crfd, cr_field);
}

static inline void aot_cmp_unsigned(AOTState* s, int crfd, uint32_t a, uint32_t b) {
    uint32_t cr_field;
    if (a < b) cr_field = 8;
    else if (a > b) cr_field = 4;
    else cr_field = 2;
    if (s->xer_so_ov >> 1) cr_field |= 1;
    aot_cr_set_field(s, crfd, cr_field);
}

static inline uint32_t aot_rotl(uint32_t val, uint32_t shift) {
    shift &= 31;
    return (val << shift) | (val >> (32 - shift));
}

static inline uint32_t aot_rotation_mask(int mb, int me) {
    uint32_t begin_mask = 0xFFFFFFFFu >> mb;
    uint32_t end_mask = (me < 31) ? (0xFFFFFFFFu >> (me + 1)) : 0;
    uint32_t mask = begin_mask ^ end_mask;
    return (me >= mb) ? mask : ~mask;
}

// Single-block mode flag (set by compare/diff harness to stop block chaining)
extern int aot_single_block_mode;

#ifdef __cplusplus
}  // extern "C"
#endif

// Block edges test AOT_EDGE_STOP alongside the downcount check. In production the
// flag can never be set, so the load is compiled out; harness builds (macOS
// build.sh passes -DAOT_HARNESS=1) keep it so AOT_COMPARE can stop chaining and
// compare single blocks. The dispatch-entry check remains unconditional either way.
#ifndef AOT_HARNESS
#define AOT_HARNESS 0
#endif
#if AOT_HARNESS
#define AOT_EDGE_STOP || aot_single_block_mode
#else
#define AOT_EDGE_STOP
#endif

#endif // AOT_RUNTIME_H
