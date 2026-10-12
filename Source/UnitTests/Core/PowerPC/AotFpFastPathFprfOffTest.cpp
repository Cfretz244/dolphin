// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The FP fast-path tests again, with the runtime header compiled in its
// JIT-parity FPSCR mode (AOT_FPRF=0, fprf1): the value forms and the memory-form
// wrappers must still match the interpreter bit for bit everywhere except the
// FPSCR status bits FPRF|FI|FR, which they no longer write. Same file, renamed
// fixture; the FPSCR-independent tests are compiled only in the exact TU.

#define AOT_FPRF 0
#define AOT_FP_TEST_FPRF_OFF_TU 1
#define AotFpFastPathTest AotFpFastPathFprfOffTest
#include "AotFpFastPathTest.cpp"

#ifdef DOLPHIN_HAS_AOT
static_assert(AOT_FPRF == 0, "aot_runtime.h was included before AOT_FPRF was set");
#endif
