#!/usr/bin/env python3
"""ROM-free regression for Wii-SDK RSO module translation (translate --rso-module).
A synthetic GameCube disc carries a DOL (0x80004000: li r3,1; blr), a static
module T/RSO/selfile.sel exporting ext_func (DOL text) and ext_data, and a module
T/RSO/RSO_Test.rso whose .text calls ext_func (external REL24), loads &ext_data
(external HA/LO), reads its own .data and takes the address of its .bss (internal
HA/LO), calls a local function (internal REL24) and carries a cross-module import
and an ADDR32 data word in .text. The translator must emit the module
base-relative (immediates from <sym>_base[section]), bind the external call to
the DOL block, patch external immediates with the static-module addresses, choose
identity words (unrelocated text + located external sites), register the
descriptor and end the dispatch in aot_rso_dispatch. The compiled library is then
run at two different module bases through a stub tracker.
Usage: python3 Tools/aot/test-rso-modules.py /absolute/path/to/dolphin-tool
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


def make_dol():
    data = bytearray(0x108)
    for offset, word in [(0, 0x100), (0x48, 0x80004000), (0x90, 8), (0xE0, 0x80004000),
                         (0x100, 0x38600001), (0x104, 0x4E800020)]:
        struct.pack_into('>I', data, offset, word)
    return data


# Module text (section 1, file offset 0x100). Offsets are relative to .text.
TEXT = [
    0x7C0802A6,  # 00 mflr r0                 (_prolog)
    0x48000001,  # 04 bl ext_func             external REL24, import 0
    0x3C800000,  # 08 lis r4,ext_data@ha      external HA, import 1
    0x38840000,  # 0c addi r4,r4,ext_data@l   external LO, import 1
    0x3CA00000,  # 10 lis r5,data@ha          internal HA -> section 2 + 0
    0x80A50000,  # 14 lwz r5,data@l(r5)       internal LO -> section 2 + 0
    0x3CC00000,  # 18 lis r6,bss@ha           internal HA -> section 3 + 8
    0x38C60000,  # 1c addi r6,r6,bss@l        internal LO -> section 3 + 8
    0x48000001,  # 20 bl local                internal REL24 -> section 1 + 0x2c
    0x7C0803A6,  # 24 mtlr r0
    0x4E800020,  # 28 blr
    0x38630010,  # 2c addi r3,r3,0x10         (local)
    0x4E800020,  # 30 blr
    0x00000000,  # 34 .long ext_data          external ADDR32 (data in text)
    0x48000001,  # 38 bl other_func           cross-module import 2 (never executed)
    0x4E800020,  # 3c blr
]
R_ADDR32, R_LO, R_HA, R_REL24 = 1, 4, 6, 10


def make_rso(name, nsec, sections, text, data, internals, externals, exports, imports):
    """sections: {index: (offset, size)}; returns the file bytes (header at 0)."""
    f = bytearray(0x400)
    for i, (off, size) in sections.items():
        struct.pack_into('>2I', f, 0x58 + 8 * i, off, size)
    if text:
        for i, w in enumerate(text):
            struct.pack_into('>I', f, sections[1][0] + 4 * i, w)
    if data:
        f[sections[2][0]:sections[2][0] + len(data)] = data
    pos = 0x200
    name_off = pos
    f[pos:pos + len(name) + 1] = name.encode() + b'\0'
    pos += 0x30

    def table(records, fmt):
        nonlocal pos
        off = pos
        for r in records:
            struct.pack_into(fmt, f, pos, *r)
            pos += struct.calcsize(fmt)
        return off, pos - off

    int_off, int_size = table(internals, '>3I')
    ext_off, ext_size = table(externals, '>3I')
    names = bytearray()

    def name_index(n):
        o = len(names)
        names.extend(n.encode() + b'\0')
        return o

    exp_records = [(name_index(n), o, s, 0) for n, o, s in exports]
    exp_off, exp_size = table(exp_records, '>4I')
    exp_names = pos
    f[pos:pos + len(names)] = names
    pos += len(names)
    names = bytearray()
    imp_records = [(name_index(n), 0, 0) for n in imports]
    imp_off, imp_size = table(imp_records, '>3I')
    imp_names = pos
    f[pos:pos + len(names)] = names
    pos += len(names)
    struct.pack_into('>22I', f, 0, 0, 0, nsec, 0x58, name_off, len(name), 1, 0x20, 0x01010100,
                     0, 0, 0, int_off, int_size, ext_off, ext_size, exp_off, exp_size, exp_names,
                     imp_off, imp_size, imp_names)
    return bytes(f[:pos])


def make_fst(files):
    """files: [(name, offset, size)] inside T/RSO/. Returns the FST bytes."""
    strings = bytearray(b'\0')

    def s(n):
        o = len(strings)
        strings.extend(n.encode() + b'\0')
        return o

    count = 3 + len(files)
    entries = [(0x01000000, 0, count), (0x01000000 | s('T'), 0, count),
               (0x01000000 | s('RSO'), 1, count)]
    entries += [(s(n), off, size) for n, off, size in files]
    return b''.join(struct.pack('>3I', *e) for e in entries) + bytes(strings)


def main():
    tool = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='aot-rso-modules-') as temp:
        root = pathlib.Path(temp)
        rso = make_rso(
            'c:\\T\\RSO\\RSO_Test.plf', 10, {1: (0x100, 4 * len(TEXT)), 2: (0x140, 0x10), 3: (0, 0x20)},
            TEXT, struct.pack('>I', 0x11223344),
            internals=[(0x112, (2 << 8) | R_HA, 0), (0x116, (2 << 8) | R_LO, 0),
                       (0x11A, (3 << 8) | R_HA, 8), (0x11E, (3 << 8) | R_LO, 8),
                       (0x120, (1 << 8) | R_REL24, 0x2C)],
            externals=[(0x104, (0 << 8) | R_REL24, 0), (0x10A, (1 << 8) | R_HA, 0),
                       (0x10E, (1 << 8) | R_LO, 0), (0x134, (1 << 8) | R_ADDR32, 0),
                       (0x138, (2 << 8) | R_REL24, 0)],
            exports=[('_prolog', 0, 1)], imports=['ext_func', 'ext_data', 'other_func'])
        sel = make_rso('c:\\T\\Test.elf', 10, {}, None, None, [], [],
                       exports=[('ext_func', 0, 1), ('ext_data', 0x40, 2)], imports=[])
        iso = bytearray(0x5000)
        iso[:6] = b'TSTE01'
        fst = make_fst([('RSO_Test.rso', 0x3000, len(rso)), ('selfile.sel', 0x4000, len(sel))])
        for offset, value in [(0x1C, 0xC2339F3D), (0x420, 0x1000), (0x424, 0x2000),
                              (0x428, len(fst)), (0x42C, len(fst))]:
            struct.pack_into('>I', iso, offset, value)
        iso[0x1000:0x1108] = make_dol()
        iso[0x2000:0x2000 + len(fst)] = fst
        iso[0x3000:0x3000 + len(rso)] = rso
        iso[0x4000:0x4000 + len(sel)] = sel
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
        static = root / 'static.txt'
        # ext_func's section = DOL text; ext_data's section base also inside the DOL.
        static.write_text('# sel section -> located address\n1 0x80004000\n2 0x80004004  # data\n')
        spec = f'0:T/RSO/RSO_Test.rso:{static}'
        out = root / 'generated'
        log = run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output', str(out),
                  '--rso-module', spec)
        assert 'RSO module TSTE01_rso0 (RSO_Test.plf, T/RSO/RSO_Test.rso)' in log, log
        assert '(1 cross-module import sites)' in log, log
        mod = (out / 'TSTE01_blocks_rso0.c').read_text()
        disp = (out / 'TSTE01_dispatch.c').read_text()
        # Internal immediates read the base slots; external ones are patched in.
        assert 'TSTE01_rso0_base[2]' in mod and 'TSTE01_rso0_base[3]+0x8u' in mod, mod
        # The external call binds straight to the owning DOL image's block.
        assert 'TSTE01_d0_block_80004000' in mod, mod
        assert 'return aot_rso_dispatch(s);' in disp, disp
        assert 'aot_register_game_rso_modules("TSTE01"' in disp, disp
        assert '"RSO_Test.plf",10u,' in disp, disp
        # Identity words: the located external HA/LO sites (ext_data = 0x80004044).
        assert '{0x108u,0x3c808000u}' in mod and '{0x10cu,0x38844044u}' in mod, mod
        # Errors: a static base outside the DOL, a duplicate module.
        bad = root / 'bad-static.txt'
        bad.write_text('1 0x80004000\n2 0x81000000\n')
        run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output',
            str(root / 'bad1'), '--rso-module', f'0:T/RSO/RSO_Test.rso:{bad}', ok=False)
        run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output',
            str(root / 'bad2'), '--rso-module', spec, '--rso-module', spec, ok=False)
        harness = out / 'test.c'
        harness.write_text(r'''
#include <assert.h>
#include <string.h>
#include "aot_images.h"
extern void TSTE01_dispatch(AOTState*);
static uint32_t dol[2] = {0x38600001, 0x4e800020};
static uint64_t generation = 1;
static const AotImageDesc* images; static uint32_t image_count;
static const AotRsoModuleDesc* rso; static uint32_t rso_count;
static uint32_t text_base, data_base, exit_pc; static int fallbacks, exits, reads;
int aot_single_block_mode;
AotFastMem aot_fast_mem;
AotActiveImage aot_active_image, aot_active_image_aux, aot_active_image_real;
const uint64_t* aot_images_generation = &generation;
uint64_t aot_images_seen;
static int matches(const AotImageDesc* i) {
  int ok = i->word_count > 0;
  for (uint32_t w = 0; w < i->word_count; w++)
    ok &= i->words[w].addr - 0x80004000u < 8 && dol[(i->words[w].addr - 0x80004000u) / 4] == i->words[w].word;
  return ok;
}
void aot_images_rescan(void) {
  aot_images_seen = generation;
  aot_active_image = (AotActiveImage){0, 0, 0};
  for (uint32_t i = 0; i < image_count; i++)
    if (!aot_active_image.table && matches(&images[i]))
      aot_active_image = (AotActiveImage){images[i].base, images[i].size, images[i].table};
}
AOTBlockFunc aot_images_lookup(uint32_t pc) { return 0; }
uint32_t aot_images_block_size(uint32_t pc) { return 0; }
/* Stub tracker: the module is "located" at text_base with its sections after it. */
void aot_rso_dispatch(AOTState* s) {
  if (s->downcount <= 0) return;
  uint32_t off = s->pc - text_base;
  if (off < rso[0].text_size && rso[0].table[off >> 2]) { rso[0].table[off >> 2](s); return; }
  ++exits; exit_pc = s->pc; s->downcount = 0;
}
void aot_interpreter_single_step(AOTState* s) { ++fallbacks; s->downcount = 0; }
AOT_SLOWPATH_CC uint32_t aot_read_u32_slow(AOTState* s, uint32_t addr) {
  ++reads; return addr == data_base ? 0x11223344u : 0xbad;
}
void aot_register_game(const char* id, void (*d)(AOTState*), AOTBlockFunc (*l)(uint32_t), uint32_t v) {}
void aot_register_game_image(const char* id, const char* h) {}
void aot_register_image_block_sizes(const char* id, uint32_t (*lookup)(uint32_t)) {}
void aot_register_game_images(const char* id, const AotImageDesc* i, uint32_t n, uint32_t v) { assert(v == AOT_IMAGES_VERSION); images = i; image_count = n; }
void aot_register_game_rso_modules(const char* id, const AotRsoModuleDesc* m, uint32_t n, uint32_t v) { assert(v == AOT_IMAGES_VERSION); rso = m; rso_count = n; }
static void call_module(uint32_t module_base, uint32_t bss_base) {
  text_base = module_base + 0x100; data_base = module_base + 0x140;
  memset(rso[0].base_slots, 0, rso[0].num_sections * 4);
  rso[0].base_slots[1] = text_base; rso[0].base_slots[2] = data_base; rso[0].base_slots[3] = bss_base;
  AOTState s; memset(&s, 0, sizeof s);
  s.pc = text_base; s.msr = 0x30; s.downcount = 1000; s.spr[8] = 0x80005000;  /* LR: no code */
  fallbacks = exits = reads = 0;
  TSTE01_dispatch(&s);
  assert(fallbacks == 0 && exits == 1 && exit_pc == 0x80005000 && s.pc == 0x80005000);
  assert(s.gpr[3] == 0x11);                 /* ext_func (DOL) then the local function */
  assert(s.gpr[4] == 0x80004044u);          /* &ext_data, patched at translate time */
  assert(s.gpr[5] == 0x11223344u && reads == 1);  /* own .data through base_slots[2] */
  assert(s.gpr[6] == bss_base + 8);          /* own .bss through base_slots[3] */
  assert(s.gpr[0] == 0x80005000u && s.spr[8] == 0x80005000u);
}
int main(void) {
  assert(images && image_count == 1 && rso && rso_count == 1);
  assert(!strcmp(rso[0].name, "RSO_Test.plf") && rso[0].num_sections == 10);
  assert(rso[0].text_section == 1 && rso[0].text_size == 0x40 && rso[0].section_sizes[2] == 0x10);
  assert(rso[0].word_count >= 4);
  for (uint32_t w = 0; w < rso[0].word_count; w++) assert(rso[0].words[w].offset - 0x100u < 0x40);
  call_module(0x80b36440, 0x80b40000);
  call_module(0x90123400, 0x90200020);       /* moved: same code, new bases */
  return 0;
}
''')
        for harness_mode in (0, 1):
            run('clang', '-O2', '-std=c2x', '-Wno-deprecated-non-prototype',
                f'-DAOT_HARNESS={harness_mode}', '-I' + str(out),
                *map(str, sorted(out.glob('*.c'))), '-o', str(root / 'test'))
            run(str(root / 'test'))
        print('PASS: RSO module translation (base-relative relocs, DOL-bound externals, identity '
              'words, descriptor registration, aot_rso_dispatch)')


if __name__ == '__main__':
    main()
