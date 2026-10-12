// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// fastmem1: the AOT fastmem fault handler must recognise exactly the eight
// `_fmw` instruction forms of aot_runtime.h (register offset, UXTW, no shift)
// and nothing else. Encodings below are from the clang assembler.

#include <cstdint>

#include <gtest/gtest.h>

#include "Core/PowerPC/AOT/AotFastmem.h"

namespace
{
struct Template
{
  uint32_t insn;
  const char* text;
  uint32_t size, store, rt, rn, rm;
};

constexpr Template kTemplates[] = {
    {0x38694a83, "ldrb w3, [x20, w9, uxtw]", 1, 0, 3, 20, 9},
    {0x78624820, "ldrh w0, [x1, w2, uxtw]", 2, 0, 0, 1, 2},
    {0xb87e4a7c, "ldr w28, [x19, w30, uxtw]", 4, 0, 28, 19, 30},
    {0xf86a4907, "ldr x7, [x8, w10, uxtw]", 8, 0, 7, 8, 10},
    {0x382648a4, "strb w4, [x5, w6, uxtw]", 1, 1, 4, 5, 6},
    {0x782d498b, "strh w11, [x12, w13, uxtw]", 2, 1, 11, 12, 13},
    {0xb822483d, "str w29, [x1, w2, uxtw]", 4, 1, 29, 1, 2},
    {0xf8364abe, "str x30, [x21, w22, uxtw]", 8, 1, 30, 21, 22},
};

constexpr struct
{
  uint32_t insn;
  const char* text;
} kOthers[] = {
    {0x29400440, "ldp w0, w1, [x2]"},
    {0xb9400420, "ldr w0, [x1, #4]"},
    {0xb85fc020, "ldur w0, [x1, #-4]"},
    {0xb8626820, "ldr w0, [x1, x2]"},
    {0xb8625820, "ldr w0, [x1, w2, uxtw #2]"},
    {0xb8a24820, "ldrsw x0, [x1, w2, uxtw]"},
    {0x78e24820, "ldrsh w0, [x1, w2, uxtw]"},
    {0xbc624820, "ldr s0, [x1, w2, uxtw]"},
    {0xb862c820, "ldr w0, [x1, w2, sxtw]"},
    {0xf8a24820, "prfm pldl1keep, [x1, w2, uxtw]"},
    {0xd503201f, "nop"},
};
}  // namespace

TEST(AotFastmemDecodeTest, TheEightTemplatesDecode)
{
  for (const Template& t : kTemplates)
  {
    AotFastmemAccess a{};
    ASSERT_TRUE(aot_fastmem_decode(t.insn, &a)) << t.text;
    EXPECT_EQ(a.size, t.size) << t.text;
    EXPECT_EQ(a.store, t.store) << t.text;
    EXPECT_EQ(a.rt, t.rt) << t.text;
    EXPECT_EQ(a.rn, t.rn) << t.text;
    EXPECT_EQ(a.rm, t.rm) << t.text;
  }
}

TEST(AotFastmemDecodeTest, OtherFormsDoNotDecode)
{
  for (const auto& o : kOthers)
  {
    AotFastmemAccess a{};
    EXPECT_FALSE(aot_fastmem_decode(o.insn, &a)) << o.text;
  }
}

#if defined(__aarch64__)
// The aot_runtime.h primitives must assemble to exactly those forms.
namespace
{
#define AOT_PRIM(name, body) __attribute__((noinline)) uint64_t name(uint8_t* b, uint64_t v, uint32_t e) { body; }
AOT_PRIM(Ld8, return aot_fmw_ld8(b, e))
AOT_PRIM(Ld16, return aot_fmw_ld16(b, e))
AOT_PRIM(Ld32, return aot_fmw_ld32(b, e))
AOT_PRIM(Ld64, return aot_fmw_ld64(b, e))
AOT_PRIM(St8, aot_fmw_st8(b, static_cast<uint32_t>(v), e); return 0)
AOT_PRIM(St16, aot_fmw_st16(b, static_cast<uint32_t>(v), e); return 0)
AOT_PRIM(St32, aot_fmw_st32(b, static_cast<uint32_t>(v), e); return 0)
AOT_PRIM(St64, aot_fmw_st64(b, v, e); return 0)
#undef AOT_PRIM
}  // namespace

TEST(AotFastmemDecodeTest, RuntimePrimitivesUseTheTemplates)
{
  using Prim = uint64_t (*)(uint8_t*, uint64_t, uint32_t);
  const struct
  {
    Prim fn;
    uint32_t size, store;
  } prims[] = {{Ld8, 1, 0},  {Ld16, 2, 0}, {Ld32, 4, 0}, {Ld64, 8, 0},
               {St8, 1, 1},  {St16, 2, 1}, {St32, 4, 1}, {St64, 8, 1}};
  for (const auto& p : prims)
  {
    // The primitive's access is among the first few instructions of the function
    // (scan up to its `ret`: the primitives sit next to each other).
    const uint32_t* code = reinterpret_cast<const uint32_t*>(p.fn);
    int found = 0;
    for (int i = 0; i < 8 && code[i] != 0xd65f03c0u /* ret */; i++)
    {
      AotFastmemAccess a{};
      if (aot_fastmem_decode(code[i], &a))
      {
        found++;
        EXPECT_EQ(a.size, p.size);
        EXPECT_EQ(a.store, p.store);
      }
    }
    EXPECT_EQ(found, 1) << "size " << p.size << " store " << p.store;
  }
}
#endif
