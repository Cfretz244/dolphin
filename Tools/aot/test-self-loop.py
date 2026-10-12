#!/usr/bin/env python3
"""ROM-free regression for self-loop blocks emitted as a C goto loop (sl1): a
trusted multi-image library whose hot block is `fadds f1,f1,f2; stw r3,0(r4);
addi r4,r4,4; addi r3,r3,K; bdnz <self>` keeps its register cache across
iterations (`goto _loop;`, no musttail to itself). The loop must give the same
architectural state as the interpreter semantics in chained mode, when the
downcount or a pending exception stops it at the back-edge (state written back,
pc = loop head, resumable), at an FP-unavailable exit at the top of a later
iteration, and in harness single-block mode (one iteration per call).
Usage: python3 Tools/aot/test-self-loop.py /absolute/path/to/dolphin-tool
"""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


def run(*args, ok=True, env=None):
    p = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    if (p.returncode == 0) != ok:
        raise AssertionError(f'{args}: exit {p.returncode}\n{p.stdout}')
    return p.stdout


def code(k):
    # fadds f1,f1,f2 / stw r3,0(r4) / addi r4,r4,4 / addi r3,r3,K / bdnz 0x80004000 / blr
    return [0xEC21102A, 0x90640000, 0x38840004, 0x38630000 | k, 0x4200FFF0, 0x4E800020]


def make_dol(k):
    data = bytearray(0x118)
    for offset, word in [(0, 0x100), (0x48, 0x80004000), (0x90, 0x18), (0xE0, 0x80004000)]:
        struct.pack_into('>I', data, offset, word)
    struct.pack_into('>6I', data, 0x100, *code(k))
    return data


HARNESS = r'''
#include <assert.h>
#include <string.h>
#include "aot_images.h"
extern void TSTE01_dispatch(AOTState*);
static uint32_t words[6];
static uint64_t generation = 1;
static const AotImageDesc* images; static uint32_t image_count; static uint32_t images_version;
static int fallbacks, slow_stores, clear_fp_at;
static uint32_t mem[16];
int aot_single_block_mode;
AotFastMem aot_fast_mem;  /* no RAM mapping: every store takes the slow path below */
AotActiveImage aot_active_image;
const uint64_t* aot_images_generation = &generation;
uint64_t aot_images_seen;
static uint32_t rd(uint32_t a) { return a - 0x80004000u < 24 ? words[(a - 0x80004000u) / 4] : 0; }
void aot_images_rescan(void) {
  aot_images_seen = generation; aot_active_image = (AotActiveImage){0, 0, 0};
  for (uint32_t i = 0; i < image_count; i++) {
    int ok = images[i].word_count > 0;
    for (uint32_t w = 0; w < images[i].word_count; w++) ok &= rd(images[i].words[w].addr) == images[i].words[w].word;
    if (ok) { aot_active_image = (AotActiveImage){images[i].base, images[i].size, images[i].table}; return; }
  }
}
AOTBlockFunc aot_images_lookup(uint32_t pc) {
  if (generation != aot_images_seen) aot_images_rescan();
  uint32_t idx = (pc - aot_active_image.base) >> 2;
  return idx < aot_active_image.size ? aot_active_image.table[idx] : 0;
}
uint32_t aot_images_block_size(uint32_t pc) { return aot_images_lookup(pc) ? 5 : 0; }
void aot_interpreter_single_step(AOTState* s) { ++fallbacks; s->downcount = 0; }
void aot_register_game(const char* id, void (*d)(AOTState*), AOTBlockFunc (*l)(uint32_t), uint32_t v) {}
void aot_register_game_image(const char* id, const char* h) {}
void aot_register_game_flags(const char* id, uint32_t f) {}
void aot_register_image_block_sizes(const char* id, uint32_t (*lookup)(uint32_t)) {}
void aot_register_game_images(const char* id, const AotImageDesc* i, uint32_t n, uint32_t v) { images = i; image_count = n; images_version = v; }
AOT_SLOWPATH_CC void aot_write_u32_slow(AOTState* s, uint32_t val, uint32_t addr) {
  ++slow_stores; assert(addr - 0x80001000u < sizeof mem); mem[(addr - 0x80001000u) / 4] = val;
  if (slow_stores == clear_fp_at) s->msr &= ~AOT_MSR_FP;  /* next iteration's MSR.FP check fails */
}
/* FP unavailable: the runtime raises the exception and leaves pc at the instruction */
int aot_check_fpu(AOTState* s, uint32_t pc) { assert(!(s->msr & AOT_MSR_FP)); s->pc = pc; return 0; }
/* never reached: the operands are finite, the gather pipe is unused */
AOT_SLOWPATH_CC void aot_faddsx(AOTState* s, int fd, int fa, int fb) { assert(0); }
AOT_SLOWPATH_CC void aot_gp_flush(AOTState* s) { assert(0); }
void aot_gp_capture(uint32_t physical, uint32_t val, uint32_t size) { assert(0); }
static double f64(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static uint64_t b64(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static AOTState start(int downcount) {
  AOTState s; memset(&s, 0, sizeof s);
  s.pc = 0x80004000u; s.downcount = downcount; s.msr = AOT_MSR_FP;
  s.spr[8] = 0x80005000u;  /* blr target: no block -> interpreter */
  s.spr[9] = 4; s.gpr[4] = 0x80001000u;
  s.ps[1].ps0 = s.ps[1].ps1 = b64(1.0); s.ps[2].ps0 = s.ps[2].ps1 = b64(0.5);
  memset(mem, 0, sizeof mem); fallbacks = slow_stores = clear_fp_at = 0;
  return s;
}
static void check_iter(const AOTState* s, uint32_t n, uint32_t k) {
  assert(s->gpr[3] == n * k && s->gpr[4] == 0x80001000u + 4 * n && s->spr[9] == 4 - n);
  assert(f64(s->ps[1].ps0) == 1.0 + 0.5 * n && f64(s->ps[1].ps1) == 1.0 + 0.5 * n);
  for (uint32_t i = 0; i < 4; i++) assert(mem[i] == (i < n ? i * k : 0));
}
static void scenario(uint32_t k) {
  /* chained: four iterations in one call, then blr -> dispatch -> interpreter */
  AOTState s = start(1000); TSTE01_dispatch(&s);
  check_iter(&s, 4, k); assert(fallbacks == 1 && slow_stores == 4);
  uint32_t fpscr = s.fpscr;
  /* downcount runs out at the second back-edge: written back, pc = head, resumable */
  s = start(7); TSTE01_dispatch(&s);
  check_iter(&s, 2, k); assert(s.pc == 0x80004000u && s.downcount <= 0 && !fallbacks);
  s.downcount = 1000; TSTE01_dispatch(&s);
  check_iter(&s, 4, k); assert(fallbacks == 1 && s.fpscr == fpscr);
  /* a pending program exception stops the first back-edge */
  s = start(1000); s.exceptions = 2; TSTE01_dispatch(&s);
  check_iter(&s, 1, k); assert(s.pc == 0x80004000u && !fallbacks);
  /* MSR.FP cleared during iteration 2: the exit at the top of iteration 3 (before any
     instruction of that iteration defines a register) must still write back every
     loop-carried register */
  s = start(1000); clear_fp_at = 2; TSTE01_dispatch(&s);
  check_iter(&s, 2, k); assert(s.pc == 0x80004000u && !fallbacks);
#if AOT_HARNESS
  /* single-block mode (the diff harness calls the block directly): one iteration per call */
  AOTBlockFunc head = aot_images_lookup(0x80004000u); assert(head);
  s = start(1000); aot_single_block_mode = 1;
  for (uint32_t n = 1; n <= 3; n++) { head(&s); check_iter(&s, n, k); assert(s.pc == 0x80004000u); }
  aot_single_block_mode = 0;
#endif
}
int main(void) {
  assert(images && image_count == 2 && images_version == AOT_IMAGES_VERSION);
  static const uint32_t c1[6] = {CODE1}, c2[6] = {CODE2};
  memcpy(words, c1, sizeof words); scenario(1);
  memcpy(words, c2, sizeof words); ++generation; scenario(2);
  return 0;
}
'''


def main():
    tool = str(pathlib.Path(sys.argv[1]).resolve())
    env = {k: v for k, v in os.environ.items() if k != 'AOT_NO_SUPERBLOCKS'}
    # Slow-path counting contract: the harness has no RAM (aot_fast_mem zeroed) and counts
    # aot_*_slow calls, so fastmem's direct window access is incompatible by design. The
    # fastmem memory path is covered by test-fastmem.py and the ROM gates.
    env['AOT_FASTMEM'] = '0'
    with tempfile.TemporaryDirectory(prefix='aot-self-loop-') as temp:
        root = pathlib.Path(temp)
        iso = bytearray(0x4000)
        iso[:6] = b'TSTE01'
        for offset, value in [(0x1C, 0xC2339F3D), (0x420, 0x1000), (0x424, 0x2000),
                              (0x428, 32), (0x42C, 32)]:
            struct.pack_into('>I', iso, offset, value)
        iso[0x1000:0x1118] = make_dol(1)
        iso[0x3000:0x3118] = make_dol(2)
        struct.pack_into('>6I', iso, 0x2000, 0x01000000, 0, 2, 0, 0x3000, 0x118)
        iso[0x2018:0x2020] = b'alt.dol\0'
        disc = root / 'test.iso'
        disc.write_bytes(iso)
        trace = bytearray(struct.pack('<7I', 0x54485044, 4, 1, 0, 0, 0, 36))
        trace += struct.pack('<2I', 0x80004000, 0x18)
        trace += struct.pack('<2I', 0x80004000, 2)
        for i in (1, 2):
            trace += struct.pack('<3IQ6I', 6, i, 1, i, *code(i))
        (root / 'test.dpht').write_bytes(trace)
        cfg = root / 'test.db'
        run(tool, 'cfg', '--iso', str(disc), '--trace', str(root / 'test.dpht'), '--dol-images',
            '--output', str(cfg), env=env)
        out = root / 'generated'
        out.mkdir()
        (out / 'translate.log').touch()
        run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output', str(out), env=env)
        text = ''.join(p.read_text() for p in out.glob('*blocks*.c'))
        assert text.count('goto _loop;') == 2, text  # one per image
        assert 'return TSTE01_d0_block_80004000(s)' not in text, text
        harness = out / 'test.c'
        harness.write_text(HARNESS.replace('CODE1', ','.join(map(hex, code(1))))
                           .replace('CODE2', ','.join(map(hex, code(2)))))
        for harness_mode in (0, 1):
            run('clang', '-O2', '-std=c2x', '-Wno-deprecated-non-prototype',
                f'-DAOT_HARNESS={harness_mode}', '-I' + str(out),
                *map(str, sorted(out.glob('*.c'))), '-o', str(root / 'test'))
            run(str(root / 'test'))
        print('PASS: self-loop goto emission, chained iterations, downcount/exception stop + resume, '
              'single-block mode')


if __name__ == '__main__':
    main()
