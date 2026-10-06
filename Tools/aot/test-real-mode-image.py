#!/usr/bin/env python3
"""ROM-free regression for real-mode images (translate --real-mode-image): the
OS exception vectors at physical 0x100-0x1700, entered with MSR.IR=DR=0.
A synthetic low-memory dump holds a vector at 0x900 (mtsprg0; li; lwz 0xc0(0);
rfi). The translator must discover its blocks statically, emit them with every
memory access on the MMU slow path (physical EA, no fast-path range match),
deduplicate identical dumps, mark the images AOT_IMAGE_REAL_MODE with
discriminators at the cached virtual alias, and the generated dispatch must
enter them only while MSR.IR is clear.
Usage: python3 Tools/aot/test-real-mode-image.py /absolute/path/to/dolphin-tool
"""
import pathlib
import struct
import subprocess
import sys
import tempfile


def run(*args, ok=True):
    p = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (p.returncode == 0) != ok:
        raise AssertionError(f'{args}: exit {p.returncode}\n{p.stdout}')
    return p.stdout


def make_dol(value):
    data = bytearray(0x108)
    for offset, word in [(0, 0x100), (0x48, 0x80004000), (0x90, 8), (0xE0, 0x80004000),
                         (0x100, 0x38600000 | value), (0x104, 0x4E800020)]:
        struct.pack_into('>I', data, offset, word)
    return data


def make_lowmem(li_value):
    mem = bytearray(0x1000)
    # 0x900: mtsprg0 r4 | li r3,N | lwz r4,0xc0(0) | rfi ; 0xa00.. zero (no vector)
    for i, word in enumerate([0x7C9043A6, 0x38600000 | li_value, 0x808000C0, 0x4C000064]):
        struct.pack_into('>I', mem, 0x900 + 4 * i, word)
    struct.pack_into('>I', mem, 0xC0, 0x00123450)  # data word (OS globals), not code
    return mem


def main():
    tool = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='aot-real-mode-') as temp:
        root = pathlib.Path(temp)
        iso = bytearray(0x4000)
        iso[:6] = b'TSTE01'
        for offset, value in [(0x1C, 0xC2339F3D), (0x420, 0x1000), (0x424, 0x2000),
                              (0x428, 32), (0x42C, 32)]:
            struct.pack_into('>I', iso, offset, value)
        iso[0x1000:0x1108] = make_dol(1)
        struct.pack_into('>6I', iso, 0x2000, 0x01000000, 0, 1, 0, 0, 0)
        disc = root / 'test.iso'
        disc.write_bytes(iso)
        trace = bytearray(struct.pack('<7I', 0x54485044, 4, 1, 0, 0, 0, 36))
        trace += struct.pack('<2I', 0x80004000, 8)
        trace += struct.pack('<2I', 0x80004000, 1)
        trace += struct.pack('<3IQ3I', 3, 1, 1, 1, 0x38600001, 0x4E800020, 0xDEADBEEF)
        (root / 'test.dpht').write_bytes(trace)
        cfg = root / 'test.db'
        run(tool, 'cfg', '--iso', str(disc), '--trace', str(root / 'test.dpht'), '--dol-images',
            '--output', str(cfg))
        dumps = []
        for name, value in [('a', 7), ('b', 8), ('a2', 7)]:
            path = root / f'lowmem-{name}.bin'
            path.write_bytes(make_lowmem(value))
            dumps.append(str(path))
        out = root / 'generated'
        log = run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output', str(out),
                  '--real-mode-image', ','.join(dumps))
        assert 'identical to an earlier dump, skipped' in log, log
        assert "<real:lowmem-a.bin>" in log and "<real:lowmem-b.bin>" in log, log
        src = ''.join(p.read_text() for p in out.glob('*.c'))
        # The real-mode vector block: the empty fast-mem descriptor, never the live one.
        real = ''.join(p.read_text() for p in out.glob('TSTE01_blocks_d[12]_*.c'))
        assert 'aot_fm=(AotFastMem){.dbat=aot_fast_mem.dbat}' in real, real
        assert 'aot_fm=aot_fast_mem;' not in real, real
        assert 'aot_active_image_real' in src
        # rfi leaves through the shared dispatch (MSR.IR test), never a per-site probe of
        # the real-mode table.
        assert 'TSTE01_d1_fast_table[_idx]' not in real and 'TSTE01_d2_fast_table[_idx]' not in real
        # A bogus dump (no vector) is an error, not an empty image.
        (root / 'empty.bin').write_bytes(bytes(0x1000))
        bad = root / 'bad'
        run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output', str(bad),
            '--real-mode-image', str(root / 'empty.bin'), ok=False)
        harness = out / 'test.c'
        harness.write_text(r'''
#include <assert.h>
#include <string.h>
#include "aot_images.h"
extern void TSTE01_dispatch(AOTState*);
extern AOTBlockFunc TSTE01_lookup_block(uint32_t);
static uint32_t dol[2] = {0x38600001, 0x4e800020};
static uint32_t vec[4] = {0x7C9043A6, 0x38600007, 0x808000C0, 0x4C000064};
static uint64_t generation = 1;
static const AotImageDesc* images; static uint32_t image_count;
static int fallbacks, reads, sprg0_writes, rfis; static uint32_t last_read;
int aot_single_block_mode;
AotFastMem aot_fast_mem;
AotActiveImage aot_active_image, aot_active_image_aux, aot_active_image_real;
const uint64_t* aot_images_generation = &generation;
uint64_t aot_images_seen;
static uint32_t rd(uint32_t a) {
  if (a - 0x80004000u < 8) return dol[(a - 0x80004000u) / 4];
  if (a - 0x80000900u < 16) return vec[(a - 0x80000900u) / 4];
  return 0;
}
static int matches(const AotImageDesc* i) {
  int ok = i->word_count > 0;
  for (uint32_t w = 0; w < i->word_count; w++) ok &= rd(i->words[w].addr) == i->words[w].word;
  return ok;
}
void aot_images_rescan(void) {
  aot_images_seen = generation;
  aot_active_image = aot_active_image_real = (AotActiveImage){0, 0, 0};
  for (uint32_t i = 0; i < image_count; i++) {
    AotActiveImage* slot = (images[i].flags & AOT_IMAGE_REAL_MODE) ? &aot_active_image_real : &aot_active_image;
    if (!slot->table && matches(&images[i])) *slot = (AotActiveImage){images[i].base, images[i].size, images[i].table};
  }
}
AOTBlockFunc aot_images_lookup(uint32_t pc) { return 0; }
uint32_t aot_images_block_size(uint32_t pc) { return 0; }
void aot_interpreter_single_step(AOTState* s) { ++fallbacks; s->downcount = 0; }
AOT_SLOWPATH_CC uint32_t aot_read_u32_slow(AOTState* s, uint32_t addr) { ++reads; last_read = addr; return 0x00123450; }
void aot_mtspr_special(AOTState* s, uint32_t spr, uint32_t val) { if (spr == 272) { ++sprg0_writes; s->spr[272] = val; } }
void aot_rfi(AOTState* s) { ++rfis; s->pc = s->spr[26]; s->msr = s->spr[27]; s->downcount = 0; }
void aot_register_game(const char* id, void (*d)(AOTState*), AOTBlockFunc (*l)(uint32_t), uint32_t v) {}
void aot_register_game_image(const char* id, const char* h) {}
void aot_register_image_block_sizes(const char* id, uint32_t (*lookup)(uint32_t)) {}
void aot_register_game_images(const char* id, const AotImageDesc* i, uint32_t n, uint32_t v) { assert(v == AOT_IMAGES_VERSION); images = i; image_count = n; }
static void vector(uint32_t msr, uint32_t expect_r3, int expect_fallbacks) {
  AOTState s; memset(&s, 0, sizeof s); s.pc = 0x900; s.msr = msr; s.downcount = 100;
  s.gpr[4] = 0xabcd; s.spr[26] = 0x80004000; s.spr[27] = 0x30;
  fallbacks = reads = sprg0_writes = rfis = 0; last_read = 0;
  TSTE01_dispatch(&s);
  assert(fallbacks == expect_fallbacks);
  if (!expect_fallbacks) {
    assert(s.gpr[3] == expect_r3 && s.gpr[4] == 0x00123450 && s.spr[272] == 0xabcd);
    assert(reads == 1 && last_read == 0xc0 && sprg0_writes == 1 && rfis == 1);
    assert(s.pc == 0x80004000 && s.msr == 0x30);
  }
}
int main(void) {
  assert(images && image_count == 3);
  assert(images[1].flags == AOT_IMAGE_REAL_MODE && images[2].flags == AOT_IMAGE_REAL_MODE);
  assert(images[1].base == 0x900 && images[0].flags == 0);
  for (uint32_t w = 0; w < images[1].word_count; w++) assert(images[1].words[w].addr - 0x80000900u < 16);
  assert(images[1].word_count == 4);
  vector(0, 7, 0);                  /* IR=DR=0: translated vector */
  vector(0x30, 0, 1);               /* IR set: 0x900 is not this code -> interpreter */
  vec[1] = 0x38600008; ++generation;  /* the other DOL's vectors */
  vector(0, 8, 0);
  vec[1] = 0x38600009; ++generation;  /* unknown vectors: interpreter */
  vector(0, 0, 1);
  return 0;
}
''')
        for harness_mode in (0, 1):
            run('clang', '-O2', '-std=c2x', '-Wno-deprecated-non-prototype',
                f'-DAOT_HARNESS={harness_mode}', '-I' + str(out),
                *map(str, sorted(out.glob('*.c'))), '-o', str(root / 'test'))
            run(str(root / 'test'))
        print('PASS: real-mode image discovery, slow-path memory, dedup, IR-gated dispatch')


if __name__ == '__main__':
    main()
