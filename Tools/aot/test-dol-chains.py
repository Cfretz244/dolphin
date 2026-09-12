#!/usr/bin/env python3
"""ROM-free native tests for guarded same-cache-line fallthrough chains."""
import pathlib
import sqlite3
import struct
import subprocess
import sys
import tempfile


def run(*args):
    p = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode:
        raise AssertionError(f'{args}: exit {p.returncode}\n{p.stdout}')
    return p.stdout


def main():
    tool = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='aot-dol-chains-') as temp:
        root = pathlib.Path(temp)
        iso = bytearray(0x4000)
        iso[:6] = b'TSTE01'
        for offset, value in [(0x1c, 0xc2339f3d), (0x420, 0x1000), (0x424, 0x2000),
                              (0x428, 32), (0x42c, 32)]:
            struct.pack_into('>I', iso, offset, value)
        for offset, add in [(0x1000, 2), (0x3000, 8)]:
            for field, value in [(0, 0x100), (0x48, 0x80004000), (0x90, 24), (0xe0, 0x80004000)]:
                struct.pack_into('>I', iso, offset + field, value)
            words = [0x38600001, 0x41820014, 0x38630000 | add, 0x4182000c,
                     0x38630004, 0x4e800020]
            struct.pack_into('>6I', iso, offset + 0x100, *words)
        struct.pack_into('>6I', iso, 0x2000, 0x01000000, 0, 2, 0, 0x3000, 0x118)
        iso[0x2018:0x2020] = b'alt.dol\0'
        disc = root / 'test.iso'
        disc.write_bytes(iso)
        trace = bytearray(struct.pack('<7I', 0x54485044, 4, 1, 0, 0, 0, 36))
        trace += struct.pack('<4I', 0x80004000, 8, 0x80004000, 1)
        trace += struct.pack('<3IQ2I', 2, 1, 1, 1, 0x38600001, 0x41820014)
        capture = root / 'test.dpht'
        capture.write_bytes(trace)
        cfg, out = root / 'test.db', root / 'generated'
        run(tool, 'cfg', '--iso', str(disc), '--trace', str(capture), '--dol-images', '--output', str(cfg))
        with sqlite3.connect(cfg) as db:
            assert db.execute('select pc,count from dol_image_blocks where image_id=0 order by pc').fetchall() == [
                (0x80004000, 2), (0x80004008, 2), (0x80004010, 2)]
        log = run(tool, 'translate', '--guarded-images', '--iso', str(disc), '--cfg', str(cfg), '--output', str(out))
        assert 'chains at 2 native entries' in log, log
        generated = ''.join(p.read_text() for p in out.glob('*blocks*.c'))
        assert 'aot_match_chain(0x80004000u' in generated
        assert 'chain-inlined fallthrough: 0x80004008u' in generated
        assert 'chain-inlined fallthrough: 0x80004010u' not in generated  # return ends the proof
        harness = out / 'test.c'
        harness.write_text(r'''
#include <assert.h>
#include "TSTE01_images.h"
extern AOTBlockFunc TSTE01_lookup_block(uint32_t);
int aot_single_block_mode;
static uint32_t code[]={0x38600001,0x41820014,0x38630002,0x4182000c,0x38630004,0x4e800020};
static unsigned guards, fallbacks;
static int resident=1;
int aot_match_code(uint32_t pc,const uint32_t* words,uint32_t count) {
  ++guards;
  if(pc<0x80004000u || pc+count*4>0x80004018u) return 0;
  for(unsigned i=0;i<count;++i) if(code[(pc-0x80004000u)/4+i]!=words[i]) return 0;
  return 1;
}
int aot_match_chain(uint32_t pc,const uint32_t* words,uint32_t count) {
  return resident && aot_match_code(pc,words,count);
}
void aot_interpreter_single_step(AOTState* s) { ++fallbacks; s->downcount=0; }
void aot_register_game(const char* id, void (*d)(AOTState*), AOTBlockFunc (*l)(uint32_t),uint32_t v) {}
void aot_register_game_image(const char* id,const char* h) {}
void aot_register_image_block_sizes(const char* id,uint32_t (*f)(uint32_t)) {}
static AOTState state(int count,int eq) {
  AOTState s={0}; s.pc=0x80004000u;s.downcount=count;s.spr[8]=0x80004018u;
  aot_cr_set_field(&s,0,eq?2:0);return s;
}
int main(void) {
  AOTBlockFunc entry=TSTE01_lookup_block(0x80004000u);assert(entry);
  AOTState s=state(100,0);guards=fallbacks=0;entry(&s);
  assert(s.gpr[3]==7 && fallbacks==1 && guards==2); // one guard shared by first two bodies
  s=state(2,0);guards=fallbacks=0;entry(&s);
  assert(s.pc==0x80004008u && s.gpr[3]==1 && s.downcount==0 && guards==1 && !fallbacks);
  s=state(100,0);s.exceptions=1;entry(&s);assert(s.pc==0x80004008u && s.gpr[3]==1);
  s=state(100,1);guards=fallbacks=0;entry(&s);
  assert(s.gpr[3]==1 && fallbacks==1 && guards==1); // taken branch exits before inline body
#if AOT_HARNESS
  s=state(100,0);aot_single_block_mode=1;entry(&s);aot_single_block_mode=0;
  assert(s.pc==0x80004008u && s.gpr[3]==1 && s.downcount==98);
#endif
  code[2]=0x38630008;s=state(100,0);entry(&s);
  assert(s.gpr[3]==13); // stale entry redirects when a later word in the chain changes
  resident=0;s=state(100,0);fallbacks=0;TSTE01_dispatch(&s);
  assert(s.gpr[3]==0 && fallbacks==1); // no fused native execution without residency
  return 0;
}
''')
        for mode in (0, 1):
            run('clang', '-O2', '-std=c2x', f'-DAOT_HARNESS={mode}', '-I'+str(out),
                *map(str, sorted(out.glob('*.c'))), '-o', str(root/'test'))
            run(str(root/'test'))
        # Cache/mapping mutators cannot be crossed by a shared guard.
        for name, opcode in [('icbi', 0x7c0007ac), ('mtmsr', 0x7c000124)]:
            modified = bytearray(iso)
            for offset in (0x1000, 0x3000):
                struct.pack_into('>I', modified, offset + 0x108, opcode)
            image = root / (name + '.iso')
            image.write_bytes(modified)
            db, output = root / (name + '.db'), root / name
            run(tool, 'cfg', '--iso', str(image), '--trace', str(capture), '--dol-images', '--output', str(db))
            run(tool, 'translate', '--guarded-images', '--iso', str(image), '--cfg', str(db), '--output', str(output))
            text = ''.join(p.read_text() for p in output.glob('*blocks*.c'))
            assert 'aot_match_chain(0x80004000u' not in text, name
        # Identical safe code at the end of a line cannot share the next line.
        shifted = bytearray(iso)
        for offset in (0x1000, 0x3000):
            for field in (0x48, 0xe0):
                struct.pack_into('>I', shifted, offset + field, 0x80004018)
        shifted_trace = bytearray(trace)
        for offset in (28, 36):
            struct.pack_into('<I', shifted_trace, offset, 0x80004018)
        (root/'shifted.iso').write_bytes(shifted)
        (root/'shifted.dpht').write_bytes(shifted_trace)
        run(tool, 'cfg', '--iso', str(root/'shifted.iso'), '--trace', str(root/'shifted.dpht'),
            '--dol-images', '--output', str(root/'shifted.db'))
        run(tool, 'translate', '--guarded-images', '--iso', str(root/'shifted.iso'), '--cfg', str(root/'shifted.db'),
            '--output', str(root/'shifted'))
        text = ''.join(p.read_text() for p in (root/'shifted').glob('*blocks*.c'))
        assert 'aot_match_chain(0x80004018u' not in text
        print('PASS: shared resident guard, fallthrough/taken edges, timing, exceptions, harness boundaries, and image switching')


if __name__ == '__main__':
    main()
