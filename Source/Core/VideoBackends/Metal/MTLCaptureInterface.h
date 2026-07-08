// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// C interface for the blit-based offscreen frame capture used by embedding
// frontends (the Delta iOS app). Implemented in MTLGfx.mm; kept plain C so an
// ObjC++/ARC bridge can include it without pulling in Dolphin C++ headers.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The frontend owns the buffer; it must hold one full XFB-sized frame
// (width * height * 4 bytes) and outlive the capture session.
void Dolphin_SetFrameCaptureBuffer(uint8_t* buffer);
bool Dolphin_IsFrameReady(void);
void Dolphin_ClearFrameReady(void);

#ifdef __cplusplus
}
#endif
