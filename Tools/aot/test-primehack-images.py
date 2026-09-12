#!/usr/bin/env python3
"""ROM-free regression for verified PrimeHack variants and native patch switching."""
import pathlib
import sqlite3
import struct
import subprocess
import sys
import tempfile


PATCHES = [
    (0x80098ee4, 0xec000072, 0xec010072),
    (0x80099138, 0x4bffe6dd, 0x60000000),
    (0x80183a8c, 0xd03f03dc, 0x60000000),
    (0x80183a64, 0xd03f03dc, 0x60000000),
    (0x8017661c, 0x901f0118, 0x60000000),
    (0x802fb5b4, 0xd03f009c, 0xd23f009c),
    (0x8019fbcc, 0x4bea3ca9, 0x60000000),
    (0x8018b8d4, 0x41820014, 0x48000354),
]
BASE = 0x80090000
SIZE = 0x8046d344 - BASE


def run(*args, ok=True):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (result.returncode == 0) != ok:
        raise AssertionError(f'{args}: exit {result.returncode}\n{result.stdout}')
    return result.stdout


def main():
    tool = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='primehack-aot-') as temp:
        root = pathlib.Path(temp)
        code = bytearray(SIZE)
        def put(address, *words):
            struct.pack_into('>' + 'I' * len(words), code, address - BASE, *words)
        for address, original, replacement in PATCHES:
            put(address, original, 0x4e800020)
        put(0x8046d340, 0x4e800020)
        branch = PATCHES[-1][0]
        put(branch + 4, 0x38600001, 0x4e800020)
        put(branch + 0x14, 0x38600002, 0x4e800020)
        put(branch + 0x354, 0x38600003, 0x4e800020)
        dol = bytearray(0x100) + code
        for offset, value in [(0, 0x100), (0x48, BASE), (0x90, SIZE), (0xe0, branch)]:
            struct.pack_into('>I', dol, offset, value)
        # Synthetic container with the required title/revision and patch words;
        # no Nintendo disc bytes are used. Runtime still independently gates Wii mode.
        iso = bytearray(0x3000) + dol
        iso[:6] = b'R3ME01'
        for offset, value in [(0x1c, 0xc2339f3d), (0x420, 0x3000),
                              (0x424, 0x2000), (0x428, 32), (0x42c, 32)]:
            struct.pack_into('>I', iso, offset, value)
        struct.pack_into('>6I', iso, 0x2000, 0x01000000, 0, 2, 0, 0x2f00, 4)
        iso[0x2018:0x2020] = b'pad.bin\0'
        disc = root / 'test.iso'
        disc.write_bytes(iso)
        (root / 'code.bin').write_bytes(code)
        trace = bytearray(struct.pack('<7I', 0x54485044, 4, 1, 0, 0, 0, 36))
        trace += struct.pack('<2I', branch, 4)
        # A patched-only snapshot must seed discovery without becoming trusted code.
        trace += struct.pack('<5IQI', branch, 1, 1, 1, 1, 1, PATCHES[-1][2])
        capture = root / 'test.dpht'
        capture.write_bytes(trace)
        cfg = root / 'test.db'
        args = (tool, 'cfg', '--iso', str(disc), '--trace', str(capture),
                '--dol-images', '--primehack', '--output')
        run(*args, str(cfg))
        with sqlite3.connect(cfg) as db:
            assert db.execute('select count(*) from dol_images').fetchone()[0] == 5
            assert db.execute("select value from metadata where key='primehack_patch_set'").fetchone()[0] == 'mp1-r3me01-v1'
            assert db.execute('select count(*) from dol_image_blocks where image_id>0').fetchone()[0] > 0
        out = root / 'generated'
        run(tool, 'translate', '--guarded-images', '--iso', str(disc), '--cfg', str(cfg), '--output', str(out))
        harness = out / 'test.c'
        harness.write_text(r'''
#include <assert.h>
int aot_match_code(unsigned,const unsigned*,unsigned);
int aot_match_chain(unsigned pc,const unsigned* words,unsigned count) { return aot_match_code(pc,words,count); }
#include <stdio.h>
#include <stdlib.h>
#include "R3ME01_images.h"
extern AOTBlockFunc R3ME01_lookup_block(uint32_t);
static uint32_t code[0x400000/4];
static unsigned fallbacks;
int aot_single_block_mode;
AotFastMem aot_fast_mem;
int aot_match_code(uint32_t pc, const uint32_t* words, uint32_t count) {
  if (pc<0x80090000 || (pc&3) || count>(0x80490000u-pc)/4 || pc>=0x80490000u) return 0;
  for(unsigned i=0;i<count;++i) if(code[(pc-0x80090000)/4+i]!=words[i]) return 0;
  return 1;
}
void aot_interpreter_single_step(AOTState* s) { ++fallbacks; s->downcount=0; }
void aot_register_game(const char* id, void (*d)(AOTState*), AOTBlockFunc (*l)(uint32_t), uint32_t v) {}
void aot_register_game_image(const char* id, const char* h) {}
void aot_register_image_block_sizes(const char* id, uint32_t (*l)(uint32_t)) {}
int aot_check_fpu(AOTState* s, uint32_t pc) { return 1; }
void aot_fmulsx(AOTState* s,int d,int a,int c) { abort(); }
uint32_t aot_convert_to_single(uint64_t v) { abort(); }
void aot_write_u32_slow(AOTState* s,uint32_t v,uint32_t a) { abort(); }
static unsigned execute(AOTBlockFunc fn) {
  AOTState s={0}; s.pc=0x8018b8d4; s.downcount=100; s.spr[8]=0x80500000;
  fallbacks=0; fn(&s); assert(fallbacks==1); return s.gpr[3];
}
static const unsigned addresses[]={0x80098ee4,0x80099138,0x80183a8c,0x80183a64,0x8017661c,0x802fb5b4,0x8019fbcc,0x8018b8d4};
static const unsigned originals[]={0xec000072,0x4bffe6dd,0xd03f03dc,0xd03f03dc,0x901f0118,0xd03f009c,0x4bea3ca9,0x41820014};
static const unsigned replacements[]={0xec010072,0x60000000,0x60000000,0x60000000,0x60000000,0xd23f009c,0x60000000,0x48000354};
int main(int argc,char** argv) {
  FILE* f=fopen(argv[1],"rb"); assert(f); size_t n=fread(code,4,sizeof(code)/4,f); fclose(f);
  for(size_t i=0;i<n;++i) code[i]=__builtin_bswap32(code[i]);
  AOTBlockFunc stale=R3ME01_lookup_block(0x8018b8d4); assert(stale);
  unsigned original=execute(R3ME01_dispatch); assert(original==1 || original==2);
  unsigned masks[]={0xff,0x7f,0x1f,0x9f,0};
  for(unsigned m=0;m<5;++m) {
    for(unsigned i=0;i<8;++i) code[(addresses[i]-0x80090000)/4]=(masks[m]&(1u<<i))?replacements[i]:originals[i];
    for(unsigned i=0;i<8;++i) assert(R3ME01_lookup_block(addresses[i]));
    assert(execute(R3ME01_dispatch)==((masks[m]&0x80)?3:original));
    assert(execute(stale)==((masks[m]&0x80)?3:original));
  }
  code[(0x8018b8d4-0x80090000)/4]=0x48000008;
  assert(!R3ME01_lookup_block(0x8018b8d4));
  return 0;
}
''')
        for mode in (0, 1):
            run('clang', '-O2', '-std=c2x', f'-DAOT_HARNESS={mode}', '-I'+str(out),
                *map(str, out.glob('*.c')), '-o', str(root/'test'))
            run(str(root/'test'), str(root/'code.bin'))
        run('bash', str(out/'build.sh'))
        run('clang', '-O2', '-std=c2x', '-DAOT_HARNESS=1', '-I'+str(out), str(harness),
            '-Wl,-force_load,'+str(out/'libR3ME01_aot.a'), '-o', str(root/'archive-test'))
        run(str(root/'archive-test'), str(root/'code.bin'))
        # Both disc identity and the exact patch recipe must survive validation.
        with sqlite3.connect(cfg) as db:
            db.execute("update dol_images set sha256='bad' where id=1")
        assert 'identity mismatch' in run(tool, 'translate', '--guarded-images', '--iso', str(disc), '--cfg', str(cfg),
                                         '--output', str(root/'bad'), ok=False)
        iso[3] = ord('X')
        disc.write_bytes(iso)
        assert 'R3ME01' in run(*args, str(root/'wrong-title.db'), ok=False)
        iso[:6] = b'R3ME01'
        iso[0x3100 + PATCHES[0][0] - BASE] ^= 1
        disc.write_bytes(iso)
        assert 'patch words' in run(*args, str(root/'wrong-patch.db'), ok=False)
        print('PASS: verified PrimeHack variants, native mode switching, stale entry, restoration, unknown code, and identity rejection')


if __name__ == '__main__':
    main()
