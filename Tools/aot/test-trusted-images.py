#!/usr/bin/env python3
"""ROM-free regression for the trusted multi-image translation (the default for
--dol-images CFGs). Two DOLs share 0x80004000 with different `li r3` words; the
generated dispatch must run whichever image a (simulated) tracker selects, with
no per-block instruction guards in the emitted code.
Usage: python3 Tools/aot/test-trusted-images.py /absolute/path/to/dolphin-tool
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


def main():
    tool = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='aot-trusted-images-') as temp:
        root = pathlib.Path(temp)
        iso = bytearray(0x4000)
        iso[:6] = b'TSTE01'
        for offset, value in [(0x1C, 0xC2339F3D), (0x420, 0x1000), (0x424, 0x2000),
                              (0x428, 32), (0x42C, 32)]:
            struct.pack_into('>I', iso, offset, value)
        iso[0x1000:0x1108] = make_dol(1)
        iso[0x3000:0x3108] = make_dol(2)
        struct.pack_into('>6I', iso, 0x2000, 0x01000000, 0, 2, 0, 0x3000, 0x108)
        iso[0x2018:0x2020] = b'alt.dol\0'
        disc = root / 'test.iso'
        disc.write_bytes(iso)
        trace = bytearray(struct.pack('<7I', 0x54485044, 4, 1, 0, 0, 0, 36))
        trace += struct.pack('<2I', 0x80004000, 8)
        trace += struct.pack('<2I', 0x80004000, 2)
        for i in (1, 2):
            trace += struct.pack('<3IQ3I', 3, i, 1, i, 0x38600000 | i, 0x4E800020, 0xDEADBEEF)
        (root / 'test.dpht').write_bytes(trace)
        cfg = root / 'test.db'
        run(tool, 'cfg', '--iso', str(disc), '--trace', str(root / 'test.dpht'), '--dol-images',
            '--output', str(cfg))
        out = root / 'generated'
        out.mkdir()
        (out / 'translate.log').touch()
        log = run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output', str(out))
        assert 'Trusted multi-image library' in log, log
        assert 'aot_match_code' not in ''.join(p.read_text() for p in out.glob('*.c'))
        # --images restricts emission to the listed base ids.
        one = root / 'one'
        one.mkdir()
        run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output', str(one),
            '--images', '1')
        assert not list(one.glob('TSTE01_blocks_d0_*.c')) and list(one.glob('TSTE01_blocks_d1_*.c'))
        harness = out / 'test.c'
        harness.write_text(r'''
#include <assert.h>
#include <string.h>
#include "aot_images.h"
extern void TSTE01_dispatch(AOTState*);
extern AOTBlockFunc TSTE01_lookup_block(uint32_t);
/* Simulated tracker: one RAM word at 0x80004000 plus the shared blr. */
static uint32_t ram[2] = {0x38600001, 0x4e800020};
static uint64_t generation = 1;
static const AotImageDesc* images; static uint32_t image_count; static uint32_t images_version;
static uint32_t (*block_size)(uint32_t);
static int fallbacks, rescans;
int aot_single_block_mode;
AotActiveImage aot_active_image;
const uint64_t* aot_images_generation = &generation;
uint64_t aot_images_seen;
static uint32_t rd(uint32_t a) { return (a - 0x80004000u) / 4 < 2 ? ram[(a - 0x80004000u) / 4] : 0; }
void aot_images_rescan(void) {
  ++rescans; aot_images_seen = generation; aot_active_image = (AotActiveImage){0, 0, 0};
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
uint32_t aot_images_block_size(uint32_t pc) { return aot_images_lookup(pc) ? 2 : 0; }
void aot_interpreter_single_step(AOTState* s) { ++fallbacks; s->downcount = 0; }
void aot_register_game(const char* id, void (*d)(AOTState*), AOTBlockFunc (*l)(uint32_t), uint32_t v) {}
void aot_register_game_image(const char* id, const char* h) {}
void aot_register_image_block_sizes(const char* id, uint32_t (*lookup)(uint32_t)) { block_size = lookup; }
void aot_register_game_images(const char* id, const AotImageDesc* i, uint32_t n, uint32_t v) { images = i; image_count = n; images_version = v; }
static void execute(uint32_t expected, int expect_fallbacks) {
  AOTState s; memset(&s, 0, sizeof s); s.pc = 0x80004000; s.downcount = 100; s.spr[8] = 0x80004008;
  fallbacks = 0; TSTE01_dispatch(&s); assert(s.gpr[3] == expected); assert(fallbacks == expect_fallbacks);
}
int main(void) {
  assert(images && image_count == 2 && images_version == AOT_IMAGES_VERSION);
  assert(images[0].word_count >= 1 && images[1].word_count >= 1);
  assert(images[0].table[0] && images[1].table[0] && images[0].table[0] != images[1].table[0]);
  execute(1, 1); assert(rescans == 1);
  execute(1, 1); assert(rescans == 1);           /* no invalidation: no rescan */
  ram[0] = 0x38600002; ++generation;              /* alternate image loaded + icbi */
  execute(2, 1); assert(rescans == 2);
  assert(TSTE01_lookup_block(0x80004000) == images[1].table[0]);
  ram[0] = 0x38600003; ++generation;              /* unknown code: interpreter */
  execute(0, 1); assert(aot_active_image.table == 0);
  assert(!TSTE01_lookup_block(0x80004000));
#if AOT_HARNESS
  assert(block_size && block_size(0x80004000) == 0);
#endif
  ram[0] = 0x38600001; ++generation;              /* reload / savestate */
  execute(1, 1); assert(aot_active_image.table == images[0].table);
  return 0;
}
''')
        for harness_mode in (0, 1):
            run('clang', '-O2', '-std=c2x', '-Wno-deprecated-non-prototype',
                f'-DAOT_HARNESS={harness_mode}', '-I' + str(out),
                *map(str, sorted(out.glob('*.c'))), '-o', str(root / 'test'))
            run(str(root / 'test'))
        run('bash', str(out / 'build.sh'))
        run('clang', '-O2', '-std=c2x', '-DAOT_HARNESS=1', '-I' + str(out), str(harness),
            '-Wl,-force_load,' + str(out / 'libTSTE01_aot.a'), '-o', str(root / 'archive-test'))
        run(str(root / 'archive-test'))
        print('PASS: trusted multi-image emission, tracker-driven selection, --images filter')


if __name__ == '__main__':
    main()
