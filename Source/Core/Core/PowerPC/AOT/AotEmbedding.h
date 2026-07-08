// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Constants for embedding frontends (the Delta iOS bridge) that configure
// Dolphin from ObjC++/plain-C code without including Dolphin C++ headers.

#ifdef __cplusplus
extern "C" {
#endif

// The PowerPC::CPUCore::AOT enumerator value, for Config Main.Core.CPUCore.
// Defined (and static_asserted against the enum) in AOTCore.cpp; frozen at 6
// for compatibility with deployed configs.
extern const int kDolphinCPUCoreAOT;

#ifdef __cplusplus
}
#endif
