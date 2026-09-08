#!/usr/bin/env python3
"""ROM-free integration regression for alternate DOL CFG + generated native dispatch.
Usage: python3 Tools/aot/test-dol-images.py /absolute/path/to/dolphin-tool
Requires macOS ARM64 and clang (same environment as the stack's AOT emitter).
"""
import pathlib
import sqlite3
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
    with tempfile.TemporaryDirectory(prefix='aot-dol-images-') as temp:
        root = pathlib.Path(temp)
        iso = bytearray(0x4000)
        iso[:6] = b'TSTE01'
        # GC disc with a boot DOL and one alternate DOL in its filesystem.
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
        capture = root / 'test.dpht'
        capture.write_bytes(trace)
        cfg = root / 'test.db'
        run(tool, 'cfg', '--iso', str(disc), '--trace', str(capture), '--dol-images', '--output', str(cfg))
        with sqlite3.connect(cfg) as db:
            assert db.execute('select count(*) from dol_images').fetchone()[0] == 2
            assert db.execute('select count(*) from dol_image_blocks').fetchone()[0] == 2
        # Existing captures/DBs must never be silently overwritten.
        run(tool, 'cfg', '--iso', str(disc), '--trace', str(capture), '--dol-images', '--output', str(cfg), ok=False)
        out = root / 'generated'
        out.mkdir()
        (out / 'translate.log').touch()  # stack.sh creates this before invoking translate
        run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output', str(out))
        harness = out / 'test.c'
        harness.write_text(r'''
#include <assert.h>
#include "TSTE01_images.h"
extern AOTBlockFunc TSTE01_lookup_block(uint32_t);
static uint32_t code[] = {0x38600001, 0x4e800020};
static int fallbacks;
static uint32_t (*block_size)(uint32_t);
int aot_single_block_mode;
int aot_match_code(uint32_t pc, const uint32_t* words, uint32_t count) {
  if (pc != 0x80004000 || count != 2) return 0;
  return words[0] == code[0] && words[1] == code[1];
}
void aot_interpreter_single_step(AOTState* s) { ++fallbacks; s->downcount=0; }
void aot_register_game(const char* id, void (*d)(AOTState*), AOTBlockFunc (*l)(uint32_t), uint32_t v) {}
void aot_register_game_image(const char* id, const char* h) {}
void aot_register_image_block_sizes(const char* id, uint32_t (*lookup)(uint32_t)) { block_size=lookup; }
static void execute(AOTBlockFunc fn, uint32_t expected) {
  AOTState s = {0}; s.pc=0x80004000; s.downcount=100; s.spr[8]=0x80004008;
  fallbacks=0; fn(&s); assert(s.gpr[3]==expected); assert(fallbacks==1);
}
int main(void) {
  assert(!TSTE01_lookup_block(0x80004001));
  assert(!TSTE01_lookup_block(0x80005000));
  assert(block_size && block_size(0x80004000)==2);
  execute(TSTE01_dispatch,1);
  AOTBlockFunc stale = TSTE01_lookup_block(0x80004000);
  code[0]=0x38600002;
  execute(TSTE01_dispatch,2);
  execute(stale,2); // direct entry into old image must redirect before executing it
  code[0]=0x38600003;
  assert(!TSTE01_lookup_block(0x80004000));
  AOTState s={0}; s.pc=0x80004000;s.downcount=100;
  fallbacks=0;TSTE01_dispatch(&s);assert(fallbacks==1 && s.gpr[3]==0);
  code[0]=0x38600001; execute(TSTE01_dispatch,1); // reload/save-state reversal
  code[1]=0x60000000; assert(!TSTE01_lookup_block(0x80004000)); // partial image
  return 0;
}
''')
        for harness_mode in (0, 1):
            run('clang', '-O2', '-std=c2x', '-Wno-deprecated-non-prototype',
                f'-DAOT_HARNESS={harness_mode}', '-I'+str(out),
                *map(str, sorted(out.glob('*.c'))), '-o', str(root/'test'))
            run(str(root/'test'))
        # Exercise the shipping mix of ThinLTO blocks and a native dispatch
        # object, as well as the direct-source harness builds above.
        run('bash', str(out/'build.sh'))
        run('clang', '-O2', '-std=c2x', '-DAOT_HARNESS=1', '-I'+str(out), str(harness),
            '-Wl,-force_load,'+str(out/'libTSTE01_aot.a'), '-o', str(root/'archive-test'))
        run(str(root/'archive-test'))
        # Legacy JIT scheduling can reorder before the first terminator. Keep
        # the matching prefix as a seed, decode disc words, and follow branches.
        reordered_iso = bytearray(iso)
        reordered_trace = bytearray(struct.pack('<7I', 0x54485044, 4, 1, 0, 0, 0, 36))
        reordered_trace += struct.pack('<4I', 0x80004000, 24, 0x80004000, 2)
        for i, offset in ((1, 0x1000), (2, 0x3000)):
            words = [0x38600000 | i, 0x38800004, 0x38A00005, 0x38C00006, 0x48000004, 0x4E800020]
            struct.pack_into('>I', reordered_iso, offset + 0x90, 24)
            struct.pack_into('>6I', reordered_iso, offset + 0x100, *words)
            words[4], words[5] = words[5], words[4]
            reordered_trace += struct.pack('<3IQ6I', 6, i, 1, i, *words)
        struct.pack_into('>I', reordered_iso, 0x2014, 0x118)
        (root/'reordered.iso').write_bytes(reordered_iso)
        (root/'reordered.dpht').write_bytes(reordered_trace)
        run(tool, 'cfg', '--iso', str(root/'reordered.iso'), '--trace', str(root/'reordered.dpht'),
            '--dol-images', '--output', str(root/'reordered.db'))
        with sqlite3.connect(root/'reordered.db') as db:
            assert db.execute('select pc,count from dol_image_blocks where image_id=0 order by pc').fetchall() == [
                (0x80004000, 5), (0x80004014, 1)]
        # The alternate image must be pinned even when boot DOL is unchanged.
        iso[0x3103] = 3
        disc.write_bytes(iso)
        text = run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output', str(root/'bad'), ok=False)
        assert 'identity mismatch' in text
        print('PASS: alternate DOL attribution, native selection, stale direct entry, unknown/partial code, reload, and image pinning')


if __name__ == '__main__':
    main()
