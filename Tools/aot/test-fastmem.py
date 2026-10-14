#!/usr/bin/env python3
"""ROM-free regression for the fastmem window (fastmem1, AOT_FASTMEM=1): a trusted
multi-image library translated with AOT_FASTMEM=1 emits the `_fmw` memory helpers
(one host load/store into the runtime's 4 GiB window) and registers
AOT_GAME_FLAG_FASTMEM. The test links the generated blocks with the runtime's own
window/handler unit (Source/Core/Core/PowerPC/AOT/AotFastmem.cpp), brings the window
up over a small RAM buffer and runs one block that does
  lwz r5,0(r4); stw r5,4(r4)        RAM load/store through the window (no fault)
  lwz r6,0(r7)                      r7 = 0x00002000: unmapped -> fault -> aot_read_u32_slow
  stw r6,0(r8)                      r8 = 0xCC008000: inline pipe test -> gather pipe
  stw r5,0(r9)                      r9 = 0x00003000: unmapped -> fault -> aot_write_u32_slow
  lhz r10,2(r4); lbz r11,K(r4)      16/8-bit loads through the window
  lfd f1,8(r4); stfd f1,16(r4)      64-bit load/store through the window
  sth r5,0x3000(r8)                 0xCC00B000: inline MMIO test -> _gp -> aot_write_u16_slow
  lwz r12,0x3000(r8)                0xCC00B000: inline MMIO test -> aot_read_u32_slow (no fault)
  blr
and checks registers, RAM, the pipe and the handler's fault counters (exactly one
read and one write fault), in plain and harness builds. Before that, the Phase B
probe (aot_fastmem_probe keep=0) must bring the window up, pass the self-test and
tear it down again (window gone, previous SIGBUS/SIGSEGV dispositions back), and a
bad argument must report FAIL(args,0) without leaving anything behind.
Usage: python3 Tools/aot/test-fastmem.py /absolute/path/to/dolphin-tool
"""
import os
import pathlib
import platform
import struct
import subprocess
import sys
import tempfile

SRC_CORE = pathlib.Path(__file__).resolve().parents[2] / 'Source' / 'Core'


def run(*args, ok=True, env=None):
    p = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    if (p.returncode == 0) != ok:
        raise AssertionError(f'{args}: exit {p.returncode}\n{p.stdout}')
    return p.stdout


def code(k):
    return [0x80A40000, 0x90A40004, 0x80C70000, 0x90C80000, 0x90A90000, 0xA1440002,
            0x89640000 | k, 0xC8240008, 0xD8240010, 0xB0A83000, 0x81883000, 0x4E800020]


N = len(code(0))


def make_dol(k):
    data = bytearray(0x100 + 4 * N)
    for offset, word in [(0, 0x100), (0x48, 0x80004000), (0x90, 4 * N), (0xE0, 0x80004000)]:
        struct.pack_into('>I', data, offset, word)
    struct.pack_into(f'>{N}I', data, 0x100, *code(k))
    return data


HARNESS = r'''
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "aot_images.h"
#include "AotFastmemTest.h"
#define STARTS(str, lit) (strncmp((str), (lit), strlen(lit)) == 0)
extern AOT_BLOCK_CC void TSTE01_dispatch(AOTState*);
static uint32_t words[NWORDS];
static uint64_t generation = 1;
static const AotImageDesc* images; static uint32_t image_count; static uint32_t images_version;
static int fallbacks, flags_registered, slow_r32, slow_w32, slow_w16, other_slow, captures;
static uint32_t last_w32_val, last_w32_addr, last_w16_val, last_w16_addr;
int aot_single_block_mode;
AotFastMem aot_fast_mem;
AotActiveImage aot_active_image;
const uint64_t* aot_images_generation = &generation;
uint64_t aot_images_seen;
static uint32_t rd(uint32_t a) { return a - 0x80004000u < 4 * NWORDS ? words[(a - 0x80004000u) / 4] : 0; }
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
uint32_t aot_images_block_size(uint32_t pc) { return aot_images_lookup(pc) ? NWORDS : 0; }
AOT_BLOCK_CC void aot_interpreter_single_step(AOTState* s) { ++fallbacks; s->downcount = 0; }
void aot_register_game(const char* id, AOTBlockFunc d, AOTBlockFunc (*l)(uint32_t), uint32_t v) {}
void aot_register_game_image(const char* id, const char* h) {}
void aot_register_game_flags(const char* id, uint32_t f) { assert(f == AOT_GAME_FLAG_FASTMEM); ++flags_registered; }
void aot_register_image_block_sizes(const char* id, uint32_t (*lookup)(uint32_t)) {}
void aot_register_game_images(const char* id, const AotImageDesc* i, uint32_t n, uint32_t v) { images = i; image_count = n; images_version = v; }
/* Slow helpers: only the two unmapped accesses (via the fault handler) and the
   non-pipe MMIO store (via the inline test) may reach them. */
AOT_SLOWPATH_CC uint32_t aot_read_u32_slow(AOTState* s, uint32_t a) {
  ++slow_r32; assert(a == 0x00002000u || a == 0xCC00B000u); return a == 0x00002000u ? 0xDEADBEEFu : 0xC0DE0001u;
}
AOT_SLOWPATH_CC uint32_t aot_read_u8_slow(AOTState* s, uint32_t a) { ++other_slow; return 0; }
AOT_SLOWPATH_CC uint32_t aot_read_u16_slow(AOTState* s, uint32_t a) { ++other_slow; return 0; }
AOT_SLOWPATH_CC uint64_t aot_read_u64_slow(AOTState* s, uint32_t a) { ++other_slow; return 0; }
AOT_SLOWPATH_CC void aot_write_u32_slow(AOTState* s, uint32_t v, uint32_t a) { ++slow_w32; last_w32_val = v; last_w32_addr = a; }
AOT_SLOWPATH_CC void aot_write_u16_slow(AOTState* s, uint32_t v, uint32_t a) { ++slow_w16; last_w16_val = v; last_w16_addr = a; }
AOT_SLOWPATH_CC void aot_write_u16_br_slow(AOTState* s, uint32_t v, uint32_t a) { ++other_slow; }
AOT_SLOWPATH_CC void aot_write_u8_slow(AOTState* s, uint32_t v, uint32_t a) { ++other_slow; }
AOT_SLOWPATH_CC void aot_write_u64_slow(AOTState* s, uint64_t v, uint32_t a) { ++other_slow; }
int aot_check_fpu(AOTState* s, uint32_t pc) { assert(0); return 0; }
AOT_SLOWPATH_CC void aot_gp_flush(AOTState* s) { assert(0); }
void aot_gp_capture(uint32_t physical, uint32_t val, uint32_t size) {
  ++captures; assert(physical == 0x0C008000u && val == 0xDEADBEEFu && size == 4);
}
static uint32_t dbat[1u << 15];
static uint8_t pipe[64];
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
int main(void) {
  assert(images && image_count == 2 && flags_registered == 1);
  uint8_t* ram = 0; assert(posix_memalign((void**)&ram, 16384, 0x8000) == 0); memset(ram, 0, 0x8000);
  static AOTState s;
  /* Phase B probe, teardown mode: up, self-test, down; dispositions restored. */
  struct sigaction bus0, segv0, now;
  assert(sigaction(SIGBUS, 0, &bus0) == 0 && sigaction(SIGSEGV, 0, &segv0) == 0);
  AotFastmemProbeResult pr;
  char line[256];
  for (int round = 0; round < 2; round++) {
    assert(aot_fastmem_probe(&s, ram, 0x8000, 0, 0, 0, 0, 0, &pr));
    assert(pr.failed_step == AOT_FMP_STEP_NONE && pr.window_addr != 0 && pr.init.base == 0);
    assert(aot_fastmem_window_base() == 0);
    assert(pr.init.alias[0] == 1 && pr.init.alias[1] == 1 && pr.init.alias[2] == -1 && pr.init.alias[4] == -1);
    assert(pr.init.handler_installed && pr.init.selftest_ok && pr.init.selftest_faults == 8);
    assert(pr.teardown == 1 && pr.dealloc_kr == 0 && pr.handler_restored == 1 && !pr.keep);
    assert(sigaction(SIGBUS, 0, &now) == 0 && now.sa_handler == bus0.sa_handler);
    assert(sigaction(SIGSEGV, 0, &now) == 0 && now.sa_handler == segv0.sa_handler);
    aot_fastmem_probe_format(&pr, line, sizeof line);
    assert(STARTS(line, "window=UP aliases=2/2 handler=OK selftest=8/8 fault_us="));
    assert(strstr(line, " mode=teardown teardown=OK total_us=") != 0);
  }
  /* A failed step reports itself and leaves nothing behind. */
  assert(!aot_fastmem_probe(&s, ram, 0x1001, 0, 0, 0, 0, 0, &pr));
  assert(pr.failed_step == AOT_FMP_STEP_ARGS && pr.window_addr == 0 && aot_fastmem_window_base() == 0);
  aot_fastmem_probe_format(&pr, line, sizeof line);
  assert(STARTS(line, "window=FAIL(args,0) aliases=0/0 handler=- selftest=0/8 "));
  AotFastmemInit r;
  assert(aot_fastmem_window_create(&s, ram, 0x8000, 0, 0, 0, 0, &r));
  assert(r.base && r.alias[0] == 1 && r.alias[1] == 1 && r.alias[2] == -1 && r.alias[4] == -1);
  assert(r.handler_installed && r.selftest_ok && r.selftest_faults == 8);
  /* the uncached mirror sees the same bytes */
  ram[0x10] = 0x5A; assert(r.base[0xC0000010u] == 0x5A && r.base[0x80000010u] == 0x5A); ram[0x10] = 0;
  aot_fast_mem.base = r.base;
  dbat[0xCC008000u >> 17] = 0x0C000000u | AOT_BAT_MAPPED_BIT;
  aot_fast_mem.dbat = dbat;
  static const uint32_t c1[NWORDS] = {CODE1}, c2[NWORDS] = {CODE2};
  for (int img = 1; img <= 2; img++) {
    memcpy(words, img == 1 ? c1 : c2, sizeof words); ++generation;
    const uint32_t k = img == 1 ? 1 : 3;
    static const uint8_t data[24] = {0x11,0x22,0x33,0x44, 0,0,0,0, 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
    memset(ram + 0x1000, 0, 32); memcpy(ram + 0x1000, data, 16); memset(pipe, 0, sizeof pipe);
    fallbacks = slow_r32 = slow_w32 = slow_w16 = other_slow = captures = 0;
    memset(&s, 0, sizeof s);
    s.pc = 0x80004000u; s.downcount = 1000; s.msr = AOT_MSR_FP | AOT_MSR_DR;
    s.spr[8] = 0x80005000u;  /* blr target: no block -> interpreter */
    s.gpr[4] = 0x80001000u; s.gpr[7] = 0x00002000u; s.gpr[8] = 0xCC008000u; s.gpr[9] = 0x00003000u;
    s.gather_pipe_ptr = pipe; s.gather_pipe_base_ptr = pipe;
    AotFastmemStats st0; aot_fastmem_get_stats(&st0);
    TSTE01_dispatch(&s);
    AotFastmemStats st; aot_fastmem_get_stats(&st);
    assert(fallbacks == 1 && s.pc == 0x80005000u);
    assert(s.gpr[5] == 0x11223344u && be32(ram + 0x1004) == 0x11223344u);  /* RAM via the window */
    assert(s.gpr[6] == 0xDEADBEEFu && slow_r32 == 2);  /* read fault -> slow, + the MMIO load below */
    assert(s.gpr[12] == 0xC0DE0001u);                    /* MMIO load: inline test -> slow, no fault */
    assert(be32(pipe) == 0xDEADBEEFu && (uint8_t*)s.gather_pipe_ptr == pipe + 4);  /* pipe arm */
    assert(slow_w32 == 1 && last_w32_val == 0x11223344u && last_w32_addr == 0x00003000u);  /* write fault */
    assert(s.gpr[10] == 0x3344u && s.gpr[11] == data[k]);
    assert(s.ps[1].ps0 == 0x0123456789ABCDEFull && memcmp(ram + 0x1010, data + 8, 8) == 0);
    assert(slow_w16 == 1 && (last_w16_val & 0xFFFFu) == 0x3344u && last_w16_addr == 0xCC00B000u);  /* MMIO test */
    assert(other_slow == 0);
    assert(st.reads[AOT_FMW_REGION_LOWMEM] - st0.reads[AOT_FMW_REGION_LOWMEM] == 1);
    assert(st.writes[AOT_FMW_REGION_LOWMEM] - st0.writes[AOT_FMW_REGION_LOWMEM] == 1);
    assert(aot_fastmem_fault_total() == 2u * img && st.unhandled == 0);
#if AOT_HARNESS
    assert(captures == 2);
#else
    assert(captures == 0);
#endif
  }
  aot_fastmem_window_destroy();
  assert(aot_fastmem_window_base() == 0);
  assert(sigaction(SIGSEGV, 0, &now) == 0 && now.sa_handler == segv0.sa_handler);
  return 0;
}
'''


def main():
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        print('SKIP: the fastmem window is Apple arm64 only')
        return
    tool = str(pathlib.Path(sys.argv[1]).resolve())
    env = {k: v for k, v in os.environ.items() if k not in ('AOT_NO_SUPERBLOCKS', 'AOT_FPRF')}
    env['AOT_FASTMEM'] = '1'
    with tempfile.TemporaryDirectory(prefix='aot-fastmem-') as temp:
        root = pathlib.Path(temp)
        iso = bytearray(0x4000)
        iso[:6] = b'TSTE01'
        size = 0x100 + 4 * N
        for offset, value in [(0x1C, 0xC2339F3D), (0x420, 0x1000), (0x424, 0x2000),
                              (0x428, 32), (0x42C, 32)]:
            struct.pack_into('>I', iso, offset, value)
        iso[0x1000:0x1000 + size] = make_dol(1)
        iso[0x3000:0x3000 + size] = make_dol(3)
        struct.pack_into('>6I', iso, 0x2000, 0x01000000, 0, 2, 0, 0x3000, size)
        iso[0x2018:0x2020] = b'alt.dol\0'
        disc = root / 'test.iso'
        disc.write_bytes(iso)
        trace = bytearray(struct.pack('<7I', 0x54485044, 4, 1, 0, 0, 0, 36))
        trace += struct.pack('<2I', 0x80004000, 4 * N)
        trace += struct.pack('<2I', 0x80004000, 2)
        for i in (1, 3):
            trace += struct.pack(f'<3IQ{N}I', N, i, 1, i, *code(i))
        (root / 'test.dpht').write_bytes(trace)
        cfg = root / 'test.db'
        run(tool, 'cfg', '--iso', str(disc), '--trace', str(root / 'test.dpht'), '--dol-images',
            '--output', str(cfg), env=env)
        out = root / 'generated'
        out.mkdir()
        (out / 'translate.log').touch()
        log = run(tool, 'translate', '--iso', str(disc), '--cfg', str(cfg), '--output', str(out),
                  env=env)
        assert 'Memory ops (AOT_FASTMEM): 1' in log, log
        assert '-DAOT_FASTMEM=1' in (out / 'aot_flags.txt').read_text()
        text = ''.join(p.read_text() for p in out.glob('*blocks*.c'))
        for h in ('aot_read_u32_fmw(', 'aot_write_u32_fmw(', 'aot_read_u16_fmw(', 'aot_read_u8_fmw(',
                  'aot_read_u64_fmw(', 'aot_write_u64_fmw(', 'aot_write_u16_fmw('):
            assert h in text, h
        assert '_fm(s,' not in text, text
        dispatch = ''.join(p.read_text() for p in out.glob('*.c') if 'blocks' not in p.name)
        assert dispatch.count('aot_register_game_flags("TSTE01"') == 1, dispatch
        (out / 'AotFastmemTest.h').write_text('#include "AotFastmem.h"\n')
        (out / 'test.c').write_text(HARNESS.replace('NWORDS', str(N))
                                    .replace('CODE1', ','.join(map(hex, code(1))))
                                    .replace('CODE2', ','.join(map(hex, code(3)))))
        # The runtime unit, compiled against the checked-in header tree.
        fm_obj = root / 'AotFastmem.o'
        run('clang++', '-std=c++20', '-O2', '-c', '-I' + str(SRC_CORE),
            str(SRC_CORE / 'Core' / 'PowerPC' / 'AOT' / 'AotFastmem.cpp'), '-o', str(fm_obj))
        hdr = out / 'AotFastmem.h'
        hdr.write_text((SRC_CORE / 'Core' / 'PowerPC' / 'AOT' / 'AotFastmem.h').read_text()
                       .replace('"Core/PowerPC/AOT/aot_runtime.h"', '"aot_runtime.h"'))
        for harness_mode in (0, 1):
            run('clang', '-O2', '-std=c2x', '-Wno-deprecated-non-prototype',
                f'-DAOT_HARNESS={harness_mode}', '-DAOT_FASTMEM=1', '-I' + str(out),
                *map(str, sorted(out.glob('*.c'))), str(fm_obj), '-lc++', '-o', str(root / 'test'))
            run(str(root / 'test'))
        print('PASS: fastmem window: _fmw emission + flag registration, RAM through the window, '
              'read/write faults -> slow helpers (handler counters), inline pipe + MMIO store/load tests, '
              '8/16/32/64-bit forms, harness and plain builds; probe teardown restores the process state')


if __name__ == '__main__':
    main()
