// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// fastmem1: see AotFastmem.h. Dependency-free on purpose (ROM-free test).

#include "Core/PowerPC/AOT/AotFastmem.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(__APPLE__) && defined(__aarch64__)
#define AOT_FASTMEM_SUPPORTED 1
#include <mach/mach.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif
#else
#define AOT_FASTMEM_SUPPORTED 0
#endif

namespace
{
constexpr uint64_t kWindowSize = 0x1'0000'0000ull;
// A guard past the 4 GiB: `uxtw` keeps every access START inside the window,
// but an access at 0xFFFFFFF9.. would spill up to 7 bytes beyond it, and a
// page-crossing store may perform its second half even when the first faults.
constexpr uint64_t kGuardSize = 0x10000;
constexpr uint64_t kReserveSize = kWindowSize + kGuardSize;
constexpr uint32_t kSelfTestEA = 0x00001000u;

uint8_t* s_base = nullptr;
AOTState* s_state = nullptr;
AotFastmemStats s_stats{};
std::atomic<int> s_in_handler{0};
// Self-test mode: the handler supplies/records values instead of calling the
// slow helpers, so the self-test never touches emulator state.
volatile int s_selftest = 0;
volatile uint64_t s_selftest_store = 0;
volatile uint32_t s_selftest_store_size = 0;
volatile int s_selftest_faults = 0;
constexpr uint64_t kSelfTestPattern = 0x8877665544332211ull;

uint32_t RegionOf(uint32_t ea)
{
  const uint32_t top = ea >> 28;
  if (top == 0)
    return AOT_FMW_REGION_LOWMEM;
  if ((ea >> 24) == 0xC8)
    return AOT_FMW_REGION_EFB;
  if (top == 0xC || top == 0xD)
    return AOT_FMW_REGION_MMIO;
  return AOT_FMW_REGION_OTHER;
}

#if AOT_FASTMEM_SUPPORTED
struct sigaction s_old_bus{}, s_old_segv{};
bool s_handler_installed = false;

using ThreadState = __darwin_arm_thread_state64;

uint64_t GetReg(const ThreadState& ss, uint32_t r)
{
  if (r < 29)
    return ss.__x[r];
  if (r == 29)
    return reinterpret_cast<uint64_t>(__darwin_arm_thread_state64_get_fp(ss));
  if (r == 30)
    return __darwin_arm_thread_state64_get_lr(ss);
  return 0;  // zr
}

void SetReg(ThreadState& ss, uint32_t r, uint64_t v)
{
  if (r < 29)
    ss.__x[r] = v;
  else if (r == 29)
    __darwin_arm_thread_state64_set_fp(ss, reinterpret_cast<void*>(v));
  else if (r == 30)
    __darwin_arm_thread_state64_set_lr_fptr(ss, reinterpret_cast<void*>(v));
  // r == 31: a load into zr discards the value
}

uint64_t Swap(uint64_t v, uint32_t size)
{
  switch (size)
  {
  case 1:
    return v & 0xFF;
  case 2:
    return __builtin_bswap16(static_cast<uint16_t>(v));
  case 4:
    return __builtin_bswap32(static_cast<uint32_t>(v));
  default:
    return __builtin_bswap64(v);
  }
}

void WriteErr(const char* msg)
{
  ssize_t r = write(STDERR_FILENO, msg, std::strlen(msg));
  (void)r;
}

// Emulates the faulting window access. Returns false when the instruction is
// not one of ours (the caller chains to the previous handler -> crash).
bool HandleWindowFault(ThreadState& ss)
{
  const uint64_t pc = __darwin_arm_thread_state64_get_pc(ss);
  uint32_t insn;
  std::memcpy(&insn, reinterpret_cast<const void*>(pc), sizeof(insn));
  AotFastmemAccess a;
  if (!aot_fastmem_decode(insn, &a) || GetReg(ss, a.rn) != reinterpret_cast<uint64_t>(s_base))
  {
    s_stats.unhandled++;
    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "AOT fastmem: unhandled window fault pc=%#llx insn=%#010x base=%p\n",
                  static_cast<unsigned long long>(pc), insn, static_cast<void*>(s_base));
    WriteErr(msg);
    return false;
  }
  // The guest EA is the index register, NOT si_addr - base: for a page-crossing
  // access si_addr is the first faulting byte, not the start of the access.
  const uint32_t ea = static_cast<uint32_t>(GetReg(ss, a.rm));
  const uint64_t mask = a.size == 8 ? ~0ull : (1ull << (a.size * 8)) - 1;

  if (s_selftest)
  {
    s_selftest_faults = s_selftest_faults + 1;
    if (a.store)
    {
      s_selftest_store = GetReg(ss, a.rt) & mask;
      s_selftest_store_size = a.size;
    }
    else
    {
      SetReg(ss, a.rt, kSelfTestPattern & mask);
    }
  }
  else if (a.store)
  {
    // The register holds the raw (already byte-swapped, big-endian in memory)
    // value; the slow helpers take the host-order value.
    const uint64_t raw = GetReg(ss, a.rt) & mask;
    const uint64_t v = Swap(raw, a.size);
    switch (a.size)
    {
    case 1:
      aot_write_u8_slow(s_state, static_cast<uint32_t>(v), ea);
      break;
    case 2:
      aot_write_u16_slow(s_state, static_cast<uint32_t>(v), ea);
      break;
    case 4:
      aot_write_u32_slow(s_state, static_cast<uint32_t>(v), ea);
      break;
    default:
      aot_write_u64_slow(s_state, v, ea);
      break;
    }
    s_stats.writes[RegionOf(ea)]++;
  }
  else
  {
    uint64_t v;
    switch (a.size)
    {
    case 1:
      v = aot_read_u8_slow(s_state, ea);
      break;
    case 2:
      v = aot_read_u16_slow(s_state, ea);
      break;
    case 4:
      v = aot_read_u32_slow(s_state, ea);
      break;
    default:
      v = aot_read_u64_slow(s_state, ea);
      break;
    }
    // The block byte-swaps after the load: hand it the raw big-endian bytes.
    // Writing a W register zero-extends.
    SetReg(ss, a.rt, Swap(v, a.size) & mask);
    s_stats.reads[RegionOf(ea)]++;
  }
#if __has_feature(ptrauth_calls)
  __darwin_arm_thread_state64_set_pc_presigned_fptr(
      ss, ptrauth_sign_unauthenticated(reinterpret_cast<void*>(pc + 4),
                                       ptrauth_key_function_pointer, 0));
#else
  __darwin_arm_thread_state64_set_pc_fptr(ss, reinterpret_cast<void*>(pc + 4));
#endif
  return true;
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context)
{
  const uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
  uint8_t* const base = s_base;
  if (base != nullptr && addr - reinterpret_cast<uintptr_t>(base) < kReserveSize &&
      s_in_handler.exchange(1, std::memory_order_acquire) == 0)
  {
    auto* const uc = static_cast<ucontext_t*>(raw_context);
    const bool ok = HandleWindowFault(uc->uc_mcontext->__ss);
    s_in_handler.store(0, std::memory_order_release);
    if (ok)
      return;
  }
  // Not ours (or not decodable): hand it to whoever was installed before.
  const struct sigaction& old = sig == SIGBUS ? s_old_bus : s_old_segv;
  if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction != nullptr)
  {
    old.sa_sigaction(sig, info, raw_context);
    return;
  }
  if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN && old.sa_handler != nullptr)
  {
    old.sa_handler(sig);
    return;
  }
  // Default action: restore it and return; the instruction faults again and
  // the process dies with the original state (crash report intact).
  sigaction(sig, &old, nullptr);
}

bool InstallHandler()
{
  if (s_handler_installed)
    return true;
  struct sigaction sa{};
  sa.sa_sigaction = &SignalHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGBUS, &sa, &s_old_bus) != 0)
    return false;
  if (sigaction(SIGSEGV, &sa, &s_old_segv) != 0)
  {
    sigaction(SIGBUS, &s_old_bus, nullptr);
    return false;
  }
  s_handler_installed = true;
  return true;
}

void UninstallHandler()
{
  if (!s_handler_installed)
    return;
  sigaction(SIGBUS, &s_old_bus, nullptr);
  sigaction(SIGSEGV, &s_old_segv, nullptr);
  s_handler_installed = false;
}

// Maps [src, src+size) at base+ea (shared pages, not a copy) and checks the
// alias both ways with a saved/restored word. Returns 1 / 0.
int MapAlias(uint8_t* base, uint32_t ea, uint8_t* src, uint32_t size)
{
  vm_address_t target = reinterpret_cast<vm_address_t>(base) + ea;
  vm_prot_t cur = 0, max = 0;
  const kern_return_t kr =
      vm_remap(mach_task_self(), &target, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
               mach_task_self(), reinterpret_cast<vm_address_t>(src), FALSE, &cur, &max,
               VM_INHERIT_NONE);
  if (kr != KERN_SUCCESS || target != reinterpret_cast<vm_address_t>(base) + ea)
    return 0;
  if ((cur & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE))
    return 0;
  volatile uint32_t* orig = reinterpret_cast<volatile uint32_t*>(src + size - 4);
  volatile uint32_t* alias = reinterpret_cast<volatile uint32_t*>(base + ea + size - 4);
  const uint32_t saved = *orig;
  *orig = 0xA0F5A1E5u;
  const bool fwd = *alias == 0xA0F5A1E5u;
  *alias = 0x5E1A5F0Au;
  const bool back = *orig == 0x5E1A5F0Au;
  *orig = saved;
  return fwd && back ? 1 : 0;
}

// Every template once, through the window's PROT_NONE low page. Uses the
// aot_runtime.h primitives the generated code uses.
bool SelfTest(uint8_t* base, AotFastmemInit* out)
{
  s_selftest_faults = 0;
  s_selftest = 1;
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t l8 = aot_fmw_ld8(base, kSelfTestEA);
  const uint32_t l16 = aot_fmw_ld16(base, kSelfTestEA);
  const uint32_t l32 = aot_fmw_ld32(base, kSelfTestEA);
  const uint64_t l64 = aot_fmw_ld64(base, kSelfTestEA);
  bool ok = l8 == (kSelfTestPattern & 0xFF) && l16 == (kSelfTestPattern & 0xFFFF) &&
            l32 == static_cast<uint32_t>(kSelfTestPattern) && l64 == kSelfTestPattern;
  const uint64_t vals[4] = {0xA5, 0xBEEF, 0xC0FFEE11u, 0x0123456789ABCDEFull};
  aot_fmw_st8(base, static_cast<uint32_t>(vals[0]), kSelfTestEA);
  ok = ok && s_selftest_store == vals[0] && s_selftest_store_size == 1;
  aot_fmw_st16(base, static_cast<uint32_t>(vals[1]), kSelfTestEA);
  ok = ok && s_selftest_store == vals[1] && s_selftest_store_size == 2;
  aot_fmw_st32(base, static_cast<uint32_t>(vals[2]), kSelfTestEA);
  ok = ok && s_selftest_store == vals[2] && s_selftest_store_size == 4;
  aot_fmw_st64(base, vals[3], kSelfTestEA);
  ok = ok && s_selftest_store == vals[3] && s_selftest_store_size == 8;
  const auto t1 = std::chrono::steady_clock::now();
  s_selftest = 0;
  out->selftest_faults = s_selftest_faults;
  out->fault_us = std::chrono::duration<double, std::micro>(t1 - t0).count() /
                  (s_selftest_faults > 0 ? s_selftest_faults : 1);
  return ok && s_selftest_faults == 8;
}
#endif  // AOT_FASTMEM_SUPPORTED
}  // namespace

extern "C" {

int aot_fastmem_decode(uint32_t insn, AotFastmemAccess* out)
{
  // LDR/STR (register): size 111 0 00 opc 1 Rm option S 10 Rn Rt, with V=0,
  // opc<1>=0 (no signed loads, no PRFM), option=010 (UXTW), S=0.
  if ((insn & 0x3FA0FC00u) != 0x38204800u)
    return 0;
  out->size = 1u << (insn >> 30);
  out->store = ((insn >> 22) & 1) ? 0 : 1;
  out->rt = insn & 31;
  out->rn = (insn >> 5) & 31;
  out->rm = (insn >> 16) & 31;
  return 1;
}

int aot_fastmem_window_create(AOTState* s, uint8_t* ram, uint32_t ram_size, uint8_t* exram,
                              uint32_t exram_size, uint8_t* l1, uint32_t l1_size,
                              AotFastmemInit* out)
{
  std::memset(out, 0, sizeof(*out));
  for (int& a : out->alias)
    a = -1;
  aot_fastmem_window_destroy();
#if !AOT_FASTMEM_SUPPORTED
  (void)s, (void)ram, (void)ram_size, (void)exram, (void)exram_size, (void)l1, (void)l1_size;
  std::snprintf(out->error, sizeof out->error, "unsupported platform (Apple arm64 only)");
  return 0;
#else
  const auto fail = [&](const char* what) {
    std::snprintf(out->error, sizeof out->error, "%s", what);
    if (s_base)
      vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(s_base), kReserveSize);
    s_base = nullptr;
    s_state = nullptr;
    UninstallHandler();
    out->base = nullptr;
    return 0;
  };
  const vm_size_t page = vm_page_size;
  if (ram == nullptr || ram_size == 0 || ram_size % page || exram_size % page || l1_size % page)
    return fail("guest buffer missing or not page-sized");

  vm_address_t win = 0;
  out->reserve_kr = vm_allocate(mach_task_self(), &win, kReserveSize, VM_FLAGS_ANYWHERE);
  if (out->reserve_kr != KERN_SUCCESS)
    return fail("vm_allocate(4 GiB + guard) failed");
  s_base = reinterpret_cast<uint8_t*>(win);
  out->protect_kr = vm_protect(mach_task_self(), win, kReserveSize, TRUE, VM_PROT_NONE);
  if (out->protect_kr != KERN_SUCCESS)
    return fail("vm_protect(PROT_NONE) failed");

  out->alias[0] = MapAlias(s_base, 0x80000000u, ram, ram_size);
  out->alias[1] = MapAlias(s_base, 0xC0000000u, ram, ram_size);
  if (exram != nullptr && exram_size != 0)
  {
    out->alias[2] = MapAlias(s_base, 0x90000000u, exram, exram_size);
    out->alias[3] = MapAlias(s_base, 0xD0000000u, exram, exram_size);
  }
  if (l1 != nullptr && l1_size != 0)
    out->alias[4] = MapAlias(s_base, 0xE0000000u, l1, l1_size);
  for (int a : out->alias)
  {
    if (a == 0)
      return fail("vm_remap alias failed or did not verify");
  }

  s_state = s;
  if (!InstallHandler())
    return fail("sigaction(SIGBUS/SIGSEGV) failed");
  out->handler_installed = 1;
  s_stats = {};
  out->selftest_ok = SelfTest(s_base, out) ? 1 : 0;
  if (!out->selftest_ok)
    return fail("self-test did not round-trip through the fault handler");
  s_stats = {};
  out->base = s_base;
  return 1;
#endif
}

void aot_fastmem_window_destroy(void)
{
#if AOT_FASTMEM_SUPPORTED
  if (s_base)
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(s_base), kReserveSize);
  UninstallHandler();
#endif
  s_base = nullptr;
  s_state = nullptr;
}

uint8_t* aot_fastmem_window_base(void)
{
  return s_base;
}

void aot_fastmem_get_stats(AotFastmemStats* out)
{
  *out = s_stats;
}

uint64_t aot_fastmem_fault_total(void)
{
  uint64_t t = 0;
  for (int i = 0; i < AOT_FMW_REGIONS; i++)
    t += s_stats.reads[i] + s_stats.writes[i];
  return t;
}

}  // extern "C"
