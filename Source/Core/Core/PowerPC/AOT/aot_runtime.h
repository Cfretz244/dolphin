
#ifndef AOT_RUNTIME_H
#define AOT_RUNTIME_H

#include <stdint.h>

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

typedef struct { uint8_t* ram; uint32_t size; } AotFastMem;
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

static inline int aot_is_ram(uint32_t addr) {
    return ((addr & ~0x40000000u) - 0x80000000u) < aot_fast_mem.size;
}

static inline uint32_t aot_read_u8(AOTState* s, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1))
        return aot_fast_mem.ram[addr & 0x3FFFFFFFu];
    return aot_read_u8_slow(s, addr);
}
static inline uint32_t aot_read_u16(AOTState* s, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint16_t v; __builtin_memcpy(&v, aot_fast_mem.ram + (addr & 0x3FFFFFFFu), 2);
        return __builtin_bswap16(v);
    }
    return aot_read_u16_slow(s, addr);
}
static inline uint32_t aot_read_u16_se(AOTState* s, uint32_t addr) {  // sign-extended half
    return (uint32_t)(int32_t)(int16_t)aot_read_u16(s, addr);
}
static inline uint32_t aot_read_u32(AOTState* s, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint32_t v; __builtin_memcpy(&v, aot_fast_mem.ram + (addr & 0x3FFFFFFFu), 4);
        return __builtin_bswap32(v);
    }
    return aot_read_u32_slow(s, addr);
}
static inline uint64_t aot_read_u64(AOTState* s, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint64_t v; __builtin_memcpy(&v, aot_fast_mem.ram + (addr & 0x3FFFFFFFu), 8);
        return __builtin_bswap64(v);
    }
    return aot_read_u64_slow(s, addr);
}
static inline void aot_write_u8(AOTState* s, uint32_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        aot_fast_mem.ram[addr & 0x3FFFFFFFu] = (uint8_t)val;
        return;
    }
    aot_write_u8_slow(s, val, addr);
}
static inline void aot_write_u16(AOTState* s, uint32_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint16_t v = __builtin_bswap16((uint16_t)val);
        __builtin_memcpy(aot_fast_mem.ram + (addr & 0x3FFFFFFFu), &v, 2);
        return;
    }
    aot_write_u16_slow(s, val, addr);
}
static inline void aot_write_u16_br(AOTState* s, uint32_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint16_t v = (uint16_t)val;  // no swap — byte-reversed store
        __builtin_memcpy(aot_fast_mem.ram + (addr & 0x3FFFFFFFu), &v, 2);
        return;
    }
    aot_write_u16_br_slow(s, val, addr);
}
static inline void aot_write_u32(AOTState* s, uint32_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint32_t v = __builtin_bswap32(val);
        __builtin_memcpy(aot_fast_mem.ram + (addr & 0x3FFFFFFFu), &v, 4);
        return;
    }
    aot_write_u32_slow(s, val, addr);
}
static inline void aot_write_u64(AOTState* s, uint64_t val, uint32_t addr) {
    if (__builtin_expect(aot_is_ram(addr), 1)) {
        uint64_t v = __builtin_bswap64(val);
        __builtin_memcpy(aot_fast_mem.ram + (addr & 0x3FFFFFFFu), &v, 8);
        return;
    }
    aot_write_u64_slow(s, val, addr);
}

// Runtime helpers (implemented in the Dolphin runtime harness)
extern void aot_interpreter_single_step(AOTState* s);
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
extern void aot_cr_logical(AOTState* s, int crbD, int crbA, int crbB, const char* op);

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
